#ifndef AWS_LC_2_0_0_API_SHIM_FOR_OPENSSL_3_INCLUDED
#define AWS_LC_2_0_0_API_SHIM_FOR_OPENSSL_3_INCLUDED

#include <openssl/evp.h>
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

#endif