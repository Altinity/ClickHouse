#include <Common/config.h>

#if USE_SSL

#include <Functions/FunctionFactory.h>
#include <Functions/FunctionsAES.h>

namespace
{

struct AesEncryptImpl
{
    static constexpr auto name = "aes_encrypt";
    static constexpr auto compatibility_mode = OpenSSLDetails::CompatibilityMode::OpenSSL;
};

}

namespace DB
{

void registerFunctionAESEncrypt(FunctionFactory & factory)
{
    factory.registerFunction<FunctionAESEncrypt<AesEncryptImpl>>();
}

}

#endif
