#include <Utils.h>

#include <Disks/DiskObjectStorage/DiskObjectStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Common/Exception.h>

#include <boost/program_options/parsers.hpp>

namespace DB::ErrorCodes
{
extern const int BAD_ARGUMENTS;
}

DB::ContentAddressedMetadataStorage & DB::contentAddressedStorageOf(IDisk & disk, std::string_view command)
{
    auto * dos = dynamic_cast<DiskObjectStorage *>(&disk);
    if (!dos)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "{}: '{}' is not an object-storage disk", command, disk.getName());
    auto * ca = dynamic_cast<ContentAddressedMetadataStorage *>(dos->getMetadataStorage().get());
    if (!ca)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "{}: disk '{}' is not content-addressed", command, disk.getName());
    return *ca;
}

std::vector<std::string> splitOnUnquotedSemicolons(const std::string & input)
{
    std::vector<std::string> results;
    std::string current;
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool escaped = false; // Tracks whether the *previous* character was a backslash (escape)

    for (const char c : input)
    {
        if (escaped)
        {
            // The current character is escaped; just add it literally to 'current'
            current.push_back(c);
            escaped = false; // Reset escape state
        }
        else
        {
            // We're NOT currently escaping. Check what we have:
            if (c == '\\')
            {
                // A backslash can start an escape sequence unless we are in single quotes.
                // (POSIX shells treat backslash inside single quotes as literal)
                if (!in_single_quote)
                {
                    escaped = true;
                }
                else
                {
                    // In single quotes, backslash is literal:
                    current.push_back(c);
                }
            }
            else if (c == '\'' && !in_double_quote)
            {
                // Toggle single-quote mode
                in_single_quote = !in_single_quote;
                current.push_back(c);
            }
            else if (c == '\"' && !in_single_quote)
            {
                // Toggle double-quote mode
                in_double_quote = !in_double_quote;
                current.push_back(c);
            }
            else if (c == ';' && !in_single_quote && !in_double_quote)
            {
                // We found an unquoted semicolon -> finalize the current segment
                results.push_back(current);
                current.clear();
            }
            else
            {
                // Normal character, just add it
                current.push_back(c);
            }
        }
    }

    // Add whatever remains in 'current'
    if (!current.empty())
    {
        results.push_back(current);
    }

    return results;
}

bool queryRunsCasMigratePool(const std::string & query)
{
    size_t commands = 0;
    bool runs_tool = false;
    for (const auto & subquery : splitOnUnquotedSemicolons(query))
    {
        const auto words = boost::program_options::split_unix(subquery, std::string(disks_word_break_characters));
        commands += !words.empty();
        runs_tool = runs_tool || (!words.empty() && words.front() == "cas-migrate-pool");
    }
    if (runs_tool && commands > 1)
        throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
            "cas-migrate-pool must be the only command of --query: it keeps every content-addressed disk of the process "
            "from opening its pool, so no other command can run beside it");
    return runs_tool;
}
