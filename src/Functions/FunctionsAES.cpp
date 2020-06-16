#include <Functions/FunctionsAES.h>

#if USE_SSL

#include <openssl/err.h>

#include <string>

namespace DB
{
namespace ErrorCodes
{
    extern const int OPENSSL_ERROR;
}
}

namespace OpenSSLDetails
{
void onError(std::string prefix)
{
    std::string error_message(std::move(prefix));
    if (error_message.length() != 0)
        error_message += ": ";

    ERR_print_errors_cb([](const char *str, size_t len, void * msg) -> auto
    {
        reinterpret_cast<std::string *>(msg)->append(str, len);
        return 1;
    }, &error_message);

    throw DB::Exception(error_message, DB::ErrorCodes::OPENSSL_ERROR);
}

StringRef foldEncryptionKeyInMySQLCompatitableMode(size_t cipher_key_size, const StringRef & key, std::array<char, EVP_MAX_KEY_LENGTH> & folded_key)
{
    memcpy(folded_key.data(), key.data, cipher_key_size);

    for (size_t i = cipher_key_size; i < key.size; ++i)
    {
        folded_key[i % cipher_key_size] ^= key.data[i];
    }

    return StringRef(folded_key.data(), cipher_key_size);
}

const EVP_CIPHER * getCipherByName(const StringRef & cipher_name)
{
    const auto *evp_cipher = EVP_get_cipherbyname(cipher_name.data);
    if (evp_cipher == nullptr)
    {
        // For some reasons following ciphers can't be found by name.
        if (cipher_name == "aes-128-cfb128")
            evp_cipher = EVP_aes_128_cfb128();
        else if (cipher_name == "aes-192-cfb128")
            evp_cipher = EVP_aes_192_cfb128();
        else if (cipher_name == "aes-256-cfb128")
            evp_cipher = EVP_aes_256_cfb128();
    }

    return evp_cipher;
}

}

#endif
