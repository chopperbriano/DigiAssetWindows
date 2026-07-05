//
// Created by mctrivia on 30/03/24.
//
// C interface to a RIPEMD-160 hash function. Paired with SHA-256 to compute the
// HASH160 (RIPEMD160(SHA256(x))) used for DigiByte address and script hashing
// when the node parses transactions and DigiAsset data. Declared with C linkage
// so it can be implemented in / linked against C code.
//

#ifndef RIPEMD160_H
#define RIPEMD160_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Compute the 20-byte RIPEMD-160 digest of msg (msg_len bytes) into hash, which
// the caller must size to at least 20 bytes.
void ripemd160(const uint8_t* msg, uint32_t msg_len, uint8_t* hash);

#ifdef __cplusplus
}
#endif

#endif // RIPEMD160_H
