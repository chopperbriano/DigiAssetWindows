//
// Created by mctrivia on 07/04/23.
//
// Blob.h - declares the Blob class, a small owning wrapper around a
// heap-allocated block of raw bytes.
//
// Blob is a general-purpose binary buffer used across the node/pool for things
// like hashes, IPFS content and other opaque byte strings.  It owns its buffer
// (allocates on construction, frees on destruction) and provides hex <-> bytes
// conversion plus value equality, so byte strings can be compared and moved
// around by value.

#ifndef DIGIBYTECORE_BLOB_H
#define DIGIBYTECORE_BLOB_H


#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

/**
 * Owning fixed-length binary buffer.
 * Manages a malloc'd byte array (_data / _length) and supports construction
 * from a raw pointer, a byte vector, or a hex string; conversion back to hex or
 * a vector; and deep-copy semantics via the copy constructor and assignment
 * operator.  Equality compares the byte contents.
 */
class Blob {
public:
    // Copy length bytes from a raw pointer into a newly owned buffer.
    Blob(const void* data, int length);
    // Copy the contents of a byte vector into a newly owned buffer.
    explicit Blob(const std::vector<uint8_t>& data);
    // Parse a hex string (must have an even length) into raw bytes.
    explicit Blob(const std::string& hex);
    ~Blob();
    // Render the stored bytes as a lowercase hex string.
    std::string toHex() const;
    unsigned char* data() const;
    // Return a copy of the stored bytes as a vector.
    std::vector<uint8_t> vector() const;
    size_t length() const;

    // Deep-copy construction/assignment (each Blob owns its own buffer).
    Blob(const Blob& other);
    Blob& operator=(const Blob& other);


    // Byte-wise equality: same length and identical contents.
    bool operator==(const Blob& b) const {
        if (b._length != _length) return false;
        if (_data != b._data) {
            return memcmp(_data, b._data, _length) == 0;
        }
        return true;
    }

    bool operator!=(const Blob& b) const {
        return !(*this == b);
    }

private:
    unsigned char* _data;
    size_t _length;
};


#endif //DIGIBYTECORE_BLOB_H
