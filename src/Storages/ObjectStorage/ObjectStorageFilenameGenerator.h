#pragma once

#include <string>
#include <Functions/generateSnowflakeID.h>

namespace DB
{

struct ObjectStorageFilenameGenerator
{
    virtual ~ObjectStorageFilenameGenerator() = default;
    virtual std::string generate() const = 0;
};

struct NoOpObjectStorageFilenameGenerator : ObjectStorageFilenameGenerator
{
    std::string generate() const override
    {
        return "";
    }
};

struct SnowflakeObjectStorageFilenameGenerator : ObjectStorageFilenameGenerator
{
    std::string generate() const override
    {
        return std::to_string(generateSnowflakeID());
    }

private:
    std::string file_format;
};

struct PredefinedObjectStorageFilenameGenerator : public ObjectStorageFilenameGenerator
{
    explicit PredefinedObjectStorageFilenameGenerator(const std::string & filename_)
        : file_name(filename_) {}

    std::string generate() const override
    {
        return file_name;
    }

private:
    std::string file_name;
};

}
