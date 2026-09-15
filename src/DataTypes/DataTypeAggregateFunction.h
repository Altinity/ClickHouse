#pragma once

#include <AggregateFunctions/IAggregateFunction_fwd.h>
#include <Core/Field.h>
#include <DataTypes/IDataType.h>
#include <Parsers/IAST_fwd.h>


namespace DB
{

/** Type - the state of the aggregate function.
  * Type parameters is an aggregate function, the types of its arguments, and its parameters (for parametric aggregate functions).
  *
  * Data type can support versioning for serialization of aggregate function state.
  * Version 0 also means no versioning. When a table with versioned data type is attached, its version is parsed from AST. If
  * there is no version in AST, then it is either attach with no version in metadata (then version is 0) or it
  * is a new data type (then version is default - latest).
  */
class DataTypeAggregateFunction final : public IDataType
{
private:
    AggregateFunctionPtr function;
    DataTypes argument_types;
    Array parameters;
    mutable std::optional<size_t> version;

    String getNameImpl(bool with_version, bool always_emit_version) const;

public:
    static constexpr bool is_parametric = true;

    DataTypeAggregateFunction(AggregateFunctionPtr function_, const DataTypes & argument_types_,
                              const Array & parameters_, std::optional<size_t> version_ = std::nullopt);

    size_t getVersion() const;

    String getFunctionName() const;
    AggregateFunctionPtr getFunction() const { return function; }

    String doGetName() const override;
    String getNameWithoutVersion() const;
    /// Like getName(), but keeps an explicit 0 version, which getName() drops. Used by
    /// getClickHouseTypeAnnotationName().
    String getNameForAnnotation() const;
    const char * getFamilyName() const override { return "AggregateFunction"; }
    TypeIndex getTypeId() const override { return TypeIndex::AggregateFunction; }

    Array getParameters() const { return parameters; }

    bool canBeInsideNullable() const override { return false; }

    DataTypePtr getReturnType() const;
    DataTypePtr getReturnTypeToPredict() const;
    DataTypes getArgumentsDataTypes() const { return argument_types; }

    MutableColumnPtr createColumn() const override;

    Field getDefault() const override;

    /// Compares name, parameters, and argument types.
    /// When ignore_variant is false (default), also compares the state variant (Aggregation vs Window).
    static bool strictEquals(const DataTypePtr & lhs_state_type, const DataTypePtr & rhs_state_type, bool ignore_variant = false);

    /// Same as equals() but ignores the state variant (Aggregation vs Window).
    bool equalsIgnoringVariant(const IDataType & rhs) const;

    bool equals(const IDataType & rhs) const override;
    void updateHashImpl(SipHash & hash) const override;

    bool isParametric() const override { return true; }
    bool haveSubtypes() const override { return false; }
    bool shouldAlignRightInPrettyFormats() const override { return false; }

    SerializationPtr doGetSerialization(const SerializationInfoSettings &) const override;
    bool supportsSparseSerialization() const override { return false; }

    bool isVersioned() const;

    /// Version is not empty only if it was parsed from AST or implicitly cast to 0 or version according
    /// to server revision.
    /// It is ok to have an empty version value here - then for serialization a default (latest)
    /// version is used. This method is used to force some zero version to be used instead of
    /// default, or to set version for serialization in distributed queries.
    void setVersion(size_t version_, bool if_empty) const
    {
        if (version && if_empty)
            return;

        version = version_;
    }

    void updateVersionFromRevision(size_t revision, bool if_empty) const;
};

void setVersionToAggregateFunctions(DataTypePtr & type, bool if_empty, std::optional<size_t> revision = std::nullopt);

/// Checks type of any nested type is DataTypeAggregateFunction.
bool hasAggregateFunctionType(const DataTypePtr & type);

/// Same as `hasAggregateFunctionType`, but for a parsed type name instead of a resolved type, so that
/// a name that came from a file can be gated before `DataTypeFactory` looks the aggregate function up.
/// `SimpleAggregateFunction` does not count: it holds an ordinary value, not a serialized state.
bool astHasAggregateFunctionType(const ASTPtr & ast);

/// True when the name of `type` cannot be recovered from a Parquet or Iceberg schema alone, so it has
/// to be recorded next to the data: `AggregateFunction` or `SimpleAggregateFunction` anywhere inside.
bool needsClickHouseTypeAnnotation(const DataTypePtr & type);

/// The type name recorded in the Parquet `clickhouse.column_types` / Iceberg `clickhouse.type`
/// annotation. Same as `type->getName()`, except that every `AggregateFunction` state, nested ones
/// included, keeps its version: states are serialized with exactly `getVersion()`, and getName() drops
/// a zero version, so a state pinned to version 0 would be rebuilt with the default one and misread.
String getClickHouseTypeAnnotationName(const DataTypePtr & type);

/// Checks that the annotated type name describes the same physical data as `derived`, the type the
/// Parquet or Iceberg schema derives on its own. `Nullable` and `LowCardinality` are ignored at every
/// level: neither format derives a `LowCardinality`, and either side may carry a `Nullable` the other
/// does not.
///
/// An `AggregateFunction` position must sit over a `String`. Every other position is checked only as
/// strictly as the format allows, so that a stale or crafted annotation cannot re-type a column to
/// anything of the same nesting shape:
///  - Lenient (the default, and all Iceberg can do): only the nesting structure has to match, since
///    Iceberg cannot express most ClickHouse types (`UInt64` is stored as `long`, derives as `Int64`).
///  - Strict (the Parquet reader): the annotated type must also be one the parquet writer maps to what
///    the file holds. That mapping is not injective, so it is a directed relation, not an equality.
bool annotatedTypeMatchesDerived(const DataTypePtr & annotated, const DataTypePtr & derived, bool strict = false);

}
