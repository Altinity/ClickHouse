#ifndef AWS_LC_2_0_0_API_SHIM_FOR_OPENSSL_3_INCLUDED
#define AWS_LC_2_0_0_API_SHIM_FOR_OPENSSL_3_INCLUDED

#include <openssl/evp.h>
#include <openssl/hmac.h> // HMAC is used by librdkafka
#include <openssl/ssl.h>
// #include <openssl/types.h>
#include <assert.h>

/* Nudges some of the code into using older API that is fully supported by AWS-LC.
 */
#define OPENSSL_IS_BORINGSSL 1

/* Collection of various functions that are present in OpenSSL 3+ API,
    but are missing from AWS-LC API (OpenSSL 1.1.1-style API).
    To simplify porting and minimize code changes at other places,
    those missing functions/defines/constants will be defined here
    if there are trivial way to do it.

    ALSO, this header can be included from both C++ and C code,
    so make sure that there are no non-compatible C++ features.
 */

/* This type is missing in AWS-LC, moreover, it is not used anywhere in CH code
    (typically nullptr is passed to EVP_CIPHER_fetch).

    If the type was actually used anywhere this definition would cause
    a build-time error rather than a run-time or even change of behaviour.
*/
#define OSSL_LIB_CTX void

/*  All flags are defined as 0 AWS-LC's /include/openssl/ssl.h with a note:
    "The following flags do nothing"
    So I assume it is safe to do the same for the flag that is missing from ssl.h too
*/
#define SSL_OP_IGNORE_UNEXPECTED_EOF 0

/* There are two types of cipher fetching in OpenSSL 3+:
    implicit (`EVP_get_cipherbyname`) and explicit (`EVP_CIPHER_fetch`).

    Those types are necessary for OpenSSL's strategy of loading
    some of the ciphers from plugins - shared (dynamic) libraries,
    which are called providers in OpenSSL's lingo.

    BUT since AWS-LC doesn't support explicit fetching and doesn't
    load anything from shared libraries, those methods are interchangeble.

    It is a little bit sketchy that `const`-ness of the return value
    has to be removed, but it looks like all of the APIs used in practice
    are taking `const EVP_CIPHER *` param.
 */
inline EVP_CIPHER *EVP_CIPHER_fetch(OSSL_LIB_CTX *ctx,	const char *algorithm,
				     const char	*properties)
{
    assert(ctx == NULL);
    assert(properties == NULL);

    return (EVP_CIPHER *)EVP_get_cipherbyname(algorithm);
}

/* No need to free, since cipher was obtained with `EVP_CIPHER_fetch` above
    (same as `EVP_get_cipherbyname`) and doesn't require de-allocation.
*/
inline void EVP_CIPHER_free(EVP_CIPHER *cipher)
{
    (void)(cipher);
}

// Required for contrib/minizip-ng/mz_crypt_openssl.c mz_crypt_init()
// since there are no dynamically loaded engines, disabling the flag is Ok:
// all engines are initialized anyway
# define OPENSSL_INIT_ENGINE_ALL_BUILTIN 0

// librdkafka uses RAND_priv_bytes, which is not defined in AWS-LC.
// However, RAND_bytes _is_ defined and is semantically equivalent.
#define RAND_priv_bytes RAND_bytes

// SSL_CTX_use_cert_and_key is used by librdkafka but not provided by AWS-LC.
// Replicates OpenSSL's behaviour: sets certificate, private key, and optionally
// the certificate chain on the SSL_CTX.
static inline int SSL_CTX_use_cert_and_key(SSL_CTX *ctx, X509 *cert,
                                           EVP_PKEY *pkey, STACK_OF(X509) *chain,
                                           int override)
{
    (void)override;
    if (SSL_CTX_use_certificate(ctx, cert) != 1)
        return 0;
    if (SSL_CTX_use_PrivateKey(ctx, pkey) != 1)
        return 0;
    if (chain && SSL_CTX_set1_chain(ctx, chain) != 1)
        return 0;
    return 1;
}

