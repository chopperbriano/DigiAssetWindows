//
// Created by mctrivia on 04/04/24.
//
// Declares RPC::Response, the object every JSON-RPC method returns. It holds the
// result (or error) payload plus cache-lifetime metadata used by the RPC
// server's response cache: how many new blocks the answer stays valid for, which
// wallet addresses invalidate it when their state changes, and whether issuing a
// new asset invalidates it. It also tracks an estimated memory footprint so the
// cache can bound its total size.
//

#ifndef DIGIASSET_CORE_RESPONSE_H
#define DIGIASSET_CORE_RESPONSE_H


#include <jsoncpp/json/value.h>
namespace RPC {
    /**
    * A single JSON-RPC reply together with the rules that decide how long it may
    * be cached. Methods populate it via setResult/setError and the various
    * setInvalidate/setBlocksGoodFor calls; the RPC server later queries the
    * addressChanged/newBlockAdded/newAssetIssued predicates to evict stale
    * entries and calls toJSON() to serialize the reply to the client.
    */
    class Response {
    private:
        Json::Value _result;
        bool _error=false;
        size_t _size=sizeof(Response);

        std::vector<std::string> _invalidateOnAddressChange; //invalidates if one of these addresses is modified
        int _blocksGoodFor=0;  //invalidates when drops bellow 0
        bool _invalidateOnNewAsset=false;  //if true will get deleted if a new asset is issued

    public:
        Response()=default;

        //functions for setting response value
        void setResult(const Json::Value& result);     //store a success payload (clears error flag)
        void setError(const Json::Value& error);       //store an error payload (sets error flag)

        //functions for setting how long cache is good for
        void addInvalidateOnAddressChange(const std::string& address);  //evict when this address's state changes
        void setBlocksGoodFor(int blocks);                              //valid for N more blocks; <0 means never cache
        void setInvalidateOnNewAsset();                                 //evict as soon as any new asset is issued

        //function to detect if there was no response
        bool empty() const;                            //true if nothing was ever stored in this Response
        size_t size() const;                           //estimated memory footprint in bytes
        bool isCacheable() const;                      //false if blocksGoodFor<0 (never cache - e.g. live status)

        //functions to check if the cache should get deleted
        size_t addressChanged(const std::string& address) const;    //returns size if should delete, 0 if shouldn't
        size_t newBlockAdded();                                     //returns size if should delete, 0 if shouldn't
        size_t newAssetIssued();                                    //returns size if should delete, 0 if shouldn't

        //functions to convert to Json
        Json::Value toJSON(const Json::Value& id) const;   //serialize to the JSON-RPC {result,error,id} envelope
    };
}



#endif //DIGIASSET_CORE_RESPONSE_H
