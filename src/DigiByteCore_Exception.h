/**
 * @file    exception.h
 * @author  Krzysztof Okupski
 * @date    29.10.2014
 * @version 1.0
 *
 * Declaration of error class for the JSON-RPC wrapper.
 *
 * DigiByteCore_Exception.h - Exception type thrown by the low-level JSON-RPC
 * layer that talks to DigiByte Core. When a jsonrpccpp call fails, its raw
 * error code and message are wrapped in a DigiByteException, which classifies
 * the failure (connection dropped, authentication failure, or a Core-reported
 * error) and, for the last case, digs the real code and message out of the
 * embedded JSON error payload. DigiByteCore::sendcommand constructs these; the
 * higher-level DigiByteCore wrapper catches them and translates into its own
 * friendlier exception types.
 */

#ifndef DIGIASSET_CORE_DIGIBYTECORE_EXCEPTION_H
#define DIGIASSET_CORE_DIGIBYTECORE_EXCEPTION_H


#include <string>
#include <sstream>
#include <iostream>

#include <jsoncpp/json/json.h>
#include <jsoncpp/json/reader.h>
#include <jsoncpp/json/value.h>
#include <jsonrpccpp/client.h>

using Json::Value;
using Json::Reader;
using jsonrpc::Errors;


/**
 * Wraps a JSON-RPC failure from DigiByte Core into a queryable exception,
 * exposing a numeric code and a cleaned-up message via getCode()/getMessage().
 */
class DigiByteException : public std::exception {
private:
    int code;
    std::string msg;

public:
    /**
     * Classifies the raw jsonrpccpp error into one of three cases:
     *  - connection error: keeps the code, strips the " -> " prefix off the msg
     *  - authentication failure (internal error with an 18-char message):
     *    reports a fixed "Failed to authenticate successfully" message
     *  - any other error: parses code and message out of the JSON error payload
     *    embedded in the message string (see parseCode/parseMessage)
     * @param errcode  raw error code from the JSON-RPC client
     * @param message  raw error message (may contain an embedded JSON payload)
     */
    explicit DigiByteException(int errcode, const std::string& message) noexcept {

        /* Connection error */
        if (errcode == Errors::ERROR_CLIENT_CONNECTOR) {
            this->code = errcode;
            this->msg = removePrefix(message, " -> ");
            /* Authentication error */
        } else if (errcode == Errors::ERROR_RPC_INTERNAL_ERROR && message.size() == 18) {
            this->code = errcode;
            this->msg = "Failed to authenticate successfully";
            /* Miscellaneous error */
        } else {
            this->code = parseCode(message);
            this->msg = parseMessage(message);
        }
    }

    ~DigiByteException() throw() {};

    int getCode() const {
        return code;
    }

    std::string getMessage() const {
        return msg;
    }


    /**
     * Returns a copy of `in` with everything up to and including the first
     * occurrence of `pattern` removed. If `pattern` is not found, returns `in`
     * unchanged.
     */
    std::string removePrefix(const std::string& in, const std::string& pattern) {
        std::string ret = in;

        unsigned int pos = ret.find(pattern);

        if (pos <= ret.size()) {
            ret.erase(0, pos + pattern.size());
        }

        return ret;
    }

    /* Auxiliary JSON parsing */
    /**
     * Strips the "INTERNAL_ERROR: : " prefix off `in`, parses the remainder as
     * JSON, and returns error.code from it. Returns -1 if parsing fails.
     */
    int parseCode(const std::string& in) {
        Value root;
        Reader reader;

        /* Remove JSON prefix */
        std::string strJson = removePrefix(in, "INTERNAL_ERROR: : ");
        int ret = -1;

        /* Parse error message */
        bool parsingSuccessful = reader.parse(strJson.c_str(), root);
        if (parsingSuccessful) {
            ret = root["error"]["code"].asInt();
        }

        return ret;
    }

    /**
     * Strips the "INTERNAL_ERROR: : " prefix off `in`, parses the remainder as
     * JSON, and returns error.message with any leading "Error: " removed and the
     * first character upper-cased. If parsing fails, returns a diagnostic string
     * echoing the unparsed payload.
     */
    std::string parseMessage(const std::string& in) {
        Value root;
        Reader reader;

        /* Remove JSON prefix */
        std::string strJson = removePrefix(in, "INTERNAL_ERROR: : ");
        std::string ret = "Error during parsing of >>" + strJson + "<<";

        /* Parse error message */
        bool parsingSuccessful = reader.parse(strJson.c_str(), root);
        if (parsingSuccessful) {
            ret = removePrefix(root["error"]["message"].asString(), "Error: ");
            ret[0] = toupper(ret[0]);
        }

        return ret;
    }
};


#endif //DIGIASSET_CORE_DIGIBYTECORE_EXCEPTION_H
