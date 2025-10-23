#pragma once

#include <Columns/IColumn.h>
#include <Core/Field.h>
#include <Common/PODArray.h>
#include <Common/assert_cast.h>
#include <Common/typeid_cast.h>

#include <Common/logger_useful.h>

namespace DB
{

/** ColumnConstable contains another column with single element,
  *  but can convert it to full when different value inserted.
  */
class ColumnConstable final : public COWHelper<IColumnHelper<ColumnConstable>, ColumnConstable>
{
private:
    friend class COWHelper<IColumnHelper<ColumnConstable>, ColumnConstable>;

    WrappedPtr data;
    size_t s;
    bool is_const;

    ColumnConstable(const ColumnPtr & data, size_t s_);
    ColumnConstable(const ColumnConstable & src) = default;

    void convertDataToFullColumn();

public:
    bool isConst() const override { return false; }

    ColumnPtr convertToFullColumn() const;

    ColumnPtr convertToFullColumnIfConst() const override
    {
        return convertToFullColumn();
    }

    ColumnPtr removeLowCardinality() const;

    std::string getName() const override
    {
        return "Constable(" + data->getName() + ")";
    }

    const char * getFamilyName() const override
    {
        return "Constable";
    }

    TypeIndex getDataType() const override
    {
        return data->getDataType();
    }

    MutableColumnPtr cloneResized(size_t new_size) const override
    {
        if (!is_const)
            return ColumnConstable::create(data->cloneResized(new_size), new_size);
        return ColumnConstable::create(data, new_size);
    }

    size_t size() const override
    {
        return s;
    }

    Field operator[](size_t n) const override
    {
        return (*data)[is_const ? 0 : n];
    }

    void get(size_t n, Field & res) const override
    {
        data->get(is_const ? 0 : n, res);
    }

    std::pair<String, DataTypePtr> getValueNameAndType(size_t n) const override
    {
        return data->getValueNameAndType(is_const ? 0 : n);
    }

    StringRef getDataAt(size_t n) const override
    {
        return data->getDataAt(is_const ? 0 : n);
    }

    UInt64 get64(size_t n) const override
    {
        return data->get64(is_const ? 0 : n);
    }

    UInt64 getUInt(size_t n) const override
    {
        return data->getUInt(is_const ? 0 : n);
    }

    Int64 getInt(size_t n) const override
    {
        return data->getInt(is_const ? 0 : n);
    }

    bool getBool(size_t n) const override
    {
        return data->getBool(is_const ? 0 : n);
    }

    Float64 getFloat64(size_t n) const override
    {
        return data->getFloat64(is_const ? 0 : n);
    }

    Float32 getFloat32(size_t n) const override
    {
        return data->getFloat32(is_const ? 0 : n);
    }

    bool isDefaultAt(size_t n) const override
    {
        return data->isDefaultAt(is_const ? 0 : n);
    }

    bool isNullAt(size_t n) const override
    {
        return data->isNullAt(is_const ? 0 : n);
    }

#if !defined(DEBUG_OR_SANITIZER_BUILD)
    void insertRangeFrom(const IColumn & src, size_t start, size_t length) override
#else
    void doInsertRangeFrom(const IColumn & src, size_t start, size_t length) override
#endif
    {
        if (length == 0)
            return;
        //if (s > 0 && (!is_const || !src.hasEqualValues() || src.getDataAt(0) != getDataAt(0)))
        {
            convertDataToFullColumn();
            auto src_full = src.convertToFullColumnIfConst();
#if !defined(DEBUG_OR_SANITIZER_BUILD)
            data->insertRangeFrom(*src_full, start, length);
#else
            data->doInsertRangeFrom(*src_full, start, length);
#endif
        }
        s += length;
    }

    void insert(const Field & field) override
    {
        convertDataToFullColumn();
        data->insert(field);
        ++s;
    }

    bool tryInsert(const Field & field) override
    {
        convertDataToFullColumn();
        if (!data->tryInsert(field))
            return false;
        ++s;
        return true;
    }

    void insertData(const char * pos, size_t length) override
    {
        convertDataToFullColumn();
        data->insertData(pos, length);
        ++s;
    }

#if !defined(DEBUG_OR_SANITIZER_BUILD)
    void insertFrom(const IColumn & src, size_t position) override
#else
    void doInsertFrom(const IColumn & src, size_t position) override
#endif
    {
        convertDataToFullColumn();
#if !defined(DEBUG_OR_SANITIZER_BUILD)
        data->insertFrom(src, position);
#else
        data->doInsertFrom(src, position);
#endif
        ++s;
    }

#if !defined(DEBUG_OR_SANITIZER_BUILD)
    void insertManyFrom(const IColumn & src, size_t position, size_t length) override
#else
    void doInsertManyFrom(const IColumn & src, size_t position, size_t length) override
#endif
    {
        convertDataToFullColumn();
#if !defined(DEBUG_OR_SANITIZER_BUILD)
        data->insertManyFrom(src, position, length);
#else
        data->doInsertManyFrom(src, position, length);
#endif
        s += length;
    }

    void insertDefault() override
    {
        convertDataToFullColumn();
        data->insertDefault();
        ++s;
    }

    void popBack(size_t n) override
    {
        if (!is_const)
            data->popBack(n);
        s -= n;
    }

    StringRef serializeValueIntoArena(size_t n, Arena & arena, char const *& begin) const override
    {
        return data->serializeValueIntoArena(is_const ? 0 : n, arena, begin);
    }

    char * serializeValueIntoMemory(size_t n, char * memory) const override
    {
        return data->serializeValueIntoMemory(is_const ? 0 : n, memory);
    }

