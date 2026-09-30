#pragma once

#include "config.h"

#if USE_DATASKETCHES

#include <AggregateFunctions/Helpers.h>
#include <AggregateFunctions/IAggregateFunction.h>
#include <Columns/ColumnDecimal.h>
#include <Columns/ColumnsNumber.h>
#include <Common/Exception.h>
#include <Common/assert_cast.h>
#include <Core/Field.h>
#include <Core/UUID.h>
#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeDate32.h>
#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypeDateTime64.h>
#include <DataTypes/DataTypeIPv4andIPv6.h>
#include <DataTypes/DataTypeUUID.h>
#include <DataTypes/DataTypesNumber.h>
#include <IO/ReadBuffer.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteBuffer.h>
#include <IO/WriteHelpers.h>

#include <hll.hpp>

#include <cmath>
#include <memory>
#include <type_traits>

namespace DB
{

namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}


/// Keeps insertion and union states separate to preserve the estimator used before merging.
class HllSketchData
{
private:
    std::unique_ptr<datasketches::hll_sketch> sk_update;
    std::unique_ptr<datasketches::hll_union> sk_union;

    datasketches::hll_sketch * getSkUpdate(uint8_t lg_config_k, datasketches::target_hll_type tgt_type)
    {
        if (!sk_update)
            sk_update = std::make_unique<datasketches::hll_sketch>(lg_config_k, tgt_type);
        return sk_update.get();
    }

    datasketches::hll_union * getSkUnion(uint8_t lg_config_k)
    {
        if (!sk_union)
            sk_union = std::make_unique<datasketches::hll_union>(lg_config_k);
        return sk_union.get();
    }

    void foldUpdateIntoUnionIfNeeded()
    {
        if (sk_union && sk_update)
        {
            sk_union->update(*sk_update);
            sk_update.reset();
        }
    }

public:
    template <typename T>
    void insert(T value, uint8_t lg_config_k, datasketches::target_hll_type tgt_type)
    {
        getSkUpdate(lg_config_k, tgt_type)->update(value);
        foldUpdateIntoUnionIfNeeded();
    }

    void insertData(const char * data, size_t size, uint8_t lg_config_k, datasketches::target_hll_type tgt_type)
    {
        getSkUpdate(lg_config_k, tgt_type)->update(static_cast<const void *>(data), size);
        foldUpdateIntoUnionIfNeeded();
    }

    UInt64 size(datasketches::target_hll_type tgt_type) const
    {
        /// Rounding preserves exact cardinalities despite floating-point error.
        if (sk_union)
            return static_cast<UInt64>(std::llround(sk_union->get_result(tgt_type).get_estimate()));
        if (sk_update)
            return static_cast<UInt64>(std::llround(sk_update->get_estimate()));
        return 0;
    }

    void merge(const HllSketchData & rhs, uint8_t lg_config_k, datasketches::target_hll_type tgt_type)
    {
        datasketches::hll_union * u = getSkUnion(lg_config_k);

        foldUpdateIntoUnionIfNeeded();

        if (rhs.sk_update)
            u->update(*rhs.sk_update);
        else if (rhs.sk_union)
            u->update(rhs.sk_union->get_result(tgt_type));
    }

    /// You can only call this for an empty object.
    void read(ReadBuffer & in, uint8_t lg_config_k)
    {
        datasketches::hll_sketch::vector_bytes bytes;
        readVectorBinary(bytes, in);
        if (bytes.empty())
            return;

        try
        {
            auto sk = datasketches::hll_sketch::deserialize(bytes.data(), bytes.size());
            getSkUnion(lg_config_k)->update(std::move(sk));
        }
        catch (const DB::Exception &)
        {
            throw;
        }
        catch (const std::bad_alloc &)
        {
            throw;
        }
        catch (const std::exception & e)
        {
            /// Translate malformed input to avoid a logical exception in `SerializationAggregateFunction`.
            throw Exception(ErrorCodes::CORRUPTED_DATA, "Cannot deserialize HLL sketch state: {}", e.what());
        }
    }

