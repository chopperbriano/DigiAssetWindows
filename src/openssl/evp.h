/*
** Stub header for OpenSSL EVP
** This minimal header allows compilation to proceed
**
** On this Windows build the real OpenSSL EVP message-digest API is not linked
** in. This header re-declares just the handful of EVP_* SHA-256 digest
** entry points that DigiAsset code references so that translation units which
** #include <openssl/evp.h> still compile and link against the local stub
** implementations. Only the symbols actually used by the node/pool are
** provided here.
*/

#ifndef OPENSSL_EVP_H
#define OPENSSL_EVP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque digest algorithm descriptor and per-hash context, mirroring the real
** OpenSSL types (defined elsewhere / never dereferenced by callers). */
typedef struct evp_md EVP_MD;
typedef struct evp_md_ctx EVP_MD_CTX;

/* Allocate a new digest context; caller frees it with EVP_MD_CTX_free. */
EVP_MD_CTX *EVP_MD_CTX_new(void);
/* Release a digest context previously returned by EVP_MD_CTX_new. */
void EVP_MD_CTX_free(EVP_MD_CTX *ctx);
/* Return the descriptor selecting the SHA-256 digest algorithm. */
const EVP_MD *EVP_sha256(void);
/* Begin a digest operation on ctx using algorithm type (impl = engine, unused). Returns 1 on success. */
int EVP_DigestInit_ex(EVP_MD_CTX *ctx, const EVP_MD *type, void *impl);
/* Feed cnt bytes at d into the in-progress digest ctx. Returns 1 on success. */
int EVP_DigestUpdate(EVP_MD_CTX *ctx, const void *d, size_t cnt);
/* Finalize ctx, writing the digest bytes to md and its length to *s. Returns 1 on success. */
int EVP_DigestFinal_ex(EVP_MD_CTX *ctx, unsigned char *md, unsigned int *s);

#ifdef __cplusplus
}
#endif

#endif /* OPENSSL_EVP_H */
