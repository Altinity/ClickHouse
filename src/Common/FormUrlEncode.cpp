#include <Common/FormUrlEncode.h>

#include <Poco/URI.h>

namespace DB
{

std::string formUrlEncode(const std::string & value)
{
    std::string encoded;
    Poco::URI::encode(value, "!$&'()*+,;=:@/?", encoded);
    return encoded;
}

}
