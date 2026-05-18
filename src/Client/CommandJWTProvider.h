#pragma once

#if USE_JWT_CPP && USE_SSL

#include <Client/JWTProvider.h>
#include <string>

namespace DB
{

inline constexpr int DEFAULT_JWT_COMMAND_TIMEOUT_SECONDS = 30;

class CommandJWTProvider : public JWTProvider
{
public:
    CommandJWTProvider(std::string command_, int timeout_seconds_);

    std::string getJWT() override;

private:
    std::string command;
    int timeout_seconds;
};

}

#endif
