//
// digistamp.h - the "DigiStamp Pool" Permanent Storage Pool.
//
// First-class replacement for the legacy "MCTrivia's PSP" (mctrivia) class: same
// protocol and same default server (pool.digistamp.co), but its own identity and
// its own psp<index> config keys. Registered at index 2, so it reads psp2*. It is
// OPT-IN (defaultSubscribe() == false): existing nodes whose config predates it
// are unaffected until they set psp2subscribe=1; new setups point here instead of
// the deprecated psp1 (mctrivia) pool.
//

#ifndef DIGIASSET_CORE_DIGISTAMP_H
#define DIGIASSET_CORE_DIGISTAMP_H

#include "PermanentStoragePool/pools/mctrivia.h"

class digistamp : public mctrivia {
protected:
    // Opt-in only: a node whose config has no psp2subscribe does NOT auto-join,
    // so upgrading an existing node never changes its pool membership or tries to
    // resolve a psp2 payout address.
    bool defaultSubscribe() const override { return false; }

public:
    std::string getName() override;
    std::string getDescription() override;
    // getURL(), getCost(), defaultServer() are inherited from mctrivia - the
    // DigiStamp pool IS the server mctrivia already defaulted to.
};

#endif //DIGIASSET_CORE_DIGISTAMP_H
