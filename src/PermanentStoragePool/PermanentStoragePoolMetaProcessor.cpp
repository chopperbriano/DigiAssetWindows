//
// Created by mctrivia on 04/11/23.
//
//
// PermanentStoragePoolMetaProcessor.cpp - implementation of the shared
// shouldPinFile() wrapper (see header). Bridges a pool's pinning policy to the
// analyzer Database so that any file the pool decides to keep is recorded in
// the permanent-pin list under the pool's index.
//

#include "PermanentStoragePoolMetaProcessor.h"
#include "AppMain.h"
#include "Database.h"
PermanentStoragePoolMetaProcessor::PermanentStoragePoolMetaProcessor(unsigned int poolIndex) {
    _poolIndex = poolIndex;
}


/**
 * Checks with sub class if file should be pinned and adds to the pin database if it should.
 * @param name
 * @param mimeType
 * @param cid
 * @return
 */
bool PermanentStoragePoolMetaProcessor::shouldPinFile(const std::string& name, const std::string& mimeType, const std::string& cid) {
    bool shouldPin = _shouldPinFile(name, mimeType, cid);
    if (shouldPin) {
        Database* db = AppMain::GetInstance()->getDatabase();
        db->addToPermanent(_poolIndex, cid);
    }
    return shouldPin;
}
