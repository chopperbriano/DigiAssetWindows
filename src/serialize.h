//
// Created by mctrivia on 06/07/23.
//
// serialize.h - header-only binary (de)serialization helpers used across the
// node and pool server to pack simple values into, and unpack them from, a
// flat vector<uint8_t> byte buffer. Provides matching serialize()/deserialize()
// overloads for uint8_t, uint64_t (big-endian, 8 bytes), std::string, and any
// vector<T> for which a serialize() overload of T exists. Strings and vectors
// are length-prefixed with a uint64_t count. deserialize() advances a caller-
// supplied cursor index i and throws std::out_of_range when a read would run
// past the end of the buffer. These primitives are the building blocks used by
// serializable objects (e.g. blocks/transactions) elsewhere in the codebase.
//

#ifndef DIGIASSET_CORE_SERIALIZE_H
#define DIGIASSET_CORE_SERIALIZE_H

#include <vector>
#include <cstdint>
#include <cstddef>
#include <string>
#include <stdexcept>

using namespace std;




/**
 * Serialize function.  Takes input serializes it and adds it to the serializedData
 * Appends a single byte to the buffer.
 */
static void serialize(vector<uint8_t>& serializedData, const uint8_t& input) {
    serializedData.push_back(input);
}

/**
 * Reads one byte from serializedData at cursor i into output and advances i.
 * Throws out_of_range if the cursor is already at/past the end of the buffer.
 */
static void deserialize(const vector<uint8_t>& serializedData, size_t& i, uint8_t& output) {
    if (i >= serializedData.size()) throw out_of_range("read past end of data");
    output = serializedData[i];
    i++;
}

/**
 * Appends a 64-bit value as 8 bytes in big-endian (most significant byte first) order.
 */
static void serialize(vector<uint8_t>& serializedData, const uint64_t& input) {
    for (size_t shift = 56; shift > 0; shift -= 8) {
        serializedData.push_back((input >> shift) & 0xff);
    }
    serializedData.push_back(input & 0xff);
}

/**
 * Reads 8 big-endian bytes from cursor i into output and advances i by 8.
 * Throws out_of_range if fewer than 8 bytes remain.
 */
static void deserialize(const vector<uint8_t>& serializedData, size_t& i, uint64_t& output) {
    if (i + 8 > serializedData.size()) throw out_of_range("read past end of data");
    output = 0;
    for (size_t shift = 56; shift > 0; shift -= 8) {
        output += ((uint64_t) serializedData[i] << shift);
        i++;
    }
    output += serializedData[i];
    i++;
}

/**
 * Serializes a string as a uint64_t length prefix followed by its raw bytes.
 */
static void serialize(vector<uint8_t>& serializedData, const string& input) {
    //store number of elements
    serialize(serializedData, (uint64_t) input.size());

    //store elements
    for (const char& letter: input) {
        serialize(serializedData, (uint8_t) letter);
    }
}

/**
 * Reads a length-prefixed string: an 8-byte count then that many bytes, into
 * output, advancing i. Throws out_of_range if the declared length overruns the buffer.
 */
static void deserialize(const vector<uint8_t>& serializedData, size_t& i, string& output) {
    //get length
    uint64_t size;
    deserialize(serializedData, i, size);

    //error check
    if (i + size > serializedData.size()) throw out_of_range("read past end of data");

    //decode element
    output.clear();
    output.resize(size);
    for (size_t ii = 0; ii < size; ii++) {
        output[ii] = serializedData[i];
        i++;
    }
}

/**
 * Generic function to serialize vector of any type
 * Still need a function to serialize the type inside the vector for this to work
 */
template<typename T>
static void serialize(vector<uint8_t>& serializedData, const vector<T>& input) {
    //store number of elements
    serialize(serializedData, (uint64_t) input.size());

    //store elements
    for (const T& element: input) {
        serialize(serializedData, element);
    }
}


/**
 * Generic function to deserialize a vector of any type. Reads the uint64_t
 * element count, resizes output, then deserializes each element in turn using
 * the matching deserialize() overload for T. Requires such an overload to exist.
 */
template<typename T>
static void deserialize(const vector<uint8_t>& serializedData, size_t& i, vector<T>& output) {
    //get length
    uint64_t size;
    deserialize(serializedData, i, size);

    //decode element
    output.clear();
    output.resize(size);
    for (T& value: output) {
        deserialize(serializedData, i, value);
    }
}


#endif //DIGIASSET_CORE_SERIALIZE_H