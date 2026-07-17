#pragma once

namespace DB::Cas
{

/// Row tags for the sealed source-edge run. These byte values are part of the source-edge payload
/// format, so the backend-free `Formats/CasRecordStreamFormat` codec and the GC implementation must
/// use the same definitions. This header deliberately belongs to the dependency-free `Primitives/`
/// layer: it can be included by format code without pulling in the backend-facing `CasBlobInDegree.h`.
///
/// Source-edge rows use `source_id == 0` as a sentinel key. A real active edge must never use that key;
/// both sentinel tags are restricted to it. `kZeroMarker` describes a zero transition for the current
/// generation and is dropped when the row is carried forward. `kCondemned` carries the condemned
/// incarnation at the sentinel key across generations until settlement; its payload contains the full
/// deletion token and other condemned-row state. A condemned row subsumes the zero marker for that
/// generation.
constexpr char kEdgeActive = 0x01;
constexpr char kZeroMarker = 0x00;
constexpr char kCondemned  = 0x02;

}
