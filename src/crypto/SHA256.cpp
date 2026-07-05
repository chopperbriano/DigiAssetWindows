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
// SHA-256 algorithm implementation (third-party, MIT). Implements the FIPS 180-4
// message schedule, 64-round compression, padding, and finalisation behind the
// SHA256 class. Used throughout the node/pool for content hashing and download
// integrity verification.
//
 #include "SHA256.h"
#include <cstring>
#include <sstream>
#include <iomanip>

constexpr std::array<uint32_t, 64> SHA256::K;

// Initialise the eight working state words to the SHA-256 constants (fractional
// parts of the square roots of the first eight primes) and reset the counters.
SHA256::SHA256(): m_blocklen(0), m_bitlen(0) {
    m_state[0] = 0x6a09e667;
    m_state[1] = 0xbb67ae85;
    m_state[2] = 0x3c6ef372;
    m_state[3] = 0xa54ff53a;
    m_state[4] = 0x510e527f;
    m_state[5] = 0x9b05688c;
    m_state[6] = 0x1f83d9ab;
    m_state[7] = 0x5be0cd19;
}

// Buffer incoming bytes into the 64-byte block; each time the block fills, run
// transform() and advance the total bit-length counter. May be called repeatedly.
void SHA256::update(const uint8_t * data, size_t length) {
    for (size_t i = 0 ; i < length ; i++) {
        m_data[m_blocklen++] = data[i];
        if (m_blocklen == 64) {
            transform();

            // End of the block
            m_bitlen += 512;
            m_blocklen = 0;
        }
    }
}

void SHA256::update(const std::string &data) {
    update(reinterpret_cast<const uint8_t*> (data.c_str()), data.size());
}

// Finalise the hash: apply SHA-256 padding then serialise the state to 32 bytes.
// Call once after all update()s; the object should not be reused afterward.
std::array<uint8_t,32> SHA256::digest() {
    std::array<uint8_t,32> hash;

    pad();
    revert(hash);

    return hash;
}

uint32_t SHA256::rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

uint32_t SHA256::choose(uint32_t e, uint32_t f, uint32_t g) {
    return (e & f) ^ (~e & g);
}

uint32_t SHA256::majority(uint32_t a, uint32_t b, uint32_t c) {
    return (a & (b | c)) | (b & c);
}

uint32_t SHA256::sig0(uint32_t x) {
    return SHA256::rotr(x, 7) ^ SHA256::rotr(x, 18) ^ (x >> 3);
}

uint32_t SHA256::sig1(uint32_t x) {
    return SHA256::rotr(x, 17) ^ SHA256::rotr(x, 19) ^ (x >> 10);
}

// Core compression: expand the current 64-byte block into the 64-word message
// schedule, run the 64 SHA-256 rounds over a copy of the state, then fold the
// result back into m_state. Operates on the block already buffered in m_data.
void SHA256::transform() {
    uint32_t maj, xorA, ch, xorE, sum, newA, newE, m[64];
    uint32_t state[8];

    for (uint8_t i = 0, j = 0; i < 16; i++, j += 4) { // Split data in 32 bit blocks for the 16 first words
        m[i] = (m_data[j] << 24) | (m_data[j + 1] << 16) | (m_data[j + 2] << 8) | (m_data[j + 3]);
    }

    for (uint8_t k = 16 ; k < 64; k++) { // Remaining 48 blocks
        m[k] = SHA256::sig1(m[k - 2]) + m[k - 7] + SHA256::sig0(m[k - 15]) + m[k - 16];
    }

    for(uint8_t i = 0 ; i < 8 ; i++) {
        state[i] = m_state[i];
    }

    for (uint8_t i = 0; i < 64; i++) {
        maj   = SHA256::majority(state[0], state[1], state[2]);
        xorA  = SHA256::rotr(state[0], 2) ^ SHA256::rotr(state[0], 13) ^ SHA256::rotr(state[0], 22);

        ch = choose(state[4], state[5], state[6]);

        xorE  = SHA256::rotr(state[4], 6) ^ SHA256::rotr(state[4], 11) ^ SHA256::rotr(state[4], 25);

        sum  = m[i] + K[i] + state[7] + ch + xorE;
        newA = xorA + maj + sum;
        newE = state[3] + sum;

        state[7] = state[6];
        state[6] = state[5];
        state[5] = state[4];
        state[4] = newE;
        state[3] = state[2];
        state[2] = state[1];
        state[1] = state[0];
        state[0] = newA;
    }

    for(uint8_t i = 0 ; i < 8 ; i++) {
        m_state[i] += state[i];
    }
}

// Apply SHA-256 padding to the final partial block: append the 0x80 marker byte,
// zero-fill, and write the 64-bit total message length. If the length doesn't fit
// in the current block a second transform() is run so the length lands in a fresh
// block. Ends with a final transform() so m_state holds the complete digest.
void SHA256::pad() {

    uint64_t i = m_blocklen;
    uint8_t end = m_blocklen < 56 ? 56 : 64;

    m_data[i++] = 0x80; // Append a bit 1
    while (i < end) {
        m_data[i++] = 0x00; // Pad with zeros
    }

    if(m_blocklen >= 56) {
        transform();
        memset(m_data, 0, 56);
    }

    // Append to the padding the total message's length in bits and transform.
    m_bitlen += m_blocklen * 8;
    m_data[63] = (uint8_t)(m_bitlen & 0xFF);
    m_data[62] = (uint8_t)((m_bitlen >> 8) & 0xFF);
    m_data[61] = (uint8_t)((m_bitlen >> 16) & 0xFF);
    m_data[60] = (uint8_t)((m_bitlen >> 24) & 0xFF);
    m_data[59] = (uint8_t)((m_bitlen >> 32) & 0xFF);
    m_data[58] = (uint8_t)((m_bitlen >> 40) & 0xFF);
    m_data[57] = (uint8_t)((m_bitlen >> 48) & 0xFF);
    m_data[56] = (uint8_t)((m_bitlen >> 56) & 0xFF);
    transform();
}

// Write the eight 32-bit state words out to the 32-byte hash buffer in big-endian
// order (SHA's canonical output byte ordering).
void SHA256::revert(std::array<uint8_t, 32> & hash) {
    // SHA uses big endian byte ordering
    // Revert all bytes
    for (uint8_t i = 0 ; i < 4 ; i++) {
        for(uint8_t j = 0 ; j < 8 ; j++) {
            hash[i + (j * 4)] = (uint8_t)((m_state[j] >> (24 - i * 8)) & 0x000000ff);
        }
    }
}

// Convert a 32-byte digest into a 64-character lowercase hexadecimal string.
std::string SHA256::toString(const std::array<uint8_t, 32> & digest) {
    std::stringstream s;
    s << std::setfill('0') << std::hex;

    for(uint8_t i = 0 ; i < 32 ; i++) {
        s << std::setw(2) << (unsigned int) digest[i];
    }

    return s.str();
}