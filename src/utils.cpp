//
// Created by mctrivia on 14/01/24.
//
// utils.cpp - implementations of the small, general-purpose helpers in the
// utils:: namespace used across the node and pool server (string/JSON helpers,
// file utilities, random code generation, big-number modulo, console progress
// bar, and interactive console prompts). See utils.h for the declarations.
//

#include "utils.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <jsoncpp/json/value.h>
#include <random>
#include <regex>
#include <sstream>
#include <sys/stat.h>


using namespace std;

namespace utils {

    /**
     * Function that splits a string by some kind of delimiter and returns it as a vector of strings
     * @param s - input string
     * @param delimiter - delimiter
     * @return
     */
    std::vector<std::string> split(const std::string& s, char delimiter) {
        std::vector<std::string> tokens;
        std::string token;
        std::istringstream tokenStream(s);
        while (std::getline(tokenStream, token, delimiter)) {
            tokens.push_back(token);
        }
        return tokens;
    }

    /**
     * Generates a random string of a specific length
     * @param length
     * @param type
     * @return
     */
    std::string generateRandom(unsigned char length, CodeType type) {
        const std::string numerics = "0123456789";
        const std::string uppercases = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        const std::string alphanumerics = numerics + uppercases;

        std::string characters;
        switch (type) {
            case CodeType::NUMERIC:
                characters = numerics;
                break;
            case CodeType::UPPERCASE:
                characters = uppercases;
                break;
            case CodeType::ALPHANUMERIC:
                characters = alphanumerics;
                break;
        }

        std::random_device rd;
        std::mt19937 generator(rd());
        std::uniform_int_distribution<> distribution(0, characters.size() - 1);

        std::string randomString;
        for (unsigned char i = 0; i < length; ++i) {
            randomString += characters[distribution(generator)];
        }

        return randomString;
    }


    /**
     * Returns whether a file exists at the given path.
     * @param fileName - path to test
     * @return true if stat() succeeds for the path
     */
    bool fileExists(const std::string& fileName) {
        //see if this is first run
        struct stat buffer {};
        return (stat(fileName.c_str(), &buffer) == 0);
    }

    /**
     * Returns if a string contains an integer
     * @param s
     * @return
     */
    bool isInteger(const std::string& s) {
        std::istringstream iss(s);
        int n;
        iss >> n;
        return iss.eof() && !iss.fail(); // Check if reading was successful and the entire string was consumed
    }

    /**
     * Function to help debug Json values
     * @param params
     */
    void printJson(const Json::Value& params) {
        std::cout << params.toStyledString() << std::endl;
    }


    /**
     * Copies a file byte-for-byte from sourcePath to destinationPath (binary mode),
     * overwriting the destination if it exists. Prints an error to stderr and
     * returns false if either file fails to open or the copy fails.
     * @param sourcePath
     * @param destinationPath
     * @return true on success
     */
    bool copyFile(const std::string& sourcePath, const std::string& destinationPath) {
        std::ifstream source(sourcePath, std::ios::binary);
        std::ofstream destination(destinationPath, std::ios::binary);

        // Check if the source file and destination file are opened successfully
        if (!source.is_open() || !destination.is_open()) {
            std::cerr << "Error opening files!" << std::endl;
            return false;
        }

        // Copy the file
        destination << source.rdbuf();

        // Check if the copying was successful
        if (!source || !destination) {
            std::cerr << "Error copying the file!" << std::endl;
            return false;
        }

        return true;
    }

    /**
     * Erases last line and prints a progress bar
     * @param fraction
     * @param progressWidth
     */
    void printProgressBar(float fraction, int progressWidth) {
        int left=std::max(static_cast<int>(fraction*progressWidth),1);
        int right=progressWidth-left;
        std::cout << "\r[" << std::setfill('#') << std::setw(left) << '#';
        std::cout << std::setfill(' ') << std::setw(right) << "]";
        std::cout << std::fixed << std::setprecision(1) << std::setw(5) << (fraction*100) << "%";
        std::cout.flush();
    }

