//
// Tests for PermanentStoragePoolMetaProcessor.
//

#include "AppMain.h"
#include "PermanentStoragePool/PermanentStoragePoolMetaProcessor.h"
#include "gtest/gtest.h"
#include "RPCMethods.h"

#include <string>

using namespace std;

// ─────────────────────────────────────────────────────────────────────────────
// Concrete subclass that always pins or never pins based on constructor arg
// ─────────────────────────────────────────────────────────────────────────────

class AlwaysPinProcessor : public PermanentStoragePoolMetaProcessor {
public:
    explicit AlwaysPinProcessor(unsigned int poolIndex)
        : PermanentStoragePoolMetaProcessor(poolIndex) {}
protected:
    bool _shouldPinFile(const string&, const string&, const string&) override {
        return true;
    }
};

class NeverPinProcessor : public PermanentStoragePoolMetaProcessor {
public:
    explicit NeverPinProcessor(unsigned int poolIndex)
        : PermanentStoragePoolMetaProcessor(poolIndex) {}
protected:
    bool _shouldPinFile(const string&, const string&, const string&) override {
        return false;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Fixture — reuse RPCMethodsTest which wires up AppMain + Database
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(RPCMethodsTest, metaProcessor_neverPin_returnsFalse) {
    NeverPinProcessor proc(0);
    EXPECT_FALSE(proc.shouldPinFile("file.png", "image/png", "QmTestCid1"));
}

TEST_F(RPCMethodsTest, metaProcessor_alwaysPin_returnsTrue) {
    AlwaysPinProcessor proc(0);
    EXPECT_TRUE(proc.shouldPinFile("file.png", "image/png", "QmTestCid2"));
}

TEST_F(RPCMethodsTest, metaProcessor_alwaysPin_differentMimeTypes) {
    AlwaysPinProcessor proc(0);
    EXPECT_TRUE(proc.shouldPinFile("doc.pdf", "application/pdf", "QmTestCid3"));
    EXPECT_TRUE(proc.shouldPinFile("data.json", "application/json", "QmTestCid4"));
}

TEST_F(RPCMethodsTest, metaProcessor_neverPin_differentPoolIndex) {
    NeverPinProcessor proc(1);
    EXPECT_FALSE(proc.shouldPinFile("vid.mp4", "video/mp4", "QmTestCid5"));
}
