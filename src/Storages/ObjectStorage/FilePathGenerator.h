#pragma once

#include <string>
#include <Storages/PartitionedSink.h>
#include <Storages/ObjectStorage/ObjectStorageFilenameGenerator.h>

namespace DB
{
    struct ObjectStorageFilePathGenerator
    {
        virtual ~ObjectStorageFilePathGenerator() = default;
        virtual std::string getWritingPath(const std::string & partition_id, std::optional<std::string> filename_override = {}) const = 0;
        virtual std::string getReadingPath() const = 0;
    };

    struct ObjectStorageWildcardFilePathGenerator : ObjectStorageFilePathGenerator
    {
        explicit ObjectStorageWildcardFilePathGenerator(const std::string & raw_path_) : raw_path(raw_path_) {}

        std::string getWritingPath(const std::string & partition_id, std::optional<std::string> /**/ = {}) const override
        {
            return PartitionedSink::replaceWildcards(raw_path, partition_id);
        }

        std::string getReadingPath() const override
        {
            return raw_path;
        }

    private:
        std::string raw_path;

    };

    struct ObjectStorageAppendFilePathGenerator : ObjectStorageFilePathGenerator
    {
        explicit ObjectStorageAppendFilePathGenerator(
            const std::string & raw_path_,
            const std::string & file_format_,
            const std::shared_ptr<ObjectStorageFilenameGenerator> & filename_generator_)
        : raw_path(raw_path_), file_format(file_format_), filename_generator(filename_generator_){}

        std::string getWritingPath(const std::string & partition_id, std::optional<std::string> filename_override) const override
        {
            return raw_path + "/" + partition_id + "/"  + (filename_override ? *filename_override : filename_generator->generate()) + "." + file_format;
        }

        std::string getReadingPath() const override
        {
            return raw_path + "**." + file_format;
        }

    private:
        std::string raw_path;
        std::string file_format;

        std::shared_ptr<ObjectStorageFilenameGenerator> filename_generator;
    };

}
