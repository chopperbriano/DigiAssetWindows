//
// Created by mctrivia on 25/01/24.
//
// AppMain - process-wide service locator for the node exe
// (DigiAssetWindows.exe). A single instance holds pointers to the app's
// long-lived subsystems (Database, IPFS, DigiByteCore, the permanent-storage
// pool list, ChainAnalyzer, and the RPC cache/server + web server) so any
// code can reach them without threading them through call chains. It only
// stores/returns these pointers; it does not own or construct the objects
// (main() creates them and calls the setters, then reset() on shutdown).
//

#ifndef DIGIASSET_CORE_APPMAIN_H
#define DIGIASSET_CORE_APPMAIN_H



#include "ChainAnalyzer.h"
#include "Database.h"
#include "PermanentStoragePool/PermanentStoragePoolList.h"
#include "RPC/Cache.h"
#include "RPC/Server.h"
#include "WebServer.h"
#include <mutex>
/**
 * Thread-safe singleton service locator (see file header). Copy/assignment
 * are deleted; obtain the sole instance via GetInstance(). The set*() methods
 * register each subsystem pointer; the plain get*() methods log CRITICAL and
 * throw runtime_error if the subsystem was never set, while the get*IfSet()
 * variants return nullptr instead of throwing.
 */
class AppMain {
    /**
 * Singleton Start
 */
private:
    static AppMain* _pinstance;
    static std::mutex _mutex;

protected:
    AppMain() = default;
    ~AppMain() = default;

public:
    AppMain(AppMain& other) = delete;
    void operator=(const AppMain&) = delete;
    static AppMain* GetInstance();

    /**
 * Singleton End
 */


private:
    Database* _db = nullptr;
    IPFS* _ipfs = nullptr;
    DigiByteCore* _dgb = nullptr;
    PermanentStoragePoolList* _psp = nullptr;
    ChainAnalyzer* _analyzer = nullptr;
    RPC::Cache* _rpcCache = nullptr;
    RPC::Server* _rpcServer = nullptr;
    WebServer* _webServer = nullptr;

public:
    void setDatabase(Database* db);
    Database* getDatabase();

    void setIPFS(IPFS* ipfs);
    IPFS* getIPFS();

    void setDigiByteCore(DigiByteCore* dgb);
    DigiByteCore* getDigiByteCore();
    bool isDigiByteCoreSet();

    void setPermanentStoragePoolList(PermanentStoragePoolList* psp);
    PermanentStoragePoolList* getPermanentStoragePoolList();

    void setChainAnalyzer(ChainAnalyzer* analyzer);
    ChainAnalyzer* getChainAnalyzer();

    void setRpcCache(RPC::Cache* cache);
    RPC::Cache* getRpcCache();

    void setRpcServer(RPC::Server* server);
    RPC::Server* getRpcServer();

    void setWebServer(WebServer* ws);
    WebServer* getWebServer();

    // Null-safe getters (return nullptr instead of throwing)
    Database* getDatabaseIfSet() { return _db; }
    IPFS* getIPFSIfSet() { return _ipfs; }
    DigiByteCore* getDigiByteCoreIfSet() { return _dgb; }
    ChainAnalyzer* getChainAnalyzerIfSet() { return _analyzer; }
    RPC::Server* getRpcServerIfSet() { return _rpcServer; }
    WebServer* getWebServerIfSet() { return _webServer; }
    PermanentStoragePoolList* getPermanentStoragePoolListIfSet() { return _psp; }

    void reset();
};



#endif //DIGIASSET_CORE_APPMAIN_H
