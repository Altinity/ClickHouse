#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Formats/FormatSettings.h>
#include <IO/ReadBuffer.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>
#include <Common/Exception.h>
#include <Poco/Dynamic/Var.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <algorithm>
#include <initializer_list>
#include <map>
#include <string_view>

namespace DB
{
namespace ErrorCodes
{
    extern const int ATTEMPT_TO_READ_AFTER_EOF;
    extern const int CANNOT_READ_ALL_DATA;
    extern const int CORRUPTED_DATA;
    extern const int NOT_IMPLEMENTED;
    extern const int UNKNOWN_FORMAT_VERSION;
}
}

namespace DB::Cas
{

/// Encoding split (spec §4, decision 2026-06-11). Objects whose bytes are identity (the CHCA
/// envelope, the canonical tree payload, blob/pack payloads) are BINARY: the codecs use the
/// standard IO helpers — `writeBinaryLittleEndian` / `readBinaryLittleEndian` over `WriteBuffer` /
/// `ReadBuffer` — for every field; little-endian, byte-exact (it IS pool format v2). Every
/// NON-HASHED metadata object (root manifests, gc/state, retired sets, heartbeats, _pool_meta,
/// checkpoints, outcomes) is STRICT JSON: a top-level object carrying `format` + `version`,
/// fail-closed parsing (wrong format / unknown key / missing key / wrong type / malformed document
/// => CORRUPTED_DATA; newer version => UNKNOWN_FORMAT_VERSION) via the helpers in the second half of this
/// file. These objects are the operational surface a human inspects with plain S3 tools during an
/// incident; they are never hashed, so canonical byte stability is not required.

/// ---------------------------------------------------------------------------------------------
/// On-disk UInt128 wire forms. CAS serializes a 128-bit hash in two distinct non-hex byte orders,
/// and BOTH are FROZEN — changing the bytes breaks every object already written. These named, typed
/// helpers exist so a 128-bit (de)serialization can never be mis-paired with the wrong order: a site
/// asks for the order it means by name instead of open-coding it. (The lowercase-hex form lives in
/// `CasIds.h` as `u128ToHex` / `hexToU128` and is out of scope here.)
///
/// LE binary form — used by the hashed/identity codecs (envelope header fields, the canonical tree
/// payload, the gc snapshot body): exactly `writeBinaryLittleEndian` / `readBinaryLittleEndian`.
inline void writeU128LE(WriteBuffer & out, const UInt128 & v) { writeBinaryLittleEndian(v, out); }
inline UInt128 readU128LE(ReadBuffer & in) { UInt128 v; readBinaryLittleEndian(v, in); return v; }

/// BE 16-byte form — used by the root-shard manifest's protobuf `bytes` fields (`tree_id`,
/// `tree_hash`, `file_hash`, `pack_hash`). Body copied VERBATIM from the former hand-rolled
/// `u128ToBytes` / `u128FromBytes` in `CasRootShardCodec.cpp` so the bytes are unchanged.
inline std::string u128ToBytesBE(const UInt128 & v)
{
    std::string out(16, '\0');
    for (int i = 0; i < 16; ++i)
        out[i] = static_cast<char>(static_cast<UInt8>(v >> (8 * (15 - i))));
    return out;
}

inline UInt128 u128FromBytesBE(const std::string & b, std::string_view what)
{
    if (b.size() != 16)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: tree_id must be 16 bytes, got {}", what, b.size());
    UInt128 v = 0;
    for (int i = 0; i < 16; ++i)
        v = (v << 8) | static_cast<UInt8>(b[i]);
    return v;
}

/// Read exactly `n` raw bytes. The bounds check MUST precede the allocation: `n` typically comes
/// from a length field just read off the wire, so on corrupted input it can be huge (a u32 field
/// admits 4 GiB) — allocating first would mean a multi-GiB transient allocation, which under a
/// memory tracker surfaces as MEMORY_LIMIT_EXCEEDED instead of the pinned CORRUPTED_DATA.
/// Comparing against `available` as the exact remainder is valid because all CAS codec decoding
/// reads from `ReadBufferFromMemory`: the whole object is in memory, so `available` is exactly
/// the number of bytes left.
inline String readFixedBytes(ReadBuffer & in, size_t n)
{
    if (n > in.available())
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS codec: truncated encoded data: need {} bytes, {} available", n, in.available());
    String s(n, '\0');
    in.readStrict(s.data(), n);
    return s;
}

/// Decode-boundary guard. The codecs parse fully materialized objects, so running out of bytes is
/// data corruption, not an IO condition: translate the standard reading errors into CORRUPTED_DATA —
/// the code the protocol layers and tests pin for truncated persisted objects.
template <typename F>
auto decodeGuarded(std::string_view what, F && f)
{
    try
    {
        return f();
    }
    catch (const Exception & e)
    {
        /// This translation is only safe because the guarded lambdas read exclusively from in-memory
        /// buffers — no real IO inside, so these codes cannot mean a transient read failure.
        if (e.code() == ErrorCodes::CANNOT_READ_ALL_DATA || e.code() == ErrorCodes::ATTEMPT_TO_READ_AFTER_EOF)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: truncated encoded data ({})", what, e.message());
        throw;
    }
}

/// ---------------------------------------------------------------------------------------------
/// Strict JSON helpers for the non-hashed metadata codecs (see the encoding-split note above).
/// Encode is hand-written (deterministic key order = insertion order, compact); decode goes
/// through `Poco::JSON::Parser` with the fail-closed extraction helpers below.
/// ---------------------------------------------------------------------------------------------

/// JSON string writer for the metadata codecs: standard JSON escaping, forward slashes kept
/// verbatim (part names and tokens stay grep-able in an incident).
inline void writeJsonString(std::string_view s, WriteBuffer & out)
{
    static const FormatSettings settings = []
    {
        FormatSettings fs;
        fs.json.escape_forward_slashes = false;
        return fs;
    }();
    writeJSONString(s, out, settings);
}

/// Writes `"key":` — the key is a literal from the codec, never attacker-controlled.
inline void writeJsonKey(WriteBuffer & out, std::string_view key)
{
    writeJsonString(key, out);
    writeChar(':', out);
}

/// Decode-boundary guard for the JSON codecs: translates `Poco::Exception` (parser errors) and any
/// bad-cast/conversion error into CORRUPTED_DATA. Already-classified `DB::Exception`s
/// (CORRUPTED_DATA / UNKNOWN_FORMAT_VERSION from the helpers below) pass through unchanged.
template <typename F>
auto decodeJsonGuarded(std::string_view what, F && f)
{
    try
    {
        return f();
    }
    catch (const Exception &)
    {
        throw;
    }
    catch (const Poco::Exception & e)
    {
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: malformed JSON ({})", what, e.displayText());
    }
    catch (const std::exception & e)
    {
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: malformed JSON ({})", what, e.what());
    }
}

/// Parses `data` and requires the top-level value to be a JSON object.
inline Poco::JSON::Object::Ptr requireObject(std::string_view data, std::string_view what)
{
    Poco::Dynamic::Var parsed;
    try
    {
        Poco::JSON::Parser parser;
        parsed = parser.parse(String(data));
    }
    catch (const Poco::Exception & e)
    {
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: malformed JSON ({})", what, e.displayText());
    }
    if (parsed.type() != typeid(Poco::JSON::Object::Ptr))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: top-level JSON value is not an object", what);
    return parsed.extract<Poco::JSON::Object::Ptr>();
}

/// Fail-closed key lookup: a missing key is corruption.
inline Poco::Dynamic::Var requireKey(const Poco::JSON::Object & obj, std::string_view key, std::string_view what)
{
    const String key_str(key);
    if (!obj.has(key_str))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: missing key '{}'", what, key);
    return obj.get(key_str);
}

inline String requireString(const Poco::JSON::Object & obj, std::string_view key, std::string_view what)
{
    const auto var = requireKey(obj, key, what);
    if (var.type() != typeid(String))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: key '{}' must be a string", what, key);
    return var.extract<String>();
}

/// Strict unsigned integer from an already-extracted value (map iteration, where the keys are
/// data, not codec literals): rejects strings, booleans, floats and negatives. Poco parses JSON
/// integers as Int64, so values above INT64_MAX are out of scope BY DESIGN — every counter/size in
/// these formats stays far below 2^53 (the JSON-number interop bound) anyway.
inline uint64_t requireU64Var(const Poco::Dynamic::Var & var, std::string_view key, std::string_view what)
{
    if (var.isBoolean() || !var.isInteger())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: key '{}' must be a non-negative integer", what, key);
    const Int64 value = var.convert<Int64>();
    if (value < 0)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: key '{}' must be a non-negative integer", what, key);
    return static_cast<uint64_t>(value);
}

/// Strict unsigned integer at a known key.
inline uint64_t requireU64(const Poco::JSON::Object & obj, std::string_view key, std::string_view what)
{
    return requireU64Var(requireKey(obj, key, what), key, what);
}

inline Poco::JSON::Object::Ptr requireObject(const Poco::JSON::Object & obj, std::string_view key, std::string_view what)
{
    const auto var = requireKey(obj, key, what);
    if (var.type() != typeid(Poco::JSON::Object::Ptr))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: key '{}' must be an object", what, key);
    return var.extract<Poco::JSON::Object::Ptr>();
}

inline Poco::JSON::Array::Ptr requireArray(const Poco::JSON::Object & obj, std::string_view key, std::string_view what)
{
    const auto var = requireKey(obj, key, what);
    if (var.type() != typeid(Poco::JSON::Array::Ptr))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: key '{}' must be an array", what, key);
    return var.extract<Poco::JSON::Array::Ptr>();
}

/// Array element that must be an object (journal records, retired entries).
inline Poco::JSON::Object::Ptr requireObjectAt(const Poco::JSON::Array & arr, size_t i, std::string_view what)
{
    const auto var = arr.get(static_cast<unsigned int>(i));
    if (var.type() != typeid(Poco::JSON::Object::Ptr))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: array element {} must be an object", what, i);
    return var.extract<Poco::JSON::Object::Ptr>();
}

/// A nested object whose values are all strings (e.g. a ref's `mutable_files`).
inline std::map<String, String> requireStringMap(const Poco::JSON::Object & obj, std::string_view key, std::string_view what)
{
    const auto nested = requireObject(obj, key, what);
    std::map<String, String> result;
    for (const auto & [k, v] : *nested)
    {
        if (v.type() != typeid(String))
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS {}: value of '{}' inside '{}' must be a string", what, k, key);
        result[k] = v.extract<String>();
    }
    return result;
}

/// A 32-lowercase-hex-char hash string; `hexToU128`'s BAD_ARGUMENTS is translated to the pinned
/// CORRUPTED_DATA here (junk hex inside a persisted object is corruption, not a caller mistake).
inline UInt128 requireHash(const Poco::JSON::Object & obj, std::string_view key, std::string_view what)
{
    const String hex = requireString(obj, key, what);
    try
    {
        return hexToU128(hex);
    }
    catch (const Exception & e)
    {
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS {}: key '{}' is not a valid hash ({})", what, key, e.message());
    }
}

/// Fail-closed: any key outside `allowed` is corruption.
inline void checkNoUnknownKeys(
    const Poco::JSON::Object & obj, std::initializer_list<std::string_view> allowed, std::string_view what)
{
    for (const auto & [key, value] : obj)
    {
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: unknown key '{}'", what, key);
    }
}

/// Fail-closed gate for "this object was written by a newer version than this build understands".
/// `what` names the object in the message. Throws UNKNOWN_FORMAT_VERSION (the canonical "newer-writer
/// on-disk format" code) so an operator can tell a future-version object from an unsupported operation.
inline void checkVersion(uint32_t current, uint32_t seen, std::string_view what)
{
    if (seen > current)
        throw Exception(ErrorCodes::UNKNOWN_FORMAT_VERSION,
            "CAS {}: on-disk version {} is newer than this build supports (max {})", what, seen, current);
}

/// Common decode prologue for every non-hashed metadata object: parse the document (malformed =>
/// CORRUPTED_DATA), require `format` == expected (else CORRUPTED_DATA), require an integer
/// `version`; a version above `current_version` => UNKNOWN_FORMAT_VERSION (fail closed on the future,
/// never misreported as corruption), any other unexpected version => CORRUPTED_DATA.
inline Poco::JSON::Object::Ptr parseJsonDocument(
    std::string_view data, std::string_view expected_format, uint64_t current_version, std::string_view what)
{
    auto obj = requireObject(data, what);
    const String format = requireString(*obj, "format", what);
    if (format != expected_format)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS {}: unexpected format '{}' (expected '{}')", what, format, expected_format);
    const uint64_t version = requireU64(*obj, "version", what);
    checkVersion(static_cast<uint32_t>(current_version), static_cast<uint32_t>(version), what);
    if (version != current_version)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: invalid version {}", what, version);
    return obj;
}

}
