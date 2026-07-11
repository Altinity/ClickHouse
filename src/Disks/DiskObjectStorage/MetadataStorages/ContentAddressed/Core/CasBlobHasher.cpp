#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobHasher.h>

#include <IO/BufferWithOwnMemory.h>
#include <IO/HashingReadBuffer.h>
#include <IO/HashingWriteBuffer.h>
#include <IO/ReadBufferFromMemory.h>
#include <Common/Exception.h>
#include <base/hex.h>

/// `XXH_INLINE_ALL` renames every public symbol under the `XXH_INLINE_` prefix (`XXH_NAMESPACE`) and
/// makes the whole library a header-only, static-inline implementation local to THIS translation
/// unit -- no link dependency on the separately-compiled `ch_contrib::xxHash` object. This file is
/// part of the `dbms` target (not `clickhouse_functions_obj`, which gets the flag via its own
/// `target_link_libraries(... ch_contrib::xxHash)`), so the macro is defined locally here, same
/// effect, same prefixed names as `Functions/FunctionsHashing.h` uses.
/// xxHash is included through this wrapper (which marks it a system header) to suppress the vendored-C
/// warnings from lz4's shadowing copy under `-Werror -Weverything`. See `CasXXH3.h`.
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasXXH3.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int NOT_IMPLEMENTED;
    extern const int LOGICAL_ERROR;
}
}

namespace DB::Cas
{

std::string_view blobHashAlgoName(BlobHashAlgo algo)
{
    switch (algo)
    {
        case BlobHashAlgo::CityHash128:
            return "ch128";
        case BlobHashAlgo::XXH3_128:
            return "xxh3";
        case BlobHashAlgo::Sha256:
            return "sha256";
    }
    throw Exception(ErrorCodes::BAD_ARGUMENTS, "blobHashAlgoName: unknown BlobHashAlgo {}", static_cast<int>(algo));
}

BlobHashAlgo parseBlobHashAlgo(std::string_view config_value)
{
    if (config_value == "cityhash128")
        return BlobHashAlgo::CityHash128;
    if (config_value == "xxh3-128")
        return BlobHashAlgo::XXH3_128;
    if (config_value == "sha256")
        return BlobHashAlgo::Sha256;

    throw Exception(ErrorCodes::BAD_ARGUMENTS,
        "parseBlobHashAlgo: unknown blob_hash config value '{}' (expected one of "
        "cityhash128|xxh3-128|sha256)", config_value);
}

namespace
{

/// Thin adapter over the existing `HashingWriteBuffer` so `CityHash128` blob hashes stay
/// byte-identical to today. Bytes written to `*this` alias directly into `hashing`'s own buffer
/// (the same zero-copy trick `HashingWriteBuffer` itself uses against its nested sink), so this
/// adds no extra copy and no change to the chunked `CityHash128WithSeed` chaining.
class CityHash128BlobHashingWriteBuffer : public IHashingWriteBuffer
{
public:
    explicit CityHash128BlobHashingWriteBuffer(WriteBuffer & sink)
        : IHashingWriteBuffer()
        , hashing(sink)
    {
        working_buffer = hashing.buffer();
        pos = working_buffer.begin();
    }

    void sync() override
    {
        hashing.sync();
    }

    String getHashHex() override
    {
        next();
        return getHexUIntLowercase(hashing.getHash());
    }

private:
    void nextImpl() override
    {
        hashing.position() = pos;
        hashing.next();
        working_buffer = hashing.buffer();
    }

    void finalizeImpl() override
    {
        next();
        hashing.finalize();
    }

    void cancelImpl() noexcept override
    {
        hashing.cancel();
    }

    HashingWriteBuffer hashing;
};

/// A hash-and-passthrough buffer over the xxhash library's streaming `XXH3_128bits` state. Unlike
/// `CityHash128`, xxh3's streaming digest is defined to agree with its one-shot digest, so there is
/// no chunked-convention to preserve -- this just needs to feed every byte to the streaming state
/// (`update`) and forward the same bytes to `sink` unchanged.
class Xxh3128BlobHashingWriteBuffer : public BufferWithOwnMemory<IHashingWriteBuffer>
{
public:
    explicit Xxh3128BlobHashingWriteBuffer(WriteBuffer & sink_, size_t buf_size = DBMS_DEFAULT_HASHING_BLOCK_SIZE)
        : BufferWithOwnMemory<IHashingWriteBuffer>(buf_size)
        , sink(sink_)
    {
        if (!state.valid())
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Xxh3128BlobHashingWriteBuffer: failed to allocate the xxh3 streaming state");
    }

    void sync() override
    {
        sink.sync();
    }

    String getHashHex() override
    {
        next();
        UInt64 low = 0;
        UInt64 high = 0;
        state.digest(low, high);
        return getHexUIntLowercase(UInt128{low, high});
    }

private:
    void nextImpl() override
    {
        const size_t len = offset();
        if (!len)
            return;

        state.update(working_buffer.begin(), len);
        sink.write(working_buffer.begin(), len);
    }

    WriteBuffer & sink;
    Xxh3Streamer state;
};

}

std::unique_ptr<IHashingWriteBuffer> makeBlobHashingWriteBuffer(BlobHashAlgo algo, WriteBuffer & sink)
{
    switch (algo)
    {
        case BlobHashAlgo::CityHash128:
            return std::make_unique<CityHash128BlobHashingWriteBuffer>(sink);
        case BlobHashAlgo::XXH3_128:
            return std::make_unique<Xxh3128BlobHashingWriteBuffer>(sink);
        case BlobHashAlgo::Sha256:
            throw Exception(ErrorCodes::NOT_IMPLEMENTED,
                "makeBlobHashingWriteBuffer: sha256 blob hashing is not implemented yet (Phase 2 -- "
                "the variable-length digest refactor)");
    }
    throw Exception(ErrorCodes::BAD_ARGUMENTS, "makeBlobHashingWriteBuffer: unknown BlobHashAlgo {}", static_cast<int>(algo));
}

String blobHashHexOneShot(BlobHashAlgo algo, std::string_view bytes)
{
    switch (algo)
    {
        case BlobHashAlgo::CityHash128:
        {
            /// The POOL-WIDE content-hash convention (mirrors `poolContentHash` in `CasBuild.cpp`):
            /// the streaming `HashingReadBuffer` hash (chunked `CityHash128WithSeed`, chained per
            /// `DBMS_DEFAULT_HASHING_BLOCK_SIZE`), NOT a one-shot `CityHash128WithSeed` call (which
            /// diverges for any payload larger than one hash block).
            ReadBufferFromMemory in(bytes.data(), bytes.size());
            HashingReadBuffer hashing(in);
            hashing.ignoreAll();
            return getHexUIntLowercase(hashing.getHash());
        }
        case BlobHashAlgo::XXH3_128:
        {
            UInt64 low = 0;
            UInt64 high = 0;
            xxh3_128_oneshot(bytes.data(), bytes.size(), low, high);
            return getHexUIntLowercase(UInt128{low, high});
        }
        case BlobHashAlgo::Sha256:
            throw Exception(ErrorCodes::NOT_IMPLEMENTED,
                "blobHashHexOneShot: sha256 blob hashing is not implemented yet (Phase 2 -- the "
                "variable-length digest refactor)");
    }
    throw Exception(ErrorCodes::BAD_ARGUMENTS, "blobHashHexOneShot: unknown BlobHashAlgo {}", static_cast<int>(algo));
}

}
