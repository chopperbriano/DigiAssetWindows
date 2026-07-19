//
// Created by DigiAsset Core on 14/07/26.
//
// Helper functions for building wallet backed asset transactions.
// Used by the sendasset, issueasset and getwalletbalances RPC methods.
//

#ifndef DIGIASSET_CORE_ASSETWALLET_H
#define DIGIASSET_CORE_ASSETWALLET_H

#include "DigiAssetTypes.h"
#include "DigiByteTransaction.h"
#include <jsoncpp/json/value.h>
#include <string>
#include <vector>

namespace AssetWallet {

    /**
     * Returns all spendable wallet UTXOs with their asset data populated from the local database.
     * @param minconf - minimum number of confirmations(assets should use at least 1 since the
     *                  local database only knows about confirmed asset UTXOs)
     */
    std::vector<AssetUTXO> getWalletUTXOs(int minconf = 1);

    /**
     * Selects wallet UTXOs holding at least `amount` of the given asset.
     * Prefers UTXOs that hold only the requested asset so other assets don't need to be moved.
     * Throws DigiByteTransaction::exceptionNotEnoughFunds if the wallet lacks the requested amount.
     */
    std::vector<AssetUTXO> selectAssetInputs(uint64_t assetIndex, uint64_t amount);

    /**
     * Converts a user supplied amount(display units - so 1.5 of a 2 decimal asset is 150 sats)
     * in to the smallest divisible units.  Accepts integers, doubles and decimal strings.
     */
    uint64_t parseAssetAmount(const Json::Value& amount, uint8_t decimals);

    /**
     * Formats a DigiByte sat count as a decimal DGB string for wallet RPC calls.
     */
    std::string satsToDecimal(uint64_t sats);

    /**
     * Rough miner fee estimate in sats for a not yet funded asset transaction: the node's
     * estimatesmartfee rate(min relay rate fallback) over an approximated funded size
     * (base + op_return + outputs + one funding input + change).  DigiByte fees are tiny
     * so rough is fine - used by the dryrun option of the asset RPC methods.
     */
    uint64_t estimateMinerFee(const DigiByteTransaction& tx);

    /**
     * Takes a fully described asset transaction(inputs, outputs and issuance data set),
     * then funds(pays the fee), signs and broadcasts it via the DigiByte wallet.
     *
     * While funding, all wallet UTXOs that carry assets or are unconfirmed get temporarily
     * locked so the wallet can not accidentally spend assets as transaction fees.
     *
     * @param tx - transaction to send(must be writable and pass encodeAssetOpReturn)
     * @param signedHex - optional output of the raw signed transaction hex
     * @return - txid of the broadcast transaction
     */
    std::string fundSignSend(const DigiByteTransaction& tx, std::string* signedHex = nullptr);

} // namespace AssetWallet

#endif //DIGIASSET_CORE_ASSETWALLET_H
