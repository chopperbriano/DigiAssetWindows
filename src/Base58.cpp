//
// Created by mctrivia on 20/06/23.
// reformatted from https://bitcoin.stackexchange.com/questions/76480/encode-decode-base-58-c
//
// Base58.cpp - implementation of the base-58 codec used by the node to convert
// between raw bytes and the human-safe base-58 text form (e.g. DigiByte
// addresses).  Uses big-integer style long division/multiplication by 58, with
// leading-zero bytes preserved as leading '1' characters per the Bitcoin
// convention.

#include <vector>
#include <string>
#include <algorithm>
#include "Base58.h"



// Forward alphabet: index (0-57) -> base-58 character.
const uint8_t Base58::base58Map[] = {
        '1', '2', '3', '4', '5', '6', '7', '8',
        '9', 'A', 'B', 'C', 'D', 'E', 'F', 'G',
        'H', 'J', 'K', 'L', 'M', 'N', 'P', 'Q',
        'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y',
        'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g',
        'h', 'i', 'j', 'k', 'm', 'n', 'o', 'p',
        'q', 'r', 's', 't', 'u', 'v', 'w', 'x',
        'y', 'z'};
// Reverse lookup: ASCII code (low 7 bits) -> base-58 value, 0xff for invalid.
const uint8_t Base58::alphaMap[] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0xff, 0x11, 0x12, 0x13, 0x14, 0x15, 0xff,
        0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0xff, 0x2c, 0x2d, 0x2e,
        0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0xff, 0xff, 0xff, 0xff, 0xff};


/**
 * Encode raw bytes to a base-58 string.
 * Treats data as a big-endian big integer and repeatedly captures its base-58
 * digits, then emits them most-significant first.  Every leading zero byte in
 * the input (except when the input is a single byte) is rendered as a leading
 * '1' so the byte length is recoverable.
 * @param data - raw bytes to encode
 * @return the base-58 encoded string
 */
std::string Base58::encode(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> digits((data.size() * 138 / 100) + 1);
    size_t digitslen = 1;
    for (size_t i = 0; i < data.size(); i++) {
        uint32_t carry = static_cast<uint32_t>(data[i]);
        for (size_t j = 0; j < digitslen; j++) {
            carry = carry + static_cast<uint32_t>(digits[j] << 8);
            digits[j] = static_cast<uint8_t>(carry % 58);
            carry /= 58;
        }
        for (; carry; carry /= 58) {
            digits[digitslen++] = static_cast<uint8_t>(carry % 58);
        }
    }
    std::string result;
    for (size_t i = 0; i < (data.size() - 1) && !data[i]; i++) {
        result.push_back(base58Map[0]);
    }
    for (size_t i = 0; i < digitslen; i++) {
        result.push_back(base58Map[digits[digitslen - 1 - i]]);
    }
    return result;
}

/**
 * Decode a base-58 string back to raw bytes.
 * Inverse of encode(): accumulates the base-58 digits into a big integer, then
 * restores any leading '1' characters as leading zero bytes and reverses the
 * accumulator into big-endian order.  Invalid characters map through alphaMap
 * to 0xff and are treated as digit value 255 (input is assumed valid base-58).
 * @param data - base-58 text to decode
 * @return the decoded raw byte vector (big-endian)
 */
std::vector<uint8_t> Base58::decode(const std::string& data) {
    std::vector<uint8_t> result((data.size() * 138 / 100) + 1);
    size_t resultlen = 1;
    for (size_t i = 0; i < data.size(); i++) {
        uint32_t carry = static_cast<uint32_t>(alphaMap[data[i] & 0x7f]);
        for (size_t j = 0; j < resultlen; j++, carry >>= 8) {
            carry += static_cast<uint32_t>(result[j] * 58);
            result[j] = static_cast<uint8_t>(carry);
        }
        for (; carry; carry >>= 8) {
            result[resultlen++] = static_cast<uint8_t>(carry);
        }
    }
    result.resize(resultlen);
    for (size_t i = 0; i < (data.size() - 1) && data[i] == base58Map[0]; i++) {
        result.push_back(0);
    }
    std::reverse(result.begin(), result.end());
    return result;
}

