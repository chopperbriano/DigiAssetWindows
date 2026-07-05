//
// cli/main.cpp - DigiAssetWindows-cli.exe entry point.
//
// A thin command-line RPC client for the running DigiAssetWindows node. It
// takes a command name plus arguments on argv, converts each argument into
// the right JSON type (array/object, integer, double, bool, or string),
// forwards the call to the node over JSON-RPC (via DigiByteCore reading the
// local config.cfg), and prints the result. Handles the common failure cases
// with friendly messages: node/RPC service down, and commands forbidden by
// config. This is the operator's "poke the node" tool, separate from the
// node exe itself.
//

#include "Config.h"
#include "Database.h"
#include "DigiByteCore.h"
#include <iostream>
#include <jsonrpccpp/client.h>
#include <regex>


// Parse argv into a JSON argument array, send the requested command to the
// local node over RPC, and print the response. argv[1] is the command name;
// argv[2..] are its arguments, each coerced to the most specific JSON type it
// matches (bracketed text is parsed as JSON, then integer, double, bool, and
// finally raw string). Returns 1 if no command was given, otherwise 0 (errors
// are reported on stdout, not via the exit code).
int main(int argc, char* argv[]) {
    if (argc < 2) return 1;

    //get command
    string command = argv[1];

    // Prepare to parse arguments into JSON
    Json::Value args = Json::arrayValue;
    Json::Reader reader;

    for (int i = 2; i < argc; ++i) {
        std::string argStr(argv[i]);
        Json::Value parsedValue;

        // Check if the argument could be a JSON array or object
        if ((argStr.front() == '[' && argStr.back() == ']') || (argStr.front() == '{' && argStr.back() == '}')) {
            bool parsingSuccessful = reader.parse(argStr, parsedValue);
            if (parsingSuccessful) {
                // If it's a JSON array or object, append it directly
                args.append(parsedValue);
            } else {
                // Parsing failed, treat as a string
                args.append(argStr);
            }
        } else {
            // For non-JSON strings, attempt to parse as number or boolean
            char* endptr;
            long num = strtol(argv[i], &endptr, 10);
            if (*endptr == '\0') {                  //Is integer
                args.append(static_cast<Json::Int64>(num));
            } else {
                // Not an integer, try double
                double fnum = strtod(argv[i], &endptr);
                if (*endptr == '\0') {              //Is a double
                    args.append(fnum);
                } else if (argStr == "true") {      //Is bool
                    args.append(true);
                } else if (argStr == "false") {     //Is bool
                    args.append(false);
                } else {                            //Is String
                    args.append(argStr);
                }
            }
        }
    }

    //ask core what it means
    DigiByteCore dgb;
    dgb.setFileName("config.cfg", true);
    dgb.makeConnection();
    try {
        cout << dgb.sendcommand(command, args) << "\n";
    } catch (const DigiByteException& e) {
        string errorMessage=e.getMessage();

        //check if DigiAsset for Windows is offline
        if (errorMessage.substr(0,20)=="Could not connect to") {
            cout << "Exception: It looks like DigiAsset for Windows RPC Service is down.";
            return 0;
        }

        //check if command is forbiden
        regex pattern(">>.* is forbidden<<");
        if (regex_search(errorMessage, pattern)) {
            cout << "Exception: " + command + " is forbidden by config settings.";
            return 0;
        }




        //show generic error
        cout << "error code: " << e.getCode() << "\n";
        cout << "error message:\n"
             << errorMessage << "\n";
    } catch (...) {
        cout << "Exception: unexpected error.";
    }
}