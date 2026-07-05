/*
** Stub header for OpenSSL BIO
** This minimal header allows compilation to proceed
**
** Minimal drop-in replacement for OpenSSL's <openssl/bio.h> exposing only the
** handful of BIO (basic I/O abstraction) symbols this node actually uses -
** enough to chain a base64 filter onto a memory buffer for base64 decoding.
** It declares the opaque BIO/BIO_METHOD types, the relevant flag constants, and
** the BIO_* functions; the real implementations are provided by the linked
** OpenSSL library at build time. This shim just lets the code compile without
** pulling in OpenSSL's full header set.
*/

#ifndef OPENSSL_BIO_H
#define OPENSSL_BIO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handles to an OpenSSL BIO object and a BIO method (filter/source type). */
typedef struct bio_st BIO;
typedef struct bio_method_st BIO_METHOD;

#define BIO_FLAGS_READ  0x01
#define BIO_FLAGS_WRITE 0x02
#define BIO_FLAGS_BASE64_NO_NL 0x100

/* Return the base64 filter BIO method (used to base64 encode/decode a stream). */
BIO_METHOD* BIO_f_base64(void);
/* Create a new BIO using the given method. */
BIO* BIO_new(BIO_METHOD* method);
/* Create a read-only memory BIO backed by the supplied buffer of len bytes. */
BIO* BIO_new_mem_buf(const void* buf, int len);
/* Chain append below bio, returning the head of the resulting BIO chain. */
BIO* BIO_push(BIO* bio, BIO* append);
/* Set control flags on bio (e.g. BIO_FLAGS_BASE64_NO_NL). */
int BIO_set_flags(BIO* bio, int flags);
/* Read up to len bytes from bio into data; returns bytes read or <=0. */
int BIO_read(BIO* bio, void* data, int len);
/* Free an entire BIO chain starting at bio. */
int BIO_free_all(BIO* bio);

#ifdef __cplusplus
}
#endif

#endif /* OPENSSL_BIO_H */
