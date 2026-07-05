//
// Created by mctrivia on 14/01/24.
//
// utils.h - declarations for the utils:: namespace of small, general-purpose
// helper functions shared throughout the node and pool server: string
// splitting, random code generation, file existence/copy checks, integer
// parsing, JSON debug printing and memory estimation, big-number modulo, a
// console progress bar, and interactive console prompt helpers. Implementations
// live in utils.cpp.
//

#ifndef DIGIASSET_CORE_UTILS_H
#define DIGIASSET_CORE_UTILS_H

#include <jsoncpp/json/value.h>
#include <array>
#include <limits>
#include <string>
#include <vector>

namespace utils {
    // Character set to draw from when generating a random code (see generateRandom).
    enum class CodeType {
        NUMERIC,
        UPPERCASE,
        ALPHANUMERIC
    };


    // Redraw the current console line as a [###   ] xx.x% progress bar (fraction 0..1).
    void printProgressBar(float fraction, int progressWidth=60);
    // Split s into tokens on each occurrence of delimiter.
    std::vector<std::string> split(const std::string& s, char delimiter);
    // Build a random string of the given length from the character set chosen by type.
    std::string generateRandom(unsigned char length, CodeType type);
    // True if a file at fileName exists (via stat).
    bool fileExists(const std::string& fileName);
    // True if s parses cleanly and entirely as an int.
    bool isInteger(const std::string& s);
    void printJson(const Json::Value& params);  //added to make debugging easier
    // Binary-copy sourcePath to destinationPath; returns false on any open/copy error.
    bool copyFile(const std::string& sourcePath, const std::string& destinationPath);
    // Rough estimate of the heap memory a Json::Value occupies (recurses into arrays/objects).
    size_t estimateJsonMemoryUsage(const Json::Value& value);
    // Remainder of a 256-bit big-endian number (32 bytes) divided by a 64-bit divisor.
    uint64_t mod256by64(const std::array<uint8_t, 32>& numerator, uint64_t divisor);

    // Blocking console prompt: read Y/N and return the corresponding bool.
    bool getAnswerBool();
    // Blocking console prompt: read and return an int within [min, max], re-prompting on bad input.
    int getAnswerInt(int min=std::numeric_limits<int>::min(), int max=std::numeric_limits<int>::max());
    // Blocking console prompt: read and return a line, optionally requiring it to match regexPattern.
    std::string getAnswerString(const std::string& regexPattern = "");
} // namespace utils

#endif //DIGIASSET_CORE_UTILS_H