    const char * deserializeAndInsertFromArena(const char * pos) override
    {
        const auto * res = data->deserializeAndInsertFromArena(pos);
        if (is_const)
            data->popBack(1);
        ++s;
        return res;
    }

    const char * skipSerializedInArena(const char * pos) const override
    {
        return data->skipSerializedInArena(pos);
    }

    void updateHashWithValue(size_t n, SipHash & hash) const override
    {
        data->updateHashWithValue(is_const ? 0 : n, hash);
    }

    WeakHash32 getWeakHash32() const override;

    void updateHashFast(SipHash & hash) const override
    {
        data->updateHashFast(hash);
    }

    ColumnPtr filter(const Filter & filt, ssize_t result_size_hint) const override;
    void expand(const Filter & mask, bool inverted) override;

    ColumnPtr replicate(const Offsets & offsets) const override;
    ColumnPtr permute(const Permutation & perm, size_t limit) const override;
    ColumnPtr index(const IColumn & indexes, size_t limit) const override;
    void getPermutation(PermutationSortDirection direction, PermutationSortStability stability,
                        size_t limit, int nan_direction_hint, Permutation & res) const override;
    void updatePermutation(PermutationSortDirection direction, PermutationSortStability stability,
                        size_t limit, int nan_direction_hint, Permutation & res, EqualRanges & equal_ranges) const override;

    size_t byteSize() const override
    {
        return data->byteSize() + sizeof(s) + sizeof(is_const);
    }

    size_t byteSizeAt(size_t n) const override
    {
        return data->byteSizeAt(is_const ? 0 : n);
    }

    size_t allocatedBytes() const override
    {
        return data->allocatedBytes() + sizeof(s) + sizeof(is_const);
    }

#if !defined(DEBUG_OR_SANITIZER_BUILD)
    int compareAt(size_t n, size_t m, const IColumn & rhs, int nan_direction_hint) const override
#else
    int doCompareAt(size_t n, size_t m, const IColumn & rhs, int nan_direction_hint) const override
#endif
    {
        if (!is_const)
            return data->compareAt(n, m, rhs, nan_direction_hint);
        return data->compareAt(0, 0, *assert_cast<const ColumnConstable &>(rhs).data, nan_direction_hint);
    }

    void compareColumn(const IColumn & rhs, size_t rhs_row_num,
                       PaddedPODArray<UInt64> * row_indexes, PaddedPODArray<Int8> & compare_results,
                       int direction, int nan_direction_hint) const override;

    bool hasEqualValues() const override
    {
        if (is_const)
            return true;
        return data->hasEqualValues();
    }

    MutableColumns scatter(ColumnIndex num_columns, const Selector & selector) const override;

    void gather(ColumnGathererStream &) override;

    void getExtremes(Field & min, Field & max) const override
    {
        data->getExtremes(min, max);
    }

    void forEachSubcolumn(ColumnCallback callback) const override
    {
        callback(data);
    }

    void forEachSubcolumnRecursively(RecursiveColumnCallback callback) const override
    {
        callback(*data);
        data->forEachSubcolumnRecursively(callback);
    }

    void forEachMutableSubcolumn(MutableColumnCallback callback) override
    {
        callback(data);
    }

    void forEachMutableSubcolumnRecursively(RecursiveMutableColumnCallback callback) override
    {
        callback(*data);
        data->forEachMutableSubcolumnRecursively(callback);
    }

    bool structureEquals(const IColumn & rhs) const override
    {
        if (const auto * rhs_concrete = typeid_cast<const ColumnConstable *>(&rhs))
            return data->structureEquals(*rhs_concrete->data);
        return false;
    }

    double getRatioOfDefaultRows(double sample_ratio) const override
    {
        if (!is_const)
            return data->getRatioOfDefaultRows(sample_ratio);
        return data->isDefaultAt(0) ? 1.0 : 0.0;
    }

    UInt64 getNumberOfDefaultRows() const override
    {
        if (!is_const)
            return data->getNumberOfDefaultRows();
        return data->isDefaultAt(0) ? s : 0;
    }

    void getIndicesOfNonDefaultRows(Offsets & indices, size_t from, size_t limit) const override
    {
        if (!is_const)
        {
            data->getIndicesOfNonDefaultRows(indices, from, limit);
            return;
        }
        if (!data->isDefaultAt(0))
        {
            size_t to = limit && from + limit < size() ? from + limit : size();
            indices.reserve_exact(indices.size() + to - from);
            for (size_t i = from; i < to; ++i)
                indices.push_back(i);
        }
    }

    bool isNullable() const override { return isColumnNullable(*data); }
    bool onlyNull() const override
    {
        if (!is_const)
            return data->onlyNull();
        return data->isNullAt(0);
    }

    bool isNumeric() const override { return data->isNumeric(); }
    bool isFixedAndContiguous() const override { return data->isFixedAndContiguous(); }
    bool valuesHaveFixedSize() const override { return data->valuesHaveFixedSize(); }
    size_t sizeOfValueIfFixed() const override { return data->sizeOfValueIfFixed(); }
    std::string_view getRawData() const override { return data->getRawData(); }

    /// Not part of the common interface.

    const ColumnPtr & getDataColumnPtr() const { return data; }

    bool isCollationSupported() const override { return data->isCollationSupported(); }

    bool hasDynamicStructure() const override { return data->hasDynamicStructure(); }
};

ColumnConstable::Ptr createColumnConstable(const ColumnPtr & column, Field value);
ColumnConstable::Ptr createColumnConstable(const ColumnPtr & column, size_t const_value_index);
ColumnConstable::Ptr createColumnConstableWithDefaultValue(const ColumnPtr  &column);


}
