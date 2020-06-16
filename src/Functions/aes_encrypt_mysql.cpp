#include <Common/config.h>

#if USE_SSL

#include <Functions/FunctionFactory.h>
#include <Functions/FunctionsAES.h>

namespace
{

struct AesEncryptMySQLModeImpl
{
    static constexpr auto name = "aes_encrypt_mysql";
    static constexpr auto compatibility_mode = OpenSSLDetails::CompatibilityMode::MySQL;
};

}

namespace DB
{

void registerFunctionAESEncryptMysql(FunctionFactory & factory)
{
    factory.registerFunction<FunctionAESEncrypt<AesEncryptMySQLModeImpl>>();
}

}

#endif
