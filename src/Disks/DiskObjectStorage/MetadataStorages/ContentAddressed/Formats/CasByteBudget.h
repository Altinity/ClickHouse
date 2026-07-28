#pragma once
#include <cstdint>

namespace DB::Cas
{

/// The byte arithmetic every write-once text control object owes BEFORE its PUT.
///
/// Two caps, deliberately kept apart (they answer different questions and fail differently):
///
///   * the LINE cap bounds ONE encoded record. A record longer than it is not merely large, it is
///     UNREADABLE: the streaming reader refuses the line, so an object containing one can never be
///     decoded again. This predicate belongs at the point a record is emitted.
///   * the OBJECT cap bounds the whole object. It is the ADDITIVE question — a fixed frame (header,
///     meta, trailer) plus the worst-case reservation of every entry — because a producer that must
///     decide whether one MORE entry still fits cannot encode first and measure afterwards.
///
/// Both predicates accept at EQUALITY: a cap is the largest permitted value, not the first forbidden
/// one, and the readers enforce it the same way.
///
/// Additions saturate. A modular sum that wrapped would answer "fits" for an object that does not,
/// turning an overflow into a durable unreadable object — the one outcome the caps exist to prevent.

/// `a + b`, clamped to `UINT64_MAX` instead of wrapping.
constexpr uint64_t addByteBudget(uint64_t a, uint64_t b)
{
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

/// LINE predicate: `encoded_row_bytes <= line_cap`, measured EXCLUDING the '\n' terminator (the same
/// bytes `readLine` measures). `line_cap == 0` means the format declares no line cap.
constexpr bool fitsLineCap(uint64_t encoded_row_bytes, uint64_t line_cap)
{
    return line_cap == 0 || encoded_row_bytes <= line_cap;
}

/// OBJECT predicate: `fixed_bytes + entries_reservation <= object_cap`, evaluated with saturating
/// addition. `object_cap == 0` means the format declares no whole-object cap (a streamed format that
/// is never materialized whole).
constexpr bool fitsObjectCap(uint64_t fixed_bytes, uint64_t entries_reservation, uint64_t object_cap)
{
    return object_cap == 0 || addByteBudget(fixed_bytes, entries_reservation) <= object_cap;
}

}
