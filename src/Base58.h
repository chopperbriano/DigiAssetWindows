//
// Created by mctrivia on 20/06/23.
//
// Base58.h - declares the Base58 utility class.
//
// Base58 is the Bitcoin/DigiByte-style base-58 codec (the alphabet omits the
// visually ambiguous characters 0, O, I and l).  It is used throughout the node
// wherever raw byte strings must be rendered as human-safe text - most notably
// DigiByte addresses and other base58-encoded identifiers handled while the
// chain analyzer parses transactions.  All members are static; the class is a
// namespace-style holder and is never instantiated.

#ifndef DIGIASSET_CORE_BASE58_H
#define DIGIASSET_CORE_BASE58_H

#include <cstdint>
#include <string>
#include <vector>

/**
 * Static helper class providing base-58 encode/decode.
 * Holds the forward alphabet (base58Map) and the reverse lookup table
 * (alphaMap) used to translate between raw bytes and base-58 text.
 */
class Base58 {
    //static const uint8_t map[];


    static const uint8_t base58Map[];
    static const uint8_t alphaMap[];


public:
    // Encode a raw byte vector into a base-58 string.
    static std::string encode(const std::vector<uint8_t>& data);
    // Decode a base-58 string back into its raw byte vector.
    static std::vector<uint8_t> decode(const std::string& data);
};


#endif //DIGIASSET_CORE_BASE58_H
