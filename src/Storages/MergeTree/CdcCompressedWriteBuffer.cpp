#include <Storages/MergeTree/CdcCompressedWriteBuffer.h>
#include <Compression/CompressionFactory.h>
#include <Core/Defines.h>
#include <IO/WriteHelpers.h>
#include <city.h>
#include <climits>
#include <cstring>

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

CdcCompressedWriteBuffer::CdcCompressedWriteBuffer(
    WriteBuffer & out_,
    CompressionCodecPtr codec_,
    Cas::ChunkerParams chunker_params,
    size_t buf_size,
    bool use_adaptive_buffer_size_,
    size_t adaptive_buffer_initial_size)
    /// Adaptive sizing would nextImpl in 16 KiB steps and copy each into `current_chunk`. A full
    /// `max_compress_block_size` buffer (typically 1 MiB) is the right feed size for the chunker.
    : BufferWithOwnMemory<WriteBuffer>(buf_size ? buf_size : DBMS_DEFAULT_BUFFER_SIZE)
    , out(out_)
    , codec(codec_ ? std::move(codec_) : CompressionCodecFactory::instance().getDefaultCodec())
    , chunker(chunker_params)
{
    (void)use_adaptive_buffer_size_;
    (void)adaptive_buffer_initial_size;
}

void CdcCompressedWriteBuffer::emitCompressed(const char * uncompressed, UInt32 uncompressed_size)
{
    if (!uncompressed_size)
        return;

    const UInt32 compressed_reserve_size = codec->getCompressedReserveSize(uncompressed_size);
    if (out.available() >= compressed_reserve_size + sizeof(CityHash_v1_0_2::uint128))
    {
        char * out_compressed_ptr = out.position() + sizeof(CityHash_v1_0_2::uint128);
        const UInt32 compressed_size = codec->compress(uncompressed, uncompressed_size, out_compressed_ptr);
        const CityHash_v1_0_2::uint128 checksum = CityHash_v1_0_2::CityHash128(out_compressed_ptr, compressed_size);
        writeBinaryLittleEndian(checksum.low64, out);
        writeBinaryLittleEndian(checksum.high64, out);
        out.position() += compressed_size;
        return;
    }

    compressed_scratch.resize(compressed_reserve_size);
    const UInt32 compressed_size = codec->compress(uncompressed, uncompressed_size, compressed_scratch.data());
    const CityHash_v1_0_2::uint128 checksum = CityHash_v1_0_2::CityHash128(compressed_scratch.data(), compressed_size);
    writeBinaryLittleEndian(checksum.low64, out);
    writeBinaryLittleEndian(checksum.high64, out);
    out.write(compressed_scratch.data(), compressed_size);
}

void CdcCompressedWriteBuffer::nextImpl()
{
    if (!offset())
        return;

    /// HashingWriteBuffer shares this buffer and MergeTree records marks against
    /// `offsetInCurrentCompressedBlock()` *before* this flush. Those marks assume the bytes already
    /// in `current_chunk` plus this buffer will be one ClickHouse compressed block. FastCDC may want
    /// to cut in the middle of the buffer; emitting there would leave later marks pointing past the
    /// end of that block (nullable null-map vs nested size mismatch on read). Defer the cut to the
    /// end of this flush and emit the whole accumulated window as one block.
    const char * data = working_buffer.begin();
    const size_t size = offset();
    bool emit = false;
    size_t data_offset = 0;
    while (data_offset < size)
    {
        const auto res = chunker.feed(std::string_view(data + data_offset, size - data_offset));
        if (res.taken != 0)
        {
            const size_t old = current_chunk.size();
            current_chunk.resize(old + res.taken);
            memcpy(current_chunk.data() + old, data + data_offset, res.taken);
            data_offset += res.taken;
        }
        if (!res.boundary)
            break;
        emit = true;
        chunker.startChunk();
    }

    if (!emit)
        return;

    if (current_chunk.size() > INT_MAX)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CdcCompressedWriteBuffer: chunk larger than 2 GiB");
    emitCompressed(current_chunk.data(), static_cast<UInt32>(current_chunk.size()));
    current_chunk.clear();
    chunker.startChunk();
}

void CdcCompressedWriteBuffer::finalizeImpl()
{
    next();
    if (!current_chunk.empty())
    {
        if (current_chunk.size() > INT_MAX)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "CdcCompressedWriteBuffer: trailing chunk larger than 2 GiB");
        emitCompressed(current_chunk.data(), static_cast<UInt32>(current_chunk.size()));
        current_chunk.clear();
    }
    BufferWithOwnMemory<WriteBuffer>::finalizeImpl();
}

void CdcCompressedWriteBuffer::cancelImpl() noexcept
{
    BufferWithOwnMemory<WriteBuffer>::cancelImpl();
    out.cancel();
}

}
