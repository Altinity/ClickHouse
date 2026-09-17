#pragma once

#include <Compression/ICompressionCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasContentChunker.h>
#include <IO/BufferWithOwnMemory.h>
#include <IO/WriteBuffer.h>
#include <Common/PODArray.h>

namespace DB
{

/// Compresses the uncompressed stream in FastCDC windows instead of fixed `max_compress_block_size`
/// blocks. A concatenative merge that re-emits the same uncompressed runs then produces identical
/// ClickHouse compressed blocks, which CAS can store as shared blobs.
///
/// Marks must use `offsetInCurrentCompressedBlock()`, not `HashingWriteBuffer::offset()`: the
/// hashing wrapper shares this buffer and resets on every `nextImpl`, while the current ClickHouse
/// compressed block may still be accumulating in `current_chunk`.
class CdcCompressedWriteBuffer : public BufferWithOwnMemory<WriteBuffer>
{
public:
    CdcCompressedWriteBuffer(
        WriteBuffer & out_,
        CompressionCodecPtr codec_,
        Cas::ChunkerParams chunker_params,
        size_t buf_size,
        bool use_adaptive_buffer_size_,
        size_t adaptive_buffer_initial_size);

    /// Offset inside the ClickHouse compressed block that has not been emitted yet. This is the
    /// value MergeTree stores as `offset_in_decompressed_block`.
    size_t offsetInCurrentCompressedBlock() const { return current_chunk.size() + offset(); }

private:
    void nextImpl() override;
    void finalizeImpl() override;
    void cancelImpl() noexcept override;

    void emitCompressed(const char * uncompressed, UInt32 uncompressed_size);

    WriteBuffer & out;
    CompressionCodecPtr codec;
    Cas::ContentChunker chunker;
    PODArray<char> current_chunk;
    PODArray<char> compressed_scratch;
};

}
