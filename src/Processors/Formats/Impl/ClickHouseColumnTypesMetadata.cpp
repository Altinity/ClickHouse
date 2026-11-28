#include <Processors/Formats/Impl/ClickHouseColumnTypesMetadata.h>

#include <Common/Exception.h>
#include <Formats/ClickHouseTypeAnnotationUtils.h>

#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Stringifier.h>

#include <sstream>

namespace DB
{

namespace ErrorCodes
{
    extern const int INCORRECT_DATA;
}

namespace
{

constexpr auto KEY_VERSION = "version";
constexpr auto KEY_COLUMNS = "columns";
constexpr Int64 VERSION = 1;

}

String serializeClickHouseColumnTypesMetadata(const NamesAndTypesList & columns)
{
    Poco::JSON::Object::Ptr column_types = new Poco::JSON::Object;
    for (const auto & [column_name, type] : columns)
    {
        if (auto annotation = getAggregateFunctionTypeAnnotation(type); !annotation.empty())
            column_types->set(column_name, annotation);
    }

    if (column_types->size() == 0)
        return {};

    Poco::JSON::Object::Ptr metadata = new Poco::JSON::Object;
    metadata->set(KEY_VERSION, VERSION);
    metadata->set(KEY_COLUMNS, column_types);

    std::ostringstream // STYLE_CHECK_ALLOW_STD_STRING_STREAM
        oss;
    Poco::JSON::Stringifier::stringify(metadata, oss);
    return oss.str();
}

std::unordered_map<String, String> parseClickHouseColumnTypesMetadata(const String & value)
{
    std::unordered_map<String, String> result;
    try
    {
        Poco::JSON::Parser parser;
        const auto object = parser.parse(value).extract<Poco::JSON::Object::Ptr>();
        const auto version = object->getValue<Int64>(KEY_VERSION);
        if (version != VERSION)
            throw Exception(
                ErrorCodes::INCORRECT_DATA,
                "Unsupported version {} of the `{}` key-value metadata of the file, supported version is {}",
                version,
                clickhouse_column_types_key,
                VERSION);

        const auto columns = object->getObject(KEY_COLUMNS);
        if (!columns)
            throw Exception(
                ErrorCodes::INCORRECT_DATA,
                "The `{}` key-value metadata of the file has no `{}` object",
                clickhouse_column_types_key,
                KEY_COLUMNS);

        for (const auto & name : columns->getNames())
            result[name] = columns->getValue<String>(name);
    }
    catch (Exception & e)
    {
        e.addMessage("while parsing the `{}` key-value metadata of the file", clickhouse_column_types_key);
        throw;
    }
    catch (const Poco::Exception & e)
    {
        throw Exception(
            ErrorCodes::INCORRECT_DATA,
            "Cannot parse the `{}` key-value metadata of the file: {}",
            clickhouse_column_types_key,
            e.displayText());
    }
    return result;
}

}
