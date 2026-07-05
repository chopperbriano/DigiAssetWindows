//
// Created by mctrivia on 22/06/23.
//
// DigiAssetTypes.h - Plain-old-data structs shared across the node's asset
// tracking layer, plus free serialize/deserialize helpers for the ones that
// get persisted to the chain-analysis database.
//
// These are the small value types that describe DigiAssets and the metadata
// attached to them: which UTXO holds what, who may re-sign a rewritable asset,
// where exchange-rate feeds live, royalty payouts, and vote options. The
// heavier DigiAsset class (declared elsewhere) references and aggregates these.
// Each serializable struct here has a matching serialize()/deserialize() pair
// (implemented in DigiAssetTypes.cpp) so it can be flattened into a byte buffer
// and read back, using the primitives in serialize.h.
//

#ifndef DIGIASSET_CORE_DIGIASSETTYPES_H
#define DIGIASSET_CORE_DIGIASSETTYPES_H


#include <cstdint>
#include <jsoncpp/json/value.h>
#include <string>

class DigiAsset; //forward declaration

// Minimal identity record for an asset: its string assetId, the numeric
// database index, the IPFS content id (cid) of its metadata, and the block
// height it was first seen at.
struct AssetBasics {
    std::string assetId;
    uint64_t assetIndex;
    std::string cid;
    unsigned int height;
};

// Describes a single issuance (mint) event of an asset: which asset index, the
// txid that issued it, how many units were created, the block height, and the
// cid of the metadata for that issuance.
struct IssuanceBasics {
    uint64_t assetIndex;
    std::string txid;
    uint64_t amount;
    unsigned int height;
    std::string cid;
};

// One address's holding of an asset, count expressed in the asset's smallest
// unit (sats).
struct AssetHolder {
    std::string address;
    uint64_t count; //in sats
};

// A single unspent output as seen by the asset layer: its outpoint
// (txid/vout), the address that controls it, the DigiByte value in sats, and
// the list of DigiAssets carried on that output.
struct AssetUTXO {
    std::string txid;
    uint16_t vout;
    std::string address;
    uint64_t digibyte; //in sats
    std::vector<DigiAsset> assets;

    // Serialize this UTXO (and its assets) to a JSON object for API output.
    // simplified=true asks each contained asset for its compact JSON form.
    Json::Value toJSON(bool simplified = true) const;
};

// One authorized signer of a rewritable asset, with a voting weight used to
// meet the asset's signature threshold. Equality compares address and weight.
struct Signer {
    std::string address;
    uint64_t weight;

    bool operator==(const Signer& b) const {
        if (b.address != address) return false;
        if (b.weight != weight) return false;
        return true;
    }

    bool operator!=(const Signer& b) const {
        return !(
                (b.address == address) &&
                (b.weight == weight));
    }
};

// Append/read a Signer to/from a byte buffer (see DigiAssetTypes.cpp).
void serialize(std::vector<uint8_t>& serializedData, const Signer& input);
void deserialize(const std::vector<uint8_t>& serializedData, size_t& i, Signer& output);


// An exchange-rate feed source: the address publishing the rate, the index of
// the specific rate within that publisher's stream, and a display name (name
// is ignored by equality). Empty address means the feed is disabled.
struct ExchangeRate {
    std::string address;
    uint8_t index;
    std::string name; //not tested in comparison

    bool enabled() const {
        return (!address.empty());
    }

    bool operator==(const ExchangeRate& b) const {
        if (b.address != address) return false;
        if (b.address.empty()) return true; //both disabled
        if (b.index != index) return false;
        return true;
    }

    bool operator!=(const ExchangeRate& b) const {
        if (b.address.empty() && address.empty()) return false; //both disabled
        return !(
                (b.address == address) &&
                (b.index == index));
    }
};

// Append/read an ExchangeRate to/from a byte buffer (see DigiAssetTypes.cpp).
void serialize(std::vector<uint8_t>& serializedData, const ExchangeRate& input);
void deserialize(const std::vector<uint8_t>& serializedData, size_t& i, ExchangeRate& output);


// A royalty rule: pay the given amount (in sats) to the given address on
// transfers of the asset. Equality compares both fields.
struct Royalty {
    std::string address;
    uint64_t amount;

    bool operator==(const Royalty& b) const {
        if (b.address != address) return false;
        if (b.amount != amount) return false;
        return true;
    }

    bool operator!=(const Royalty& b) const {
        return !(
                (b.address == address) &&
                (b.amount == amount));
    }
};

// Append/read a Royalty to/from a byte buffer (see DigiAssetTypes.cpp).
void serialize(std::vector<uint8_t>& serializedData, const Royalty& input);
void deserialize(const std::vector<uint8_t>& serializedData, size_t& i, Royalty& output);


// One selectable option in an asset vote: the address that tallies votes for
// this choice and a human-readable label (label is ignored by equality).
struct VoteOption {
    std::string address;
    std::string label; //not tested in comparison

    bool operator==(const VoteOption& b) const {
        return (b.address == address);
    }

    bool operator!=(const VoteOption& b) const {
        return (b.address != address);
    }
};

// Append/read a VoteOption to/from a byte buffer (see DigiAssetTypes.cpp).
void serialize(std::vector<uint8_t>& serializedData, const VoteOption& input);
void deserialize(const std::vector<uint8_t>& serializedData, size_t& i, VoteOption& output);

#endif //DIGIASSET_CORE_DIGIASSETTYPES_H
