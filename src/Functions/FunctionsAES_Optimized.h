#pragma once

#include "config.h"

#if USE_SSL

#include <openssl/evp.h>
#include <openssl/err.h>
#include <array>
#include <mutex>
#include <unordered_map>

namespace DB
{

namespace ErrorCodes
{
    extern const int OPENSSL_ERROR;
}

namespace OpenSSLOptimized
{

/// Thread-local context pool to avoid repeated allocation/deallocation
class EVPContextPool
{
public:
    struct ContextWrapper
    {
        EVP_CIPHER_CTX * ctx = nullptr;
        const EVP_CIPHER * cached_cipher = nullptr;
        
        ContextWrapper()
        {
            ctx = EVP_CIPHER_CTX_new();
            if (!ctx)
                throw Exception(ErrorCodes::OPENSSL_ERROR, "Failed to allocate EVP_CIPHER_CTX");
        }
        
        ~ContextWrapper()
        {
            if (ctx)
                EVP_CIPHER_CTX_free(ctx);
        }
        
        // Non-copyable
        ContextWrapper(const ContextWrapper&) = delete;
        ContextWrapper& operator=(const ContextWrapper&) = delete;
        
        void reset()
        {
            if (ctx)
                EVP_CIPHER_CTX_reset(ctx);
        }
    };
    
    static ContextWrapper& getContext()
    {
        thread_local ContextWrapper context;
        return context;
    }
};

/// Cache for EVP_CIPHER lookups using EVP_CIPHER_fetch for better performance
class CipherCache
{
private:
    struct CacheEntry
    {
        EVP_CIPHER * cipher = nullptr;
        std::string name;
        
        ~CacheEntry()
        {
            if (cipher)
                EVP_CIPHER_free(cipher);
        }
    };
    
    std::unordered_map<std::string, std::unique_ptr<CacheEntry>> cache;
    std::mutex mutex;
    
public:
    ~CipherCache() = default;
    
    const EVP_CIPHER * getCipher(std::string_view cipher_name)
    {
        std::string name_str(cipher_name);
        
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = cache.find(name_str);
            if (it != cache.end())
                return it->second->cipher;
        }
        
        // Fetch cipher outside of lock to minimize lock hold time
        // EVP_CIPHER_fetch is thread-safe and works better with OpenSSL 3.x providers
        EVP_CIPHER * cipher = EVP_CIPHER_fetch(nullptr, name_str.c_str(), nullptr);
        
        if (!cipher)
            return nullptr;
        
        {
            std::lock_guard<std::mutex> lock(mutex);
            // Double-check in case another thread added it
            auto it = cache.find(name_str);
            if (it != cache.end())
            {
                EVP_CIPHER_free(cipher); // Free the one we just fetched
                return it->second->cipher;
            }
            
            auto entry = std::make_unique<CacheEntry>();
            entry->cipher = cipher;
            entry->name = name_str;
            
            const EVP_CIPHER * result = cipher;
            cache[name_str] = std::move(entry);
            return result;
        }
    }
    
    static CipherCache& instance()
    {
        static CipherCache cache;
        return cache;
    }
};

/// Fast cipher lookup with caching
inline const EVP_CIPHER * getCipherByNameFast(std::string_view cipher_name)
{
    return CipherCache::instance().getCipher(cipher_name);
}

} // namespace OpenSSLOptimized

} // namespace DB

#endif // USE_SSL
