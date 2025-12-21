#include <Functions/FunctionsAES.h>
#include <Interpreters/Context.h>
#include <Common/OpenSSLHelpers.h>

#if USE_SSL

#include <openssl/evp.h>
#include <openssl/err.h>

#include <string>
#include <cassert>
#include <array>
#include <cstring>
#include <string_view>

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
    throw Exception(ErrorCodes::OPENSSL_ERROR, "{} failed: {}", error_message, getOpenSSLErrors());
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
    using CipherGetter = const EVP_CIPHER *(*)();
    struct Entry
    {
        std::string_view name;
        CipherGetter getter;
    };

    /// Fast path to avoid OBJ/namemap lookups in EVP_get_cipherbyname().
    /// Statically linked OpenSSL provides direct getters for common AES modes.
    static constexpr std::array<Entry, 21> fast_paths{{
        {"aes-128-cbc", &EVP_aes_128_cbc},
        {"aes-192-cbc", &EVP_aes_192_cbc},
        {"aes-256-cbc", &EVP_aes_256_cbc},
        {"aes-128-ecb", &EVP_aes_128_ecb},
        {"aes-192-ecb", &EVP_aes_192_ecb},
        {"aes-256-ecb", &EVP_aes_256_ecb},
        {"aes-128-cfb", &EVP_aes_128_cfb},
        {"aes-192-cfb", &EVP_aes_192_cfb},
        {"aes-256-cfb", &EVP_aes_256_cfb},
        {"aes-128-cfb8", &EVP_aes_128_cfb8},
        {"aes-192-cfb8", &EVP_aes_192_cfb8},
        {"aes-256-cfb8", &EVP_aes_256_cfb8},
        {"aes-128-cfb1", &EVP_aes_128_cfb1},
        {"aes-192-cfb1", &EVP_aes_192_cfb1},
        {"aes-256-cfb1", &EVP_aes_256_cfb1},
        {"aes-128-ofb", &EVP_aes_128_ofb},
        {"aes-192-ofb", &EVP_aes_192_ofb},
        {"aes-256-ofb", &EVP_aes_256_ofb},
        {"aes-128-ctr", &EVP_aes_128_ctr},
        {"aes-192-ctr", &EVP_aes_192_ctr},
        {"aes-256-ctr", &EVP_aes_256_ctr},
    }};

    for (const auto & entry : fast_paths)
    {
        if (cipher_name.size == entry.name.size()
            && std::memcmp(cipher_name.data, entry.name.data(), entry.name.size()) == 0)
            return entry.getter();
    }

    /// Fallback keeps compatibility with custom providers/algorithms.
    return EVP_get_cipherbyname(cipher_name.toString().c_str());
}

}

}

#endif
