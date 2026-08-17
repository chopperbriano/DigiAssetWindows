//
// Created by mctrivia on 16/08/26.
//

#ifndef DIGIASSET_CORE_DIGIDOLLAR_H
#define DIGIASSET_CORE_DIGIDOLLAR_H

#include "DigiByteCore_Types.h"
#include <cstdint>
#include <string>
#include <vector>

/**
 * Decoders for the DigiDollar protocol that activated on DigiByte mainnet at block 23,869,440.
 *
 * DigiAsset Core never validates DigiDollar - the node does that.  Everything here is a reader
 * for data the node has already accepted, so the decoders are deliberately strict and return
 * false rather than throwing when a script does not match the expected shape.
 *
 * Two independent encodings are involved:
 *
 * 1. Transaction metadata, in an OP_RETURN of the DigiDollar transaction itself:
 *      OP_RETURN "DD" <txType> <fields...>
 *    All chunks are ordinary pushes, so the node reports this output as "nulldata".
 *
 * 2. The oracle price commitment, in the coinbase of the block that closes an epoch:
 *      OP_RETURN OP_ORACLE <0x03> <bundle>
 *    OP_ORACLE (0xbf) is not push only, so the node reports this output as "nonstandard".
 */
namespace DigiDollar {

    /**
     * Transaction types, taken from the high byte of the transaction version.
     * Values match the txType field written into the "DD" OP_RETURN.
     */
    enum TxType : uint8_t {
        TX_NONE = 0,
        TX_MINT = 1,
        TX_TRANSFER = 2,
        TX_REDEEM = 3
    };

    /**
     * Decoded contents of a DigiDollar transaction's OP_RETURN.
     *
     * amounts are in DigiDollar cents (100 == $1.00) and map positionally onto the
     * transaction's DigiDollar outputs - see mapAmountsToOutputs().
     */
    struct Metadata {
        TxType type = TX_NONE;
        std::vector<uint64_t> amounts;

        //mint only
        uint64_t lockHeight = 0;
        uint8_t lockTier = 0;
        std::string ownerKey; //x only pubkey hex, used to rebuild the expected vault output
    };

    /**
     * Decoded oracle price commitment.
     *
     * price is micro USD per DGB, exactly as the oracles publish it.  4216 means one DGB is
     * worth $0.004216.  timestamp is when the oracles sampled the price, which is a little
     * before the block time, not the block time itself.
     */
    struct OracleCommitment {
        uint32_t epoch = 0;
        uint64_t price = 0;
        int64_t timestamp = 0;
        uint8_t participants = 0;
        uint8_t version = 0;
    };

    /**
     * True if the transaction version carries the DigiDollar marker.  This is the cheapest
     * possible test and is the gate every other decode sits behind.
     */
    bool isDigiDollarVersion(int version);

    /**
     * Transaction type from the version field.  Returns TX_NONE if the marker is absent or the
     * high byte is not a type we understand.
     */
    TxType typeFromVersion(int version);

    /**
     * Decode the "DD" OP_RETURN of a DigiDollar transaction.
     *
     * @param txData    the transaction, already fetched from the node
     * @param height    block height, used to reject anything below activation
     * @param metadata  filled in on success
     * @return true if this is a DigiDollar transaction with well formed metadata
     */
    bool decodeMetadata(const getrawtransaction_t& txData, unsigned int height, Metadata& metadata);

    /**
     * Decode an oracle price commitment from a coinbase output script.
     *
     * @param scriptHex  scriptPubKey hex of the output
     * @param commitment filled in on success
     * @return true if the script is a well formed v0x03 oracle commitment
     */
    bool decodeOracleCommitment(const std::string& scriptHex, OracleCommitment& commitment);

    /**
     * Find the oracle price commitment in a block's coinbase transaction.
     * Not every block carries one - only the block that closes an epoch does, and only if the
     * miner was running an oracle enabled build.
     *
     * @return true if a commitment was found and decoded
     */
    bool findOracleCommitment(const getrawtransaction_t& coinbase, OracleCommitment& commitment);

    /**
     * True if a scriptPubKey is an oracle price commitment.  Used by the RPC layer to classify
     * an output the node calls "nonstandard" so it does not get logged as unexpected.
     */
    bool isOracleCommitmentScript(const std::string& scriptHex);

    /**
     * Map the metadata amounts onto transaction outputs.
     *
     * DigiDollar value lives in P2TR outputs that carry 0 DGB.  The OP_RETURN lists the amounts
     * in the same order those outputs appear, so the Nth zero valued taproot output holds the
     * Nth amount.  A mint's collateral vault is also P2TR but carries the locked DGB, so a non
     * zero value is what separates a vault from a DigiDollar output.
     *
     * @return one entry per DigiDollar output, as (vout index, amount in cents).  Empty if the
     *         amount count does not match the output count, which means we misread the tx and
     *         must not record anything.
     */
    std::vector<std::pair<uint16_t, uint64_t>> mapAmountsToOutputs(const getrawtransaction_t& txData,
                                                                  const Metadata& metadata);

    /**
     * Find a mint's collateral vault output - the only P2TR output carrying DGB.
     * @return vout index, or -1 if there is no single unambiguous vault output
     */
    int findVaultOutput(const getrawtransaction_t& txData);

    /**
     * Convert an oracle price into the sats per unit convention the exchange rate table uses.
     *
     * The exchange table stores "how many DGB sats is one unit worth", so that
     * amount * rate / 1e8 converts into sats.  The oracle publishes the inverse (micro USD per
     * DGB), so one USD is 1e14 / price sats.
     *
     * @param priceMicroUSD micro USD per DGB, as published
     * @return DGB sats per USD, or 0 if price is 0
     */
    double priceToExchangeRate(uint64_t priceMicroUSD);

} // namespace DigiDollar

#endif //DIGIASSET_CORE_DIGIDOLLAR_H
