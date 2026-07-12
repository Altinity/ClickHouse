#pragma once
#include <base/types.h>
#include <base/hex.h>
#include <Common/Exception.h>
#include <compare>
#include <cstdint>
#include <optional>
#include <string_view>

namespace DB
{
namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}
}

namespace DB::Cas
{

/// The ordered ref-transaction identifier (spec §Ordered Ref Transaction Identifier). A successful
/// writer mount establishes a strictly newer `writer_epoch`; within an epoch the mounted writer
/// allocates `ref_sequence` from a Store-wide strictly increasing counter at append time. Both fields
/// are nonzero for a valid id -- {0, 0} is never a real transaction. `writer_epoch` is the primary
/// ordering component, so tuple order matches the intended timeline even across an epoch restart that
/// resets `ref_sequence` back to one.
struct RefTxnId
{
    uint64_t writer_epoch = 0;
    uint64_t ref_sequence = 0;

    auto operator<=>(const RefTxnId &) const = default;
};

/// Renders the canonical form: two fixed-width, lower-case, 16-digit hexadecimal numbers joined by
/// '-' (e.g. "0000000000000007-000000000000008e"). Lexical order of the render equals tuple order of
/// `id`, because '-' (0x2d) sorts below every hex digit character and both fields are fixed-width.
/// Throws LOGICAL_ERROR if either field is zero: this render becomes an object key, and an invalid id
/// must never silently produce a well-formed-looking one.
inline String renderRefTxnId(const RefTxnId & id)
{
    if (id.writer_epoch == 0 || id.ref_sequence == 0)
        throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR,
            "RefTxnId: writer_epoch and ref_sequence must both be nonzero, got {}-{}",
            id.writer_epoch, id.ref_sequence);
    return getHexUIntLowercase(id.writer_epoch) + "-" + getHexUIntLowercase(id.ref_sequence);
}

/// Parses the canonical form only: exactly 33 characters, '-' at index 16, exactly 16 lower-case hex
/// digits ('0'-'9', 'a'-'f') either side, and both parsed fields nonzero. Any other shape -- short,
/// long, upper-case, non-hex, misplaced separator, or a zero component -- returns nullopt rather than
/// throwing, since parsing an untrusted listed key is an ordinary "is this ours" question.
inline std::optional<RefTxnId> parseRefTxnId(std::string_view s)
{
    constexpr size_t kFieldLen = 16;
    constexpr size_t kTotalLen = kFieldLen * 2 + 1;
    if (s.size() != kTotalLen || s[kFieldLen] != '-')
        return std::nullopt;

    /// Strict lower-case-hex-only parse: `unhexUInt` (base/hex.h) also accepts upper-case, which the
    /// canonical form must reject, so digits are validated and accumulated by hand here.
    const auto parseField = [](std::string_view field) -> std::optional<uint64_t>
    {
        uint64_t value = 0;
        for (char c : field)
        {
            uint64_t digit;
            if (c >= '0' && c <= '9')
                digit = static_cast<uint64_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                digit = static_cast<uint64_t>(c - 'a' + 10);
            else
                return std::nullopt;
            value = (value << 4) | digit;
        }
        return value;
    };

    const auto epoch = parseField(s.substr(0, kFieldLen));
    const auto seq = parseField(s.substr(kFieldLen + 1, kFieldLen));
    if (!epoch || !seq || *epoch == 0 || *seq == 0)
        return std::nullopt;
    return RefTxnId{*epoch, *seq};
}

}
