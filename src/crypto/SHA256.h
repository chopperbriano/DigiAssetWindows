/**
* MIT License
*
* Copyright (c) 2021 Jérémy LAMBERT (SystemGlitch)
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
 */
//
// Self-contained SHA-256 hash implementation (third-party, MIT). Provides the
// SHA-256 primitive used across the node and pool for content addressing and
// integrity checks (e.g. hashing IPFS content and verifying downloads). No
// external crypto library dependency.
//

#ifndef SHA256_H
#define SHA256_H

#include <string>
#include <array>
#include <cstdint>

// Incremental SHA-256 hasher: construct, feed data via update() one or more
// times, then call digest() once to finalise and read the 32-byte hash.
// A single instance is not reusable after digest() (padding mutates state).
class SHA256 {

public:
    SHA256();
    // Feed a raw byte buffer into the running hash.
    void update(const uint8_t * data, size_t length);
    // Feed a string's bytes into the running hash.
    void update(const std::string &data);
    // Finalise: apply padding, then return the 32-byte digest.
    std::array<uint8_t, 32> digest();

    // Format a digest as a 64-character lowercase hex string.
    static std::string toString(const std::array<uint8_t, 32> & digest);

private:
    uint8_t  m_data[64];
    uint32_t m_blocklen;
    uint64_t m_bitlen;
    uint32_t m_state[8]; //A, B, C, D, E, F, G, H

    static constexpr std::array<uint32_t, 64> K = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
            0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
            0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
            0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
            0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
            0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
            0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
            0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
            0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };

    // SHA-256 round primitives (as defined by the FIPS 180-4 spec):
    static uint32_t rotr(uint32_t x, uint32_t n);              // rotate right
    static uint32_t choose(uint32_t e, uint32_t f, uint32_t g);   // Ch function
    static uint32_t majority(uint32_t a, uint32_t b, uint32_t c); // Maj function
    static uint32_t sig0(uint32_t x);                            // small sigma 0
    static uint32_t sig1(uint32_t x);                            // small sigma 1
    // Compress the current 64-byte block in m_data into m_state (the 64 rounds).
    void transform();
    // Append the '1' bit, zero padding, and 64-bit length; run final transform(s).
    void pad();
    // Serialise m_state into the 32-byte hash in big-endian byte order.
    void revert(std::array<uint8_t, 32> & hash);
};

#endif