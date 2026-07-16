#pragma once

namespace DB::Cas
{

/// Sealed source-edge run row tags (spec §2.1). Extracted to this tiny identifier-layer header (codecs
/// v3 phase 5) so the backend-free `Formats/CasRecordStreamFormat` codec and the `Core/` subsystem
/// (`CasBlobInDegree`, `CasGc`, `CasFsck`) share ONE definition without the codec having to include the
/// backend-pulling `CasBlobInDegree.h`. No subsystem dependencies; safe to include from `Formats/`.
///   kEdgeActive — a surviving active edge
///   kZeroMarker — the blob transitioned to zero this generation (per-generation, never carried)
///   kCondemned  — the blob is retired-in-snapshot (carries a full condemned row at the zero sentinel key)
constexpr char kEdgeActive = 0x01;
constexpr char kZeroMarker = 0x00;
constexpr char kCondemned  = 0x02;

}
