#pragma once


namespace DB
{

enum class TTLMode : uint8_t
{
    DELETE,
    MOVE,
    GROUP_BY,
    RECOMPRESS,
    /// `TTL <expr> EXPORT TO TABLE [db.]table`: copy expired parts to an Iceberg or object storage table.
    EXPORT,
};

}
