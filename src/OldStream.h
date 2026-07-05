//
// Created by mctrivia on 26/02/24.
//
//
// OldStream - legacy "stream" API compatibility layer for the node.
//
// Recreates the key/value lookup interface exposed by the original
// (pre-Core) DigiAsset stream service so older clients keep working against
// this node.  A single public entry point, getKey(), takes an opaque string
// key (block height, block/transaction hash, DigiAsset id, address, the
// literal "height", or an "<address>_utxos" suffix) and dispatches to the
// appropriate internal fetcher, returning the result wrapped in an
// RPC::Response (with cache lifetime / invalidation hints).  All heavy
// lifting is done in OldStream.cpp; this header only publishes getKey().
//

#ifndef DIGIASSET_CORE_OLDSTREAM_H
#define DIGIASSET_CORE_OLDSTREAM_H


#include "RPC/Response.h"
#include <jsoncpp/json/value.h>
namespace OldStream {
    // Resolve a legacy stream key to its data; see getKey() in OldStream.cpp
    // for the full key-format dispatch rules.
    RPC::Response getKey(const std::string& key);
}

#endif //DIGIASSET_CORE_OLDSTREAM_H
