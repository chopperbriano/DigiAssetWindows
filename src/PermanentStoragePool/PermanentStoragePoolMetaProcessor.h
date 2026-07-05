//
// Created by mctrivia on 04/11/23.
//
//
// PermanentStoragePoolMetaProcessor.h - abstract per-transaction "what should
// this node pin" decider for a Permanent Storage Pool (PSP).
//
// When the chain analyzer processes an asset that belongs to a PSP, the pool's
// serializeMetaProcessor() emits a small serialized string and
// deserializeMetaProcessor() turns it into one of these objects. The analyzer
// then calls shouldPinFile() once for every file (name/mimeType/cid) contained
// in that asset. Each concrete pool subclasses this to encode its own pinning
// policy (e.g. "pin everything", "pin up to a paid byte budget"). Files that
// should be pinned are also recorded in the analyzer Database's permanent list,
// keyed by the owning pool's _poolIndex.
//

#ifndef DIGIASSET_CORE_PERMANENTSTORAGEPOOLMETAPROCESSOR_H
#define DIGIASSET_CORE_PERMANENTSTORAGEPOOLMETAPROCESSOR_H


#include <string>


/**
 * Abstract base for a pool's per-transaction pinning decider. Subclasses
 * implement _shouldPinFile() to define the pool's policy; the non-virtual
 * shouldPinFile() wraps that decision with the Database bookkeeping.
 * _poolIndex identifies which pool owns this processor (set at construction).
 */
class PermanentStoragePoolMetaProcessor {
protected:
    unsigned int _poolIndex;
    virtual bool _shouldPinFile(const std::string& name, const std::string& mimeType, const std::string& cid) = 0; //called for each file included in asset returns if file should be pinned
public:
    bool shouldPinFile(const std::string& name, const std::string& mimeType, const std::string& cid); //called for each file included in asset returns if file should be pinned
    explicit PermanentStoragePoolMetaProcessor(unsigned int poolIndex);
};



#endif //DIGIASSET_CORE_PERMANENTSTORAGEPOOLMETAPROCESSOR_H
