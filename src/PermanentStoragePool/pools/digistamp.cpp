//
// digistamp.cpp - see digistamp.h. Only the identity differs from mctrivia; the
// protocol, server, pricing and threads are all inherited.
//

#include "PermanentStoragePool/pools/digistamp.h"

std::string digistamp::getName() {
    return "DigiStamp Pool";
}

std::string digistamp::getDescription() {
    return "The community DigiStamp Permanent Storage Pool (pool.digistamp.co). It "
           "keeps DigiAsset metadata permanently available and pays DigiAsset nodes "
           "DGB to help host and distribute it. Replaces the legacy MCTrivia's PSP.";
}
