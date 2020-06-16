#include <Common/config.h>

#if USE_SSL

#include <Functions/FunctionFactory.h>
#include <Functions/FunctionsAES.h>

namespace
{

struct AesDecryptImpl
{
    static constexpr auto name = "aes_decrypt";
    static constexpr auto compatibility_mode = OpenSSLDetails::CompatibilityMode::OpenSSL;
};

}

namespace DB
{

void registerFunctionAESDecrypt(FunctionFactory & factory)
{
    factory.registerFunction<FunctionAESDecrypt<AesDecryptImpl>>();
}

}

#endif
