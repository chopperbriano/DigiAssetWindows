// openssl_stubs.cpp
//
// Minimal stand-in implementations of the OpenSSL BIO functions that DigiAsset
// code links against on this Windows build, where real OpenSSL is not present.
// It provides just enough of the BIO memory-buffer / base64-filter surface for
// code that decodes buffers to compile and run. NOTE: these are shallow stubs
// - BIO_f_base64 and BIO_push are no-ops, so no base64 decoding actually
// happens; BIO_read simply copies raw bytes out of the memory buffer supplied
// to BIO_new_mem_buf. Used by both the node and pool server builds.

#include "openssl/bio.h"
#include <cstring>
#include <cstdlib>

// Internal state backing a fake BIO: the source buffer, its length, the current
// read cursor, and any flags set via BIO_set_flags. A BIO* is really a pointer
// to one of these reinterpret_cast'd to the opaque BIO type.
struct BioHandle {
    const void* buf;
    int len;
    int pos;
    int flags;
};

// Return the base64 filter method. Stubbed to nullptr since no real filtering
// is performed; callers only pass it to BIO_new/BIO_push which ignore it.
BIO_METHOD* BIO_f_base64(void) {
    return nullptr; // stub
}

// Allocate a fresh empty BioHandle regardless of method. Free with BIO_free_all.
BIO* BIO_new(BIO_METHOD* method) {
    (void)method;
    return reinterpret_cast<BIO*>(new BioHandle());
}

// Wrap an existing read-only memory region [buf, buf+len) in a BIO for reading.
// Does not copy the data; the caller must keep buf alive for the BIO's lifetime.
BIO* BIO_new_mem_buf(const void* buf, int len) {
    BioHandle* h = new BioHandle();
    h->buf = buf;
    h->len = len;
    h->pos = 0;
    h->flags = 0;
    return reinterpret_cast<BIO*>(h);
}

// Chain append below bio. Stubbed to a no-op that just returns bio, so filter
// chaining (e.g. a base64 filter over a memory BIO) has no effect here.
BIO* BIO_push(BIO* bio, BIO* append) {
    (void)bio;
    (void)append;
    return bio; // stub
}

// Store the given flags on the handle (e.g. BIO_FLAGS_BASE64_NO_NL). Recorded
// but not acted upon. Returns 1.
int BIO_set_flags(BIO* bio, int flags) {
    if (bio) {
        reinterpret_cast<BioHandle*>(bio)->flags = flags;
    }
    return 1;
}

// Copy up to len bytes from the handle's buffer into data, advancing the read
// cursor. Returns the number of bytes actually copied (0 at end of buffer or if
// bio is null). Reads raw bytes - no base64 decoding is performed.
int BIO_read(BIO* bio, void* data, int len) {
    if (!bio) return 0;
    BioHandle* h = reinterpret_cast<BioHandle*>(bio);
    int remaining = h->len - h->pos;
    int to_read = (len < remaining) ? len : remaining;
    if (to_read > 0) {
        memcpy(data, static_cast<const char*>(h->buf) + h->pos, to_read);
        h->pos += to_read;
    }
    return to_read;
}

// Delete the handle behind bio. Because BIO_push is a no-op there is never an
// actual chain to walk, so this frees the single handle. Returns 1.
int BIO_free_all(BIO* bio) {
    if (bio) {
        delete reinterpret_cast<BioHandle*>(bio);
    }
    return 1;
}