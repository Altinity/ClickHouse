#include <Common/config.h>

#if USE_SSL

#include <Functions/FunctionFactory.h>
#include <Functions/FunctionsAES.h>

namespace
{

struct AesDecryptMySQLModeImpl
{
    static constexpr auto name = "aes_decrypt_mysql";
    static constexpr auto compatibility_mode = OpenSSLDetails::CompatibilityMode::MySQL;
};

}

namespace DB
{

void registerFunctionAESDecryptMysql(FunctionFactory & factory)
{
    factory.registerFunction<FunctionAESDecrypt<AesDecryptMySQLModeImpl>>();
}

}

#endif