    void write(WriteBuffer & out, uint8_t lg_config_k, datasketches::target_hll_type tgt_type) const
    {
        datasketches::hll_sketch::vector_bytes bytes;
        if (sk_update)
            bytes = sk_update->serialize_compact();
        else if (sk_union)
            bytes = sk_union->get_result(tgt_type).serialize_compact();
        else
            bytes = datasketches::hll_sketch(lg_config_k, tgt_type).serialize_compact();
        writeVectorBinary(bytes, out);
    }
};


template <typename T>
class AggregateFunctionUniqApacheHLL final : public IAggregateFunctionDataHelper<HllSketchData, AggregateFunctionUniqApacheHLL<T>>
{
    using Base = IAggregateFunctionDataHelper<HllSketchData, AggregateFunctionUniqApacheHLL<T>>;

    uint8_t lg_config_k;
    datasketches::target_hll_type target_type;

public:
    AggregateFunctionUniqApacheHLL(
        uint8_t lg_config_k_,
        datasketches::target_hll_type target_type_,
        const DataTypes & argument_types_,
        const Array & params_)
        : Base(argument_types_, params_, std::make_shared<DataTypeUInt64>())
        , lg_config_k(lg_config_k_)
        , target_type(target_type_)
    {
    }

    String getName() const override { return "uniqApacheHLL"; }

    bool allocatesMemoryInArena() const override { return false; }

    void add(AggregateDataPtr __restrict place, const IColumn ** columns, size_t row_num, Arena *) const override
    {
        auto & data = this->data(place);

        if constexpr (std::is_same_v<T, String>)
        {
            const auto value = columns[0]->getDataAt(row_num);
            data.insertData(value.data(), value.size(), lg_config_k, target_type);
        }
        else
        {
            const auto & value = assert_cast<const ColumnVectorOrDecimal<T> &>(*columns[0]).getData()[row_num];

            if constexpr (std::is_same_v<T, UUID>)
            {
                /// Convert the two host-order halves of a `UUID` to canonical bytes.
                const UInt64 halves[2] = {
                    std::byteswap(UUIDHelpers::getHighBytes(value)),
                    std::byteswap(UUIDHelpers::getLowBytes(value)),
                };
                data.insertData(reinterpret_cast<const char *>(halves), sizeof(halves), lg_config_k, target_type);
            }
            else if constexpr (std::is_same_v<T, IPv6>)
                /// Already held in network order, which is the canonical form.
                data.insertData(reinterpret_cast<const char *>(&value), sizeof(value), lg_config_k, target_type);
            else if constexpr (is_decimal<T>)
                /// Hash `DateTime64` as epoch ticks; external producers must use the same scale.
                data.insert(static_cast<Int64>(value.value), lg_config_k, target_type);
            else if constexpr (std::is_same_v<T, IPv4>)
                data.insert(static_cast<UInt64>(value.toUnderType()), lg_config_k, target_type);
            else if constexpr (std::is_same_v<T, BFloat16> || std::is_floating_point_v<T>)
                data.insert(static_cast<Float64>(value), lg_config_k, target_type);
            else if constexpr (std::is_signed_v<T>)
                data.insert(static_cast<Int64>(value), lg_config_k, target_type);
            else
                data.insert(static_cast<UInt64>(value), lg_config_k, target_type);
        }
    }

    /// Serialized sketches carry their configuration, so parameters need not match.
    bool haveSameStateRepresentationImpl(const IAggregateFunction & rhs) const override
    {
        return getName() == rhs.getName() && this->haveEqualArgumentTypes(rhs);
    }

    void merge(AggregateDataPtr __restrict place, ConstAggregateDataPtr rhs, Arena *) const override
    {
        this->data(place).merge(this->data(rhs), lg_config_k, target_type);
    }

    void serialize(ConstAggregateDataPtr __restrict place, WriteBuffer & buf, std::optional<size_t> /* version */) const override
    {
        this->data(place).write(buf, lg_config_k, target_type);
    }

    void deserialize(AggregateDataPtr __restrict place, ReadBuffer & buf, std::optional<size_t> /* version */, Arena *) const override
    {
        this->data(place).read(buf, lg_config_k);
    }

    void insertResultInto(AggregateDataPtr __restrict place, IColumn & to, Arena *) const override
    {
        assert_cast<ColumnUInt64 &>(to).getData().push_back(this->data(place).size(target_type));
    }

};

}

#endif
