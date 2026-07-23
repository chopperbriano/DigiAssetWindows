//
// custompool.h - a generic, user-configurable Permanent Storage Pool slot.
//
// The "extra" pool toward 1.0.0. Same protocol as the DigiStamp pool, but with
// NO built-in server: it stays completely inert until the operator points
// psp<index>server at a DigiAssetPoolServer and sets psp<index>subscribe=1.
// Registered at index 3 (psp3*). Lets an operator add a second pool (their own,
// or a partner's) without a code change - just config.
//

#ifndef DIGIASSET_CORE_CUSTOMPOOL_H
#define DIGIASSET_CORE_CUSTOMPOOL_H

#include "PermanentStoragePool/pools/mctrivia.h"

class custompool : public mctrivia {
protected:
    // No built-in server - must be supplied via psp<index>server.
    std::string defaultServer() const override { return ""; }
    // Opt-in only.
    bool defaultSubscribe() const override { return false; }

public:
    std::string getName() override;
    std::string getDescription() override;
    // Guarded start: does nothing until a server URL is configured, so an
    // accidentally-subscribed-but-unconfigured slot can't spin threads at "".
    void start() override;
};

#endif //DIGIASSET_CORE_CUSTOMPOOL_H
