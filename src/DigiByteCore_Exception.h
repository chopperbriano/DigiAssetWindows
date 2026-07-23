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
            /* Wrapped DigiByte Core error — message is a bitcoin-style JSON blob */
        } else if (containsErrorBlob(message)) {
            this->code = parseCode(message);
            this->msg = parseMessage(message);
            /* Directly thrown error — keep the code and message as given */
        } else {
            this->code = errcode;
            this->msg = message;
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

    /* True if the message carries a bitcoin-style {"error":{...}} JSON blob
       (the format jsonrpccpp produces when DigiByte Core rejects a call) */
    bool containsErrorBlob(const std::string& in) {
        Value root;
        Reader reader;
        std::string strJson = removePrefix(in, "INTERNAL_ERROR: : ");
        return reader.parse(strJson.c_str(), root) && root.isObject() && root.isMember("error");
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

        /* Default: pass the message through unchanged. RPC method handlers throw
         * DigiByteException with a PLAIN human message (e.g. "Domain Burned"),
         * which is not a JSON error payload. Wrapping such a message in
         * "Error during parsing of >>...<<" — and again when the CLI re-parses
         * the response — produced the confusing doubly-nested message. Only emit
         * that diagnostic when the input actually looks like a JSON payload that
         * failed to parse. */
        std::string ret = strJson;

        /* Parse error message */
        bool parsingSuccessful = reader.parse(strJson.c_str(), root);
        if (parsingSuccessful && root.isMember("error")) {
            ret = removePrefix(root["error"]["message"].asString(), "Error: ");
            if (!ret.empty()) ret[0] = toupper(ret[0]);
        } else if (!strJson.empty() && strJson[0] == '{') {
            ret = "Error during parsing of >>" + strJson + "<<";
        }

        return ret;
    }
};


#endif //DIGIASSET_CORE_DIGIBYTECORE_EXCEPTION_H
