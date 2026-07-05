//
// Created by mctrivia on 05/09/23.
//
// Config — a tiny `key=value` config-file reader/writer used across both the
// node and the pool server. Backs config.cfg, from which nearly every runtime
// setting is read (RPC credentials, ports, IPFS path, prune options, PSP pool
// URL and payout address, etc.). Typed accessors (getString/getInteger/getBool
// and their *Map variants) parse values on demand; setters plus write() persist
// changes while preserving the file's comments and original line ordering.
//

#ifndef DIGIASSET_CORE_CONFIG_H
#define DIGIASSET_CORE_CONFIG_H

#include <map>
#include <string>
#include <vector>

using namespace std;

/**
 * In-memory view of a `key=value` config file. Construct with a filename to load
 * immediately (throws exceptionConfigFileMissing if absent), query with the
 * typed getters, mutate with the setters, and persist with write(). Missing or
 * wrong-typed values throw the nested exception types unless a default is passed.
 */
class Config {
    map<string, string> _values;
    // _rawLines stores the file's contents in original order, INCLUDING comment
    // lines (`#...`) and blank lines. _keyToLineIndex maps each known key to
    // the index in _rawLines where it lives. On setString we update that line
    // in place instead of rewriting the whole file from _values, so comments
    // and user-intended ordering survive a write().
    vector<string> _rawLines;
    map<string, size_t> _keyToLineIndex;
    string _fileName;
    static bool isInteger(const string& value);
    static bool isBool(const string& value);

public:
    static const unsigned char UNKNOWN = 0;
    static const unsigned char STRING = 1;
    static const unsigned char INTEGER = 10;


    explicit Config(const string& fileName);
    explicit Config();


    string getString(const string& key) const;
    string getString(const string& key, const string& defaultValue) const;
    int getInteger(const string& key) const;
    int getInteger(const string& key, int defaultValue) const;
    bool getBool(const string& key) const;
    bool getBool(const string& key, bool defaultValue) const;
    bool isKey(const string& key, unsigned char type = UNKNOWN) const;

    //*Map getters return every key that starts with keyPrefix, with the prefix
    //stripped from the returned key. Values that don't parse to the requested
    //type are silently skipped (integer/bool variants).
    map<string, string> getStringMap(const string& keyPrefix) const;
    map<string, int> getIntegerMap(const string& keyPrefix) const;
    map<string, bool> getBoolMap(const string& keyPrefix) const;

    void setString(const string& key, const string& value);
    void setInteger(const string& key, int value);
    void setBool(const string& key, bool value);
    //*Map setters write one `key+subkey=value` entry per map element.
    void setStringMap(const string& key, const map<string, string>& values);
    void setIntegerMap(const string& key, const map<string, int>& values);
    void setBoolMap(const string& key, const map<string, bool>& values);

    void clear();                          //drop all loaded values and raw lines
    void refresh();                        //reload from _fileName (throws if missing)
    void write(string fileName = "") const;//persist to file, preserving comments/order

    /*
    ███████╗██████╗ ██████╗  ██████╗ ██████╗ ███████╗
    ██╔════╝██╔══██╗██╔══██╗██╔═══██╗██╔══██╗██╔════╝
    █████╗  ██████╔╝██████╔╝██║   ██║██████╔╝███████╗
    ██╔══╝  ██╔══██╗██╔══██╗██║   ██║██╔══██╗╚════██║
    ███████╗██║  ██║██║  ██║╚██████╔╝██║  ██║███████║
    ╚══════╝╚═╝  ╚═╝╚═╝  ╚═╝ ╚═════╝ ╚═╝  ╚═╝╚══════╝
     */

    class exception : public std::exception {
    protected:
        std::string _lastErrorMessage;
        mutable std::string _fullErrorMessage;

    public:
        explicit exception(const std::string& message = "Unknown") : _lastErrorMessage(message) {}

        virtual const char* what() const noexcept override {
            _fullErrorMessage = "Config Exception: " + _lastErrorMessage;
            return _fullErrorMessage.c_str();
        }
    };

    class exceptionConfigFileMissing : public exception {
    public:
        explicit exceptionConfigFileMissing()
            : exception("File missing") {}
    };

    /**
     * A value is missing or of wrong type
     */
    class exceptionCorruptConfigFile : public exception {
    public:
        explicit exceptionCorruptConfigFile(const std::string& error = "File corrupt")
            : exception(error) {}
    };

    /**
     * A value is missing
     */
    class exceptionCorruptConfigFile_Missing : public exceptionCorruptConfigFile {
    public:
        explicit exceptionCorruptConfigFile_Missing(const std::string& error = "Missing value")
            : exceptionCorruptConfigFile(error) {}
    };

    /**
     * A value is wrong type
     */
    class exceptionCorruptConfigFile_WrongType : public exceptionCorruptConfigFile {
    public:
        explicit exceptionCorruptConfigFile_WrongType(const std::string& error = "Value wrong type")
            : exceptionCorruptConfigFile(error) {}
    };

    class exceptionConfigFileCouldNotBeWritten : public exception {
    public:
        explicit exceptionConfigFileCouldNotBeWritten()
            : exception("Couldn't Write") {}
    };

    /**
     * Not used by class direct but can be used by code using class as common way of showing that a value in config is not correct
     */
    class exceptionConfigFileInvalid : public exception {
    public:
        explicit exceptionConfigFileInvalid(const std::string& error = "Values in the config file are not correct")
            : exception(error) {}
    };
};



#endif //DIGIASSET_CORE_CONFIG_H
