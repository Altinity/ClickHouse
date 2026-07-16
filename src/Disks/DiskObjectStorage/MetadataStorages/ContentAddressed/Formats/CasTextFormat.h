#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <IO/ReadBuffer.h>
#include <IO/WriteBuffer.h>
#include <optional>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// The v3 text file shape (spec 2026-07-15): header line {"type":"cas_<x>","v":N}, body,
/// optional trailer line, optionally one zstd frame around the whole object. This header is the
/// ONLY code that knows the shape; per-object codecs are key mapping + invariants on top of it.
///
/// Canonical text: writers emit no whitespace outside JSON strings; readers reject it
/// (CORRUPTED_DATA). Unbounded u64 values are decimal strings; hashes are 32-char lowercase hex.

/// ---- write-side JSON micro-vocabulary ----

/// Writes '{' on the first call, ',' after, then "key": . `key` must be plain ASCII (written raw).
void writeKey(WriteBuffer & out, std::string_view key, bool & first);
void writeStringValue(WriteBuffer & out, std::string_view s);
void writeHex128Value(WriteBuffer & out, const UInt128 & v);
void writeU64StringValue(WriteBuffer & out, uint64_t v);
/// Writes a bare JSON bool literal (true/false); pairs with JsonObjectReader::readBool.
void writeBoolValue(WriteBuffer & out, bool v);
/// Writes '}' ("{}" when no key was written).
void closeObject(WriteBuffer & out, bool & first);

/// ---- read-side pull cursor over one canonical JSON object ----

class JsonObjectReader
{
public:
    /// Consumes the opening '{'.
    JsonObjectReader(ReadBuffer & in_, KeyStrictness strictness_, std::string_view what_);
    /// Advances to the next key; false when the closing '}' was consumed. The caller must
    /// consume the value (one read* / skipUnknown) before the next call. Duplicate keys are
    /// CORRUPTED_DATA.
    bool nextKey(String & key);
    String readString();
    UInt128 readHex128();
    uint64_t readU64String();
    uint64_t readU64Number();
    bool readBool();
    /// The evolution rule for a key the caller does not recognize: '!'-prefixed ->
    /// UNKNOWN_FORMAT_VERSION (critical); Strict -> CORRUPTED_DATA; Tolerant -> skip the value.
    void skipUnknown(const String & key);

private:
    template <typename F>
    auto guarded(F && f);

    ReadBuffer & in;
    KeyStrictness strictness;
    String what;
    std::vector<String> seen_keys;
    bool first = true;
    bool done = false;
};

/// ---- header line / trailer line / raw line access ----

struct TextHeader
{
    String type;
    uint32_t v = 0;
};

/// Writes line 1: {"type":"cas_<x>","v":G_BUILD}\n
void writeHeaderLine(WriteBuffer & out, FormatId id);
/// Writes a trailer line: {"n":N}\n
void writeTrailerLine(WriteBuffer & out, uint64_t n);
/// Reads and gates line 1 against `id`'s registered type; wrong type -> CORRUPTED_DATA; v above
/// what this build understands -> UNKNOWN_FORMAT_VERSION.
TextHeader expectHeaderLine(ReadBuffer & in, FormatId id);
/// Best-effort "is this a CAS object, and which one" for fsck/dispatch: swallows every failure and
/// returns nullopt. Never the load-bearing gate — that is expectHeaderLine.
std::optional<TextHeader> sniffHeaderLine(std::string_view bytes);
/// Reads one line (excluding the '\n' terminator); CORRUPTED_DATA on missing terminator or a line
/// longer than `line_cap`.
String readLine(ReadBuffer & in, uint64_t line_cap, std::string_view what);

/// ---- the zstd arm ----

/// True iff `bytes` starts with the zstd frame magic (28 B5 2F FD).
bool looksZstd(std::string_view bytes);
/// Compression per the per-type policy: `Always` -> one zstd frame (any size, checksum on);
/// everything else -> identity (returns `text` unchanged).
String sealObject(FormatId id, String text);
/// Inverse of sealObject. A compressed body is only legal when `id`'s policy is `Always`
/// (declared content size checked against the cap before allocation); a raw body is always
/// accepted (repair path — e.g. an operator-restored uncompressed copy).
String openObject(FormatId id, std::string_view stored);

}
