#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace DB
{
class IDisk;
class ContentAddressedMetadataStorage;

/// The content-addressed metadata storage of `disk`, owned by `disk`. Throws `BAD_ARGUMENTS`, prefixed with `command`,
/// when `disk` is not an object-storage disk or its metadata is not content-addressed.
ContentAddressedMetadataStorage & contentAddressedStorageOf(IDisk & disk, std::string_view command);
}

/// Separators between the words of a `clickhouse-disks` command.
inline constexpr std::string_view disks_word_break_characters = " \t\v\f\a\b\r\n";

std::vector<std::string> splitOnUnquotedSemicolons(const std::string & input);

/// Whether one of the `;`-separated commands of `query` is `cas-migrate-pool`. Throws `BAD_ARGUMENTS` when it runs
/// beside any other command: the tool keeps every content-addressed disk of the process from opening its pool, so
/// no other command could work there.
bool queryRunsCasMigratePool(const std::string & query);
