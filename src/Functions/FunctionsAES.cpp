#include <Functions/FunctionsAES.h>
#include <Interpreters/Context.h>

#if USE_SSL

#include <openssl/evp.h>
#include <openssl/err.h>

#include <string>
#include <cassert>

namespace DB
{
namespace ErrorCodes
{
    extern const int OPENSSL_ERROR;
}


namespace OpenSSLDetails
{
void onError(std::string error_message)
{
    throw Exception(ErrorCodes::OPENSSL_ERROR, "{}. OpenSSL error code: {}", error_message, ERR_get_error());
}

StringRef foldEncryptionKeyInMySQLCompatitableMode(size_t cipher_key_size, StringRef key, std::array<char, EVP_MAX_KEY_LENGTH> & folded_key)
{
    assert(cipher_key_size <= EVP_MAX_KEY_LENGTH);
    memcpy(folded_key.data(), key.data, cipher_key_size);

    for (size_t i = cipher_key_size; i < key.size; ++i)
    {
        folded_key[i % cipher_key_size] ^= key.data[i];
    }

    return StringRef(folded_key.data(), cipher_key_size);
}

const EVP_CIPHER * getCipherByName(StringRef cipher_name)
{
    // OPTIMIZATION: Use cached EVP_CIPHER_fetch() for better performance in OpenSSL 3.x
    return OpenSSLOptimized::CipherCache::instance().getCipher(
        std::string_view(cipher_name.data, cipher_name.size));
}

}

}

#endif
