#pragma once

#include <string>

namespace DB
{

/// `Poco::URI::encode` leaves form delimiters unescaped unless they are explicitly reserved.
std::string formUrlEncode(const std::string & value);

}
