//
// Created by mctrivia on 16/08/26.
//

#include "DigiDollar.h"
#include "Blob.h"
#include "DigiAssetConstants.h"
#include <cstring>

using namespace std;

namespace DigiDollar {

    namespace {

        const uint8_t OP_RETURN_BYTE = 0x6a;
        const uint8_t OP_ORACLE_BYTE = 0xbf;
        const uint8_t OP_PUSHDATA1 = 0x4c;
        const uint8_t OP_PUSHDATA2 = 0x4d;
        const uint8_t OP_PUSHDATA4 = 0x4e;
        const uint8_t OP_0 = 0x00;
        const uint8_t OP_1NEGATE = 0x4f;
        const uint8_t OP_1 = 0x51;
        const uint8_t OP_16 = 0x60;

        /**
         * A single decoded element of a script.  DigiByte Core writes small integers as the
         * compact OP_0/OP_1..OP_16 opcodes rather than as pushes, so a reader that only handles
         * pushdata silently loses fields like a lockTier of 3.  Keeping the numeric value
         * alongside the pushed bytes lets callers read either form.
         */
        struct ScriptChunk {
            vector<uint8_t> data; //pushed bytes, empty for a compact integer
            int64_t number = 0;   //value if this chunk was a compact integer
            bool isNumber = false;
        };

        /**
         * Split a script into its chunks, starting at byte offset start.
         * Returns false on a truncated or malformed push rather than reading past the end.
         */
        bool splitScript(const vector<uint8_t>& script, size_t start, vector<ScriptChunk>& chunks) {
            chunks.clear();
            size_t i = start;
            while (i < script.size()) {
                uint8_t op = script[i];
                size_t length;
                if (op < OP_PUSHDATA1) {
                    length = op;
                    i++;
                } else if (op == OP_PUSHDATA1) {
                    if (i + 1 >= script.size()) return false;
                    length = script[i + 1];
                    i += 2;
                } else if (op == OP_PUSHDATA2) {
                    if (i + 2 >= script.size()) return false;
                    length = static_cast<size_t>(script[i + 1]) | (static_cast<size_t>(script[i + 2]) << 8);
                    i += 3;
                } else if (op == OP_PUSHDATA4) {
                    if (i + 4 >= script.size()) return false;
                    length = static_cast<size_t>(script[i + 1]) |
                             (static_cast<size_t>(script[i + 2]) << 8) |
                             (static_cast<size_t>(script[i + 3]) << 16) |
                             (static_cast<size_t>(script[i + 4]) << 24);
                    i += 5;
                } else if (op == OP_0) {
                    ScriptChunk chunk;
                    chunk.isNumber = true;
                    chunk.number = 0;
                    chunks.push_back(chunk);
                    i++;
                    continue;
                } else if (op == OP_1NEGATE) {
                    ScriptChunk chunk;
                    chunk.isNumber = true;
                    chunk.number = -1;
                    chunks.push_back(chunk);
                    i++;
                    continue;
                } else if ((op >= OP_1) && (op <= OP_16)) {
                    ScriptChunk chunk;
                    chunk.isNumber = true;
                    chunk.number = op - (OP_1 - 1);
                    chunks.push_back(chunk);
                    i++;
                    continue;
                } else {
                    return false; //a real opcode, not data
                }

                if (i + length > script.size()) return false;
                ScriptChunk chunk;
                chunk.data.assign(script.begin() + i, script.begin() + i + length);
                chunks.push_back(chunk);
                i += length;
            }
            return true;
        }

        /**
         * Read a chunk as an integer.  Handles both the compact integer opcodes and CScriptNum's
         * little endian sign-magnitude encoding, which is what larger values are pushed as.
         */
        bool chunkToInt(const ScriptChunk& chunk, int64_t& value) {
            if (chunk.isNumber) {
                value = chunk.number;
                return true;
            }
            if (chunk.data.empty()) {
                value = 0;
                return true;
            }
            if (chunk.data.size() > 8) return false;
            int64_t result = 0;
            for (size_t i = 0; i < chunk.data.size(); i++) {
                result |= static_cast<int64_t>(chunk.data[i]) << (8 * i);
            }
            //CScriptNum sets the high bit of the last byte to mark a negative number
            if (chunk.data.back() & 0x80) {
                result &= ~(static_cast<int64_t>(0x80) << (8 * (chunk.data.size() - 1)));
                result = -result;
            }
            value = result;
            return true;
        }

        uint32_t readLE32(const vector<uint8_t>& data, size_t offset) {
            return static_cast<uint32_t>(data[offset]) |
                   (static_cast<uint32_t>(data[offset + 1]) << 8) |
                   (static_cast<uint32_t>(data[offset + 2]) << 16) |
                   (static_cast<uint32_t>(data[offset + 3]) << 24);
        }

