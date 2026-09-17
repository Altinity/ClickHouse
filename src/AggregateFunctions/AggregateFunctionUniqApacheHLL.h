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
#include <boost/noncopyable.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <type_traits>

namespace DB
{

namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}


/** An Apache DataSketches HLL sketch as an aggregate function state, serialized in the
  * DataSketches HLL format so that `-State`/`-Merge` interoperate with external services.
  *
  * `lg_config_k` and the target type are owned by the aggregate function and passed in, so the
  * state stores the sketch alone.
  */
class HllSketchData : private boost::noncopyable
{
private:
    /// Used for insertions.
    std::unique_ptr<datasketches::hll_sketch> sk_update;
    /// Used for merging.
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
        {
            /// `hll_union` takes `lg_max_k` in [7, 21] while a sketch may use [4, 21], hence the
            /// floor of 7. It does not inflate the result: `get_result` downsamples to the smallest
            /// `lg_config_k` the union has seen. So the resolution of a merged state is the minimum
            /// over its inputs, not the `lg_config_k` of the type, and merging a coarser sketch from
            /// elsewhere lowers both the estimate and the state written back. Merged from empty
            /// states only there is no minimum, and it serializes as `lg_config_k = 7`.
            sk_union = std::make_unique<datasketches::hll_union>(std::max<uint8_t>(lg_config_k, 7));
        }
        return sk_union.get();
    }

    /// Fold a sketch updated after the union was allocated into it, so merges see one state.
    void foldUpdateIntoUnionIfNeeded()
    {
        if (sk_union && sk_update)
        {
            sk_union->update(*sk_update);
            sk_update.reset(nullptr);
        }
    }

public:
    HllSketchData() = default;
    ~HllSketchData() = default;

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
        /// Round rather than truncate: `get_estimate` returns a `double`, and `999.9999` for an
        /// exactly-known cardinality must not become `999`.
        if (sk_union)
            return static_cast<UInt64>(std::llround(sk_union->get_result(tgt_type).get_estimate()));
        if (sk_update)
            return static_cast<UInt64>(std::llround(sk_update->get_estimate()));
        return 0;
    }

    void merge(const HllSketchData & rhs, uint8_t lg_config_k, datasketches::target_hll_type tgt_type)
    {
        datasketches::hll_union * u = getSkUnion(lg_config_k);

        if (sk_update)
        {
            u->update(*sk_update);
            sk_update.reset(nullptr);
        }

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
            /// Memory pressure, not corrupted data.
            throw;
        }
        catch (const std::exception & e)
        {
            /// `datasketches` reports malformed input as `std::invalid_argument` / `std::out_of_range`.
            /// Not being `DB::Exception`, those escape `SerializationAggregateFunction`'s
            /// `catch (...)` and abort as a logical error, so translate them here.
            throw Exception(ErrorCodes::CORRUPTED_DATA, "Cannot deserialize HLL sketch state: {}", e.what());
        }
    }

    void write(WriteBuffer & out, datasketches::target_hll_type tgt_type) const
    {
        if (sk_update)
        {
            auto bytes = sk_update->serialize_compact();
            writeVectorBinary(bytes, out);
        }
        else if (sk_union)
        {
            auto bytes = sk_union->get_result(tgt_type).serialize_compact();
            writeVectorBinary(bytes, out);
        }
        else
        {
            datasketches::hll_sketch::vector_bytes bytes;
            writeVectorBinary(bytes, out);
        }
    }
};


/** `uniqApacheHLL` over a single column of a type the sketch can hash directly.
  *
  * The value takes one of the three shapes the DataSketches API accepts - an 8-byte integer, an
  * IEEE-754 double, or raw bytes - so that an external producer reaches the same sketch.
  *
  * `lg_config_k` and the target type live here rather than in the state, which is what lets states
  * of different parameterisations share one binary representation.
  */
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
                /// ClickHouse holds a UUID as two 64-bit halves in host order, so its bytes in
                /// memory are not the canonical 16 an external producer works from.
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
                /// `DateTime64(3)` holds the epoch milliseconds a caller elsewhere passes to
                /// `update(long)`. The scale belongs to the type, so both sides must agree on it.
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
    /// A serialized sketch describes its own configuration, so states of different parameterisations
    /// are interchangeable. Merging across them takes the resolution of the coarsest input.
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
        this->data(place).write(buf, target_type);
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


/// `uniqApacheHLL([lg_k, [type]])(x)`, with `lg_k` in [4, 21] (default 12) and `type` one of
/// 'HLL_4', 'HLL_6', 'HLL_8' (default 'HLL_4').
AggregateFunctionPtr createAggregateFunctionUniqApacheHLL(
    const std::string & name, const DataTypes & argument_types, const Array & params, const Settings *);

}

#endif
