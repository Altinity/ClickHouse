#pragma once

#include <cstdint>

namespace DB::Cas
{

/// The pool-wide floor for `blob_header_len`. One compile-time owner, read by BOTH
/// `validatePoolBlobHeaderLen` (pool creation / decode) and the blob-envelope codec, so the
/// mandatory-descriptor worst-case proof and the enforced floor can never guard different numbers.
/// The derivation of the floor lives in `CasPoolMetaFormat.cpp` next to the worst-case table.
inline constexpr uint64_t kMinBlobHeaderLen = 240;

}
