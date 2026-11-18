#pragma once

#include <Disks/tests/cas_test_helpers.h>

#include <Poco/Exception.h>
#include <Common/SipHash.h>

#include <optional>

namespace DB::Cas::tests
{

/// Models an ETag store faithfully enough for the staged-envelope regressions: a blob token is a
/// deterministic digest of the complete object bytes, so copying the same staging object again would
/// reproduce the same token. Each script injects a different ambiguity transition; `None` is an honest store.
class EtagFaithfulPublicationBackend : public DB::Cas::tests::CountingBackend
{
public:
    enum class FaultScript : uint8_t
    {
        None,
        CopyLandsThenCondemned,
        CopyLandsThenDeletedBeforeAbsentRetry,
        FirstCondemnedStreamLandsThenDeleted,
    };

    explicit EtagFaithfulPublicationBackend(FaultScript script_) : script(script_) {}

    std::optional<DB::Cas::Backend::RawMeta> head(const String & key, DB::Cas::TransportAccess & access) override
    {
        const auto journaled = DB::Cas::tests::CountingBackend::head(key, access);
        return withContentValue(key, journaled, access);
    }

    DB::Cas::Backend::RawRemoval remove(const String & key, const String & expected_value,
                                        DB::Cas::TransportAccess & access) override
    {
        if (!isBlobBodyKey(key))
            return DB::Cas::tests::CountingBackend::remove(key, expected_value, access);

        const auto raw = DB::Cas::InMemoryBackend::head(key, access);
        if (!raw)
            return DB::Cas::Backend::RawRemoval::Gone;
        if (withContentValue(key, raw, access)->value != expected_value)
            return DB::Cas::Backend::RawRemoval::Mismatch;
        return DB::Cas::tests::CountingBackend::remove(key, raw->value, access);
    }

    /// Replaces the token of a blob body with its content hash. Reads the store directly so the journal
    /// keeps only the caller's requests.
    std::optional<DB::Cas::Backend::RawMeta> withContentValue(
        const String & key, std::optional<DB::Cas::Backend::RawMeta> result, DB::Cas::TransportAccess & access)
    {
        if (result && isBlobBodyKey(key))
        {
            const auto body = DB::Cas::InMemoryBackend::read(key, access);
            chassert(body.has_value());
            result->value = sipHash128String(body->bytes);
        }
        return result;
    }

    void publish(const DB::Cas::BlobPublishRequest & request, DB::Cas::TransportAccess & access) override
    {
        const bool is_copy = std::holds_alternative<DB::Cas::VerbatimStagedBlobPublication>(request.publication);
        if (is_copy)
            ++copy_publications;
        else
            ++streaming_publications;

        if (!fault_fired
            && ((script == FaultScript::CopyLandsThenCondemned && is_copy)
                || (script == FaultScript::CopyLandsThenDeletedBeforeAbsentRetry && is_copy)
                || (script == FaultScript::FirstCondemnedStreamLandsThenDeleted && !is_copy)))
        {
            fault_fired = true;
            DB::Cas::tests::CountingBackend::publish(request, access);
            queued_delete_token = head(request.destination_key, access)->value;
            /// The test wants to replay this exact captured value later as a delete precondition, to
            /// prove a retag defeats it. `Etag` is never constructible from a raw string, so the only
            /// way to hold a replayable one is to mint it -- through a nested admitted operation, over
            /// this same backend instance -- at the exact moment the raw value above was observed.
            {
                DB::Cas::tests::OperationForTest mint_op(*this);
                const auto meta = (*mint_op).head(request.destination_key, DB::Cas::Retry::once());
                if (meta)
                    queued_delete_etag = meta->etag;
            }

            if (script != FaultScript::CopyLandsThenCondemned)
                first_delete = remove(request.destination_key, queued_delete_token, access);

            throw Poco::TimeoutException("ETag-faithful staged publication response lost");
        }

        DB::Cas::tests::CountingBackend::publish(request, access);
    }

    FaultScript script;
    bool fault_fired = false;
    size_t copy_publications = 0;
    size_t streaming_publications = 0;
    String queued_delete_token;
    std::optional<DB::Cas::Etag> queued_delete_etag;
    DB::Cas::Backend::RawRemoval first_delete{};

private:
    static bool isBlobBodyKey(const String & key)
    {
        return key.find("/blobs/") != String::npos && !key.ends_with(".meta");
    }
};

}