        uint64_t readLE64(const vector<uint8_t>& data, size_t offset) {
            uint64_t result = 0;
            for (size_t i = 0; i < 8; i++) {
                result |= static_cast<uint64_t>(data[offset + i]) << (8 * i);
            }
            return result;
        }

        vector<uint8_t> hexToBytes(const string& hex) {
            if (hex.empty() || (hex.size() % 2 != 0)) return {};
            try {
                return Blob(hex).vector();
            } catch (...) {
                return {};
            }
        }

        /**
         * True if an output is pay to taproot: OP_1 followed by a 32 byte push.
         * Every DigiDollar value output and every collateral vault uses this form.
         */
        bool isTaprootScript(const vector<uint8_t>& script) {
            return (script.size() == 34) && (script[0] == OP_1) && (script[1] == 32);
        }

        bool isTaprootOutput(const vout_t& output) {
            //trust the node's classification when it gives us one, fall back to the raw script
            if (output.scriptPubKey.type == "witness_v1_taproot") return true;
            if (!output.scriptPubKey.type.empty() && (output.scriptPubKey.type != "nonstandard")) return false;
            return isTaprootScript(hexToBytes(output.scriptPubKey.hex));
        }

    } // namespace

    bool isDigiDollarVersion(int version) {
        return (version & DigiAssetConstants::DIGIDOLLAR_VERSION_MASK) ==
               DigiAssetConstants::DIGIDOLLAR_VERSION_MARKER;
    }

    TxType typeFromVersion(int version) {
        if (!isDigiDollarVersion(version)) return TX_NONE;
        uint8_t type = static_cast<uint8_t>((static_cast<uint32_t>(version) >> 24) & 0xFF);
        switch (type) {
            case TX_MINT:
            case TX_TRANSFER:
            case TX_REDEEM:
                return static_cast<TxType>(type);
            default:
                return TX_NONE;
        }
    }

    bool decodeMetadata(const getrawtransaction_t& txData, unsigned int height, Metadata& metadata) {
        metadata = Metadata{};
        if (height < DigiAssetConstants::DIGIDOLLAR_ACTIVATION_HEIGHT) return false;

        TxType versionType = typeFromVersion(txData.version);
        if (versionType == TX_NONE) return false;

        //find the "DD" marked OP_RETURN.  A transaction may carry more than one OP_RETURN - a
        //DigiAsset transaction that also moves DigiDollar would - so match on the marker rather
        //than taking the first or last nulldata output.
        for (const vout_t& output: txData.vout) {
            if (output.scriptPubKey.type != "nulldata") continue;
            vector<uint8_t> script = hexToBytes(output.scriptPubKey.hex);
            if (script.size() < 4) continue;
            if (script[0] != OP_RETURN_BYTE) continue;

            vector<ScriptChunk> chunks;
            if (!splitScript(script, 1, chunks)) continue;
            if (chunks.size() < 2) continue;

            //chunk 0 must be the two byte "DD" marker
            if (chunks[0].isNumber || (chunks[0].data.size() != 2)) continue;
            if ((chunks[0].data[0] != 'D') || (chunks[0].data[1] != 'D')) continue;

            //chunk 1 is the transaction type and must agree with the version field
            int64_t rawType;
            if (!chunkToInt(chunks[1], rawType)) continue;
            if (rawType != static_cast<int64_t>(versionType)) return false; //version and payload disagree

            metadata.type = versionType;

            if (versionType == TX_MINT) {
                //OP_RETURN "DD" <1> <ddAmount> <lockHeight> <lockTier> <ownerXOnlyPubKey>
                if (chunks.size() < 6) return false;
                int64_t amount, lockHeight, lockTier;
                if (!chunkToInt(chunks[2], amount) || (amount <= 0)) return false;
                if (!chunkToInt(chunks[3], lockHeight) || (lockHeight < 0)) return false;
                if (!chunkToInt(chunks[4], lockTier) || (lockTier < 0) || (lockTier > 9)) return false;
                if (chunks[5].isNumber || (chunks[5].data.size() != 32)) return false;
                metadata.amounts.push_back(static_cast<uint64_t>(amount));
                metadata.lockHeight = static_cast<uint64_t>(lockHeight);
                metadata.lockTier = static_cast<uint8_t>(lockTier);
                metadata.ownerKey = Blob(chunks[5].data).toHex();
                return true;
            }

            //TRANSFER: OP_RETURN "DD" <2> <amount1> ... <amountN>
            //REDEEM:   OP_RETURN "DD" <3> <ddChangeAmount>   (omitted entirely when no change)
            for (size_t i = 2; i < chunks.size(); i++) {
                int64_t amount;
                if (!chunkToInt(chunks[i], amount) || (amount < 0)) return false;
                metadata.amounts.push_back(static_cast<uint64_t>(amount));
            }
            if (metadata.amounts.empty() && (versionType == TX_TRANSFER)) return false;
            return true;
        }

        //A redemption that produces no DigiDollar change carries no OP_RETURN at all.  The
        //version marker is still authoritative, so report it with an empty amount list.
        if (versionType == TX_REDEEM) {
            metadata.type = TX_REDEEM;
            return true;
        }
        return false;
    }