    /**
     * Estimates how much ram a Json::Value uses
     * @param value
     * @return
     */
    size_t estimateJsonMemoryUsage(const Json::Value& value) {
        size_t size = sizeof(Json::Value);

        if (value.isString()) {
            // Add the size of the string
            size += value.asString().size();
        } else if (value.isArray() || value.isObject()) {
            // For arrays and objects, add the size of each element/member
            for (Json::ValueConstIterator it = value.begin(); it != value.end(); ++it) {
                size += estimateJsonMemoryUsage(*it);
            }
        }
        // Add additional memory used by other types as needed

        return size;
    }

    /**
     * Computes numerator % divisor where numerator is a 256-bit unsigned integer
     * stored big-endian in 32 bytes. Works byte by byte to avoid needing 256-bit
     * arithmetic. Caller must ensure divisor is non-zero.
     * @param numerator - 32-byte big-endian value
     * @param divisor
     * @return the 64-bit remainder
     */
    uint64_t mod256by64(const array<uint8_t, 32>& numerator, uint64_t divisor) {
        uint64_t remainder = 0;

        // Process each byte of the 256-bit number
        for (int i = 0; i < 32; ++i) {
            remainder = (remainder << 8) | numerator[i];
            remainder %= divisor;
        }

        return remainder;
    }





    /**
     * Blocks reading from stdin until the user enters Y or N (case-insensitive),
     * re-prompting on any other input.
     * @return true for Y, false for N
     */
    bool getAnswerBool() {
        char input;
        while (true) {
            cin >> input;
            input = (char)toupper((unsigned char)input); // Convert to uppercase to handle lowercase inputs

            if (input == 'Y') {
                return true;
            } else if (input == 'N') {
                return false;
            } else {
                cout << "Invalid input. Please enter Y or N: ";
            }
        }
    }

    /**
     * Blocks reading a full line from stdin and parsing it as an integer within
     * [min, max], re-prompting until a valid in-range number is entered. Ignores a
     * leading pending newline/EOF so it composes with prior formatted extractions.
     * @param min - lowest accepted value
     * @param max - highest accepted value
     * @return the entered integer
     */
    int getAnswerInt(int min, int max) {
        string inputLine;
        int input;

        while (true) {
            // Check if there's a pending newline or other character in the input buffer
            if (cin.peek() == '\n' || cin.peek() == EOF) {
                cin.ignore();  // Ignore the leftover newline or EOF before reading the line
            }

            getline(cin, inputLine);  // Use getline to read the full line

            stringstream ss(inputLine);
            if (ss >> input && ss.eof()) {  // Check if the entire stringstream converts to an integer and if there's nothing else
                if (input < min || input > max) {
                    cout << "Invalid input. Please enter a number between " << min << " and " << max << ": ";
                } else {
                    return input;
                }
            } else {
                cout << "Invalid input. Please enter a valid number: ";
            }
        }
    }

    /**
     * Blocks reading a full line from stdin, re-prompting until it matches the
     * given regex (or accepting any line when regexPattern is empty). Ignores a
     * leading pending newline/EOF so it composes with prior formatted extractions.
     * @param regexPattern - ECMAScript regex the whole line must match, or "" for any input
     * @return the entered line
     */
    string getAnswerString(const string& regexPattern) {
        string input;
        regex pattern(regexPattern);

        // Check if there's a pending newline or other character in the input buffer
        if (cin.peek() == '\n' || cin.peek() == EOF) {
            cin.ignore();  // Ignore the leftover newline or EOF before reading the line
        }

        while (true) {
            getline(cin, input);  // Use getline to read the full line of input

            if (regexPattern.empty() || regex_match(input, pattern)) {
                return input;  // Return the input if it matches the pattern or no pattern is provided
            } else {
                cout << "Invalid input. Please try again: ";
            }
        }
    }
} // namespace utils