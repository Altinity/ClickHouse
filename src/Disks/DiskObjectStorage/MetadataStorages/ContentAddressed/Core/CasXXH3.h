#pragma once

/// Isolated include wrapper + tiny helper API for the xxHash XXH3-128 hash used by `CasBlobHasher`.
///
/// Two problems this header contains in one place:
///   1. In the `dbms` target a plain `#include <xxhash.h>` resolves to lz4's bundled copy
///      (`contrib/lz4/lib` is a higher-priority `-I` than the `-isystem contrib/xxHash`), and that copy
///      provides only XXH32/64 — NOT the XXH3 API. So xxHash is referenced by an explicit repo-relative
///      path (from this file's directory up to the repo root) which unambiguously picks the full
///      standalone `contrib/xxHash` that has XXH3.
///   2. `XXH_INLINE_ALL` makes xxHash a header-only static-inline implementation whose vendored C is not
///      clean under the CAS `-Werror -Weverything` flags, and whose inline functions carry an "unused"
///      attribute that trips `-Wused-but-marked-unused` at every call site. `#pragma clang system_header`
///      marks the rest of THIS header (and everything it includes, plus the helper calls below) as a
///      system header, so ALL of those warnings are suppressed here — without disabling warnings for any
///      real `CasBlobHasher` code, which only ever touches the clean `DB::Cas` helpers defined below.
///
/// lz4's and `contrib/xxHash`'s XXH3 are the same spec'd algorithm, so digests are identical regardless.
#pragma clang system_header

#include <base/types.h>
#include <cstddef>

#define XXH_INLINE_ALL
#include "../../../../../../contrib/xxHash/xxhash.h"

namespace DB::Cas
{

/// Streaming XXH3-128 state wrapper. Feed bytes with `update`, read the 128-bit digest with `digest`.
/// All raw xxHash symbols are confined to this system header, so callers see no xxHash warnings.
class Xxh3Streamer
{
public:
    Xxh3Streamer() : state(XXH3_createState()) { XXH3_128bits_reset(state); }
    ~Xxh3Streamer() { XXH3_freeState(state); }

    Xxh3Streamer(const Xxh3Streamer &) = delete;
    Xxh3Streamer & operator=(const Xxh3Streamer &) = delete;

    bool valid() const { return state != nullptr; }
    void update(const void * data, size_t len) { XXH3_128bits_update(state, data, len); }

    void digest(UInt64 & low, UInt64 & high) const
    {
        const XXH128_hash_t d = XXH3_128bits_digest(state);
        low = d.low64;
        high = d.high64;
    }

private:
    XXH3_state_t * state;
};

/// One-shot XXH3-128 of a byte range into (low, high) halves of the 128-bit digest.
inline void xxh3_128_oneshot(const void * data, size_t len, UInt64 & low, UInt64 & high)
{
    const XXH128_hash_t d = XXH3_128bits(data, len);
    low = d.low64;
    high = d.high64;
}

}
