#pragma once

#include <string>

namespace DB
{

/// Percent-encodes one `application/x-www-form-urlencoded` value. `Poco::URI::encode` takes the
/// set of reserved characters as its second argument and leaves the sub-delimiters alone when that
/// set is empty, so `&`, `=` or `+` inside a value would otherwise break the form.
std::string formUrlEncode(const std::string & value);

}
