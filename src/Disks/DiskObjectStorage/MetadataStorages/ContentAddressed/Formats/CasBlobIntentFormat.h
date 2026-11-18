#pragma once
#include <base/types.h>
#include <string_view>

namespace DB::Cas
{

/// The one byte string every blob deletion intent holds: `{"type":"cas_blob_intent","v":4}\n{}\n`.
/// The key carries the identity, so the body carries nothing.
const String & blobIntentRecord();

/// Byte equality with `blobIntentRecord`; no header gate, no parse.
bool isBlobIntentRecord(std::string_view bytes);

}