    bool isOracleCommitmentScript(const string& scriptHex) {
        //6a = OP_RETURN, bf = OP_ORACLE.  Compare on bytes so casing of the hex never matters.
        vector<uint8_t> script = hexToBytes(scriptHex);
        return (script.size() >= 4) && (script[0] == OP_RETURN_BYTE) && (script[1] == OP_ORACLE_BYTE);
    }

    bool decodeOracleCommitment(const string& scriptHex, OracleCommitment& commitment) {
        commitment = OracleCommitment{};
        vector<uint8_t> script = hexToBytes(scriptHex);
        if (script.size() < 4) return false;
        if ((script[0] != OP_RETURN_BYTE) || (script[1] != OP_ORACLE_BYTE)) return false;

        //the version byte and the bundle are pushed as separate chunks, concatenate them back
        vector<ScriptChunk> chunks;
        if (!splitScript(script, 2, chunks)) return false;
        vector<uint8_t> data;
        for (const ScriptChunk& chunk: chunks) {
            if (chunk.isNumber) return false; //bundle is never compact encoded
            data.insert(data.end(), chunk.data.begin(), chunk.data.end());
        }

        //version(1) bitmapLen(1) bitmap(bitmapLen) epoch(4) price(8) timestamp(8) sig(64)
        if (data.size() < 87) return false;
        if (data[0] != 0x03) return false; //DigiDollar V1 launched MuSig2 only, nothing else is valid
        uint8_t bitmapLength = data[1];
        if (bitmapLength == 0) return false;
        size_t expected = 1 + 1 + bitmapLength + 4 + 8 + 8 + 64;
        if (data.size() != expected) return false;

        size_t offset = 2;
        uint8_t participants = 0;
        for (uint8_t i = 0; i < bitmapLength; i++) {
            uint8_t byte = data[offset + i];
            while (byte != 0) {
                participants += (byte & 1);
                byte >>= 1;
            }
        }
        offset += bitmapLength;

        commitment.version = data[0];
        commitment.participants = participants;
        commitment.epoch = readLE32(data, offset);
        offset += 4;
        commitment.price = readLE64(data, offset);
        offset += 8;
        commitment.timestamp = static_cast<int64_t>(readLE64(data, offset));

        if (commitment.price == 0) return false;
        return true;
    }

    bool findOracleCommitment(const getrawtransaction_t& coinbase, OracleCommitment& commitment) {
        for (const vout_t& output: coinbase.vout) {
            //the node reports this output as nonstandard because OP_ORACLE is not push only,
            //but we relabel it "oracle" when parsing so accept either
            if (!isOracleCommitmentScript(output.scriptPubKey.hex)) continue;
            if (decodeOracleCommitment(output.scriptPubKey.hex, commitment)) return true;
        }
        return false;
    }

    vector<pair<uint16_t, uint64_t>> mapAmountsToOutputs(const getrawtransaction_t& txData,
                                                        const Metadata& metadata) {
        vector<pair<uint16_t, uint64_t>> result;
        if (metadata.amounts.empty()) return result;

        //DigiDollar outputs are the taproot outputs carrying no DGB.  A mint's collateral vault
        //is also taproot but holds the locked DGB, which is what keeps the two apart.
        vector<uint16_t> ddOutputs;
        for (const vout_t& output: txData.vout) {
            if (output.valueS != 0) continue;
            if (!isTaprootOutput(output)) continue;
            ddOutputs.push_back(static_cast<uint16_t>(output.n));
        }

        //A mismatch means we misread the transaction.  Recording a guess would put wrong
        //balances in the database that no later block could correct, so record nothing.
        if (ddOutputs.size() != metadata.amounts.size()) return result;

        for (size_t i = 0; i < ddOutputs.size(); i++) {
            if (metadata.amounts[i] == 0) continue; //zero value output carries nothing
            result.emplace_back(ddOutputs[i], metadata.amounts[i]);
        }
        return result;
    }

    int findVaultOutput(const getrawtransaction_t& txData) {
        int found = -1;
        for (const vout_t& output: txData.vout) {
            if (output.valueS == 0) continue;
            if (!isTaprootOutput(output)) continue;
            if (found != -1) return -1; //ambiguous, refuse to guess
            found = static_cast<int>(output.n);
        }
        return found;
    }

    double priceToExchangeRate(uint64_t priceMicroUSD) {
        if (priceMicroUSD == 0) return 0;
        //1 DGB == price/1e6 USD, so 1 USD == 1e6/price DGB == 1e14/price sats
        return 1e14 / static_cast<double>(priceMicroUSD);
    }

} // namespace DigiDollar
