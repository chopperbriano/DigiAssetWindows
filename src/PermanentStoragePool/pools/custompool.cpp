//
// custompool.cpp - see custompool.h.
//

#include "PermanentStoragePool/pools/custompool.h"
#include "Log.h"

std::string custompool::getName() {
    return getURL().empty() ? "Custom Pool (unconfigured)" : "Custom Pool";
}

std::string custompool::getDescription() {
    return "A user-configurable Permanent Storage Pool slot. Point "
           + configPrefix() + "server at any DigiAssetPoolServer and set "
           + configPrefix() + "subscribe=1 to activate it.";
}

void custompool::start() {
    // getURL() is _baseUrl (from psp<index>server) or defaultServer() (""). If it
    // resolves to empty, the slot isn't configured - stay inert rather than start
    // threads that would hammer an empty URL.
    if (getURL().empty()) {
        Log::GetInstance()->addMessage(
                "Custom PSP slot (" + configPrefix() + ") is subscribed but has no server "
                "configured - staying inert. Set " + configPrefix() + "server to activate.",
                Log::INFO);
        return;
    }
    mctrivia::start();
}