// In OpenSSL, BIO_get_ssl is a macro that calls BIO_ctrl(bio, BIO_C_GET_SSL, 0, &ssl).
// BIO's ctrl handler catches BIO_C_GET_SSL and writes the SSL pointer to the argument.
// In AWS-LC FIPS 2.0.0, the SSL BIO's ctrl handler does NOT have a case for BIO_C_GET_SSL.
// Instead, it falls through to the default case that forwards the ctrl call to
// SSL_get_rbio(ssl) which doesn't handle it properly either. As a result, ssl argument
// is never written to, leaving it as unitialized stack garbage.
// This shim reads the SSL pointer directly from BIO_get_data(bio), which is exactly
// how AWS-LC's own internal get_ssl() helper works.
#define BIO_get_ssl(b, sslp) \
    (*(SSL **)(sslp) = (SSL *)BIO_get_data(b))

// There are two issues with BIO_do_handshake.
// 1. BIO_C_DO_STATE_MACHINE is not handled by AWS-LC's SSL BIO's ctrl handler,
//    effectively creating the same issue as BIO_get_ssl.
// 2. BIO_CTRL_PUSH is not handled by AWS-LC. In OpenSSL, when
//    BIO_push(ssl_bio, transport_bio) is called, the SSL BIO's ctrl receives
//    BIO_CTRL_PUSH and internally calls SSL_set_bin(ssl, transport_bio, transport_bio)
//    to connect the SSL to the network transport. AWS-LC's SSL BIO returns -1
//    for BIO_CTRL_PUSH and does nothing. This means the SSL object has no transport
//    BIOs, so it can't read or write data, any handshake would fail.
// We fix the first one by calling SSL_do_handshake(ssl) directly.
// Fixing the second one is done lazily: on first call, it checks SSL_get_rbio(ssl)
// and if it's NULL, it calls BIO_next(b) (the transport BIO that BIO_push chained
// underneath) as the SSL's read/write BIO. The BIO_up_ref calls are necessary
// because SSL_set_bio takes ownership of the BIOs and calls BIO_free_all on them.
// This is not a 1-to-1 equivalent to OpenSSL's BIO_do_handshake, but it is
// close enough for the MongoDB driver.
static inline int BIO_do_handshake(BIO *b) {
    SSL *ssl = (SSL *)BIO_get_data(b);
    if (!ssl)
        return 0;
    if (!SSL_get_rbio(ssl) && BIO_next(b)) {
        BIO_up_ref(BIO_next(b));
        BIO_up_ref(BIO_next(b));
        SSL_set_bio(ssl, BIO_next(b), BIO_next(b));
    }
    int ret = SSL_do_handshake(ssl);
    if (ret == 1)
        return 1;
    int err = SSL_get_error(ssl, ret);
    if (err == SSL_ERROR_WANT_READ)
        BIO_set_retry_read(b);
    else if (err == SSL_ERROR_WANT_WRITE)
        BIO_set_retry_write(b);
    return ret;
}

// In OpenSSL, X509_VERIFY_PARAM_set1_host(param, name, namelen) with namelen=0
// means "use strlen(name)" to determine the length of the name. The MongoDB driver
// relies on this behaviour.
// AWS-LC explicitly rejects namelen=0 with a comment "Unlike OpenSSL, we reject
// trying to set or add an empty name". When it rejects, it sets param->poision=1,
// then the flag propagates through SSL_set1_param into SSL's verification parameters
// and causes X509_verify_cert() to fail.
// This shim replicates OpenSSL's behaviour.
static inline int X509_VERIFY_PARAM_set1_host_shim(X509_VERIFY_PARAM *param,
                                                     const char *name,
                                                     size_t namelen) {
    return X509_VERIFY_PARAM_set1_host(param, name,
                                        namelen == 0 ? strlen(name) : namelen);
}
#define X509_VERIFY_PARAM_set1_host X509_VERIFY_PARAM_set1_host_shim

// BIO_new_ssl is an OpenSSL convenience function that creates a new BIO
// wrapping a fresh SSL connection. AWS-LC provides all the building blocks
// (BIO_f_ssl, SSL_new, BIO_set_ssl) but not this wrapper.
// Used by mongodb driver.
static inline BIO *BIO_new_ssl(SSL_CTX *ctx, int client)
{
    BIO *bio = BIO_new(BIO_f_ssl());
    if (!bio)
        return NULL;

    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        BIO_free(bio);
        return NULL;
    }

    if (client)
        SSL_set_connect_state(ssl);
    else
        SSL_set_accept_state(ssl);

    BIO_set_ssl(bio, ssl, BIO_CLOSE);
    return bio;
}

#endif
