//
// Unit tests for the pool's storage payout plan (pool/StoragePlan.h). Included by
// relative path like PayoutMathTest: the pool exe is not part of the test target,
// and the header is standalone.
//
// What has to hold, because it is what decides who gets paid:
//   - a capped paid set, so one more node does not shrink everyone's share;
//   - a fixed number of paid copies per file, assigned stably;
//   - weight by bytes, not by file count;
//   - a node that proves storage outranks one that only announces it;
//   - a node holding nothing never takes a paid slot.
//

#include "gtest/gtest.h"
#include "../pool/StoragePlan.h"

using namespace StoragePlan;

namespace {
    Node node(const std::string& id, double reliability = 1.0, int64_t firstSeen = 100,
              double proof = 1.0, int64_t answered = 10, double coverage = 0.0) {
        Node n;
        n.peerId = id;
        n.reliability = reliability;
        n.firstSeen = firstSeen;
        n.proofScore = proof;
        n.proofAnswered = answered;
        n.coverage = coverage;
        return n;
    }

    std::vector<Cid> files(int count, int64_t size = 1000) {
        std::vector<Cid> out;
        for (int i = 0; i < count; i++) out.push_back({"bafy" + std::to_string(i), size});
        return out;
    }

    const Weight* find(const std::vector<Weight>& ws, const std::string& id) {
        for (const auto& w: ws) if (w.peerId == id) return &w;
        return nullptr;
    }
}

// ---- the paid set ----------------------------------------------------------

TEST(StoragePlan, CapsThePaidSetSoANewNodeDoesNotDiluteEveryone) {
    Config cfg; cfg.maxPaidNodes = 3; cfg.replicas = 3;
    std::vector<Node> nodes = {node("a", 1.0, 1), node("b", 1.0, 2), node("c", 1.0, 3)};
    auto before = weights(nodes, files(30), cfg);

    nodes.push_back(node("late", 1.0, 99));   // a fourth, equally good node arrives
    auto after = weights(nodes, files(30), cfg);

    ASSERT_EQ(after.size(), 3u);
    EXPECT_EQ(find(after, "late"), nullptr);   // waits for a slot
    EXPECT_DOUBLE_EQ(find(after, "a")->weight, find(before, "a")->weight);
}

TEST(StoragePlan, RanksByReliabilityThenSeniority) {
    Config cfg; cfg.maxPaidNodes = 2;
    auto paid = paidSet({node("old-flaky", 0.5, 1), node("new-solid", 1.0, 50), node("old-solid", 1.0, 10)}, cfg);
    ASSERT_EQ(paid.size(), 2u);
    EXPECT_EQ(paid[0].peerId, "old-solid");
    EXPECT_EQ(paid[1].peerId, "new-solid");
}

TEST(StoragePlan, ANodeHoldingNothingNeverTakesAPaidSlot) {
    Config cfg; cfg.maxPaidNodes = 1;
    // Most reliable, but every challenge failed: held = 0.
    auto paid = paidSet({node("reachable-empty", 1.0, 1, 0.0, 20), node("holder", 0.8, 5)}, cfg);
    ASSERT_EQ(paid.size(), 1u);
    EXPECT_EQ(paid[0].peerId, "holder");
}

TEST(StoragePlan, NoCapMeansEveryoneWithSomethingToShowIsPaid) {
    Config cfg; cfg.maxPaidNodes = 0;
    EXPECT_EQ(paidSet({node("a"), node("b"), node("c"), node("d")}, cfg).size(), 4u);
}

// ---- paid copies per file --------------------------------------------------

TEST(StoragePlan, AssignsEachFileToExactlyTheConfiguredNumberOfNodes) {
    Config cfg; cfg.replicas = 2;
    std::vector<Node> nodes = {node("a"), node("b"), node("c"), node("d"), node("e")};
    auto paid = paidSet(nodes, cfg);
    for (const auto& c: files(50)) EXPECT_EQ(assignees(c.cid, paid, 2).size(), 2u);

    auto ws = weights(nodes, files(50), cfg);
    unsigned int total = 0;
    for (const auto& w: ws) total += w.assignedCids;
    EXPECT_EQ(total, 100u);   // 50 files x 2 copies, no more
}

TEST(StoragePlan, LosingANodeMovesOnlyTheFilesItHeld) {
    Config cfg; cfg.replicas = 1;
    std::vector<Node> five = {node("a"), node("b"), node("c"), node("d"), node("e")};
    std::vector<Node> four = {node("a"), node("b"), node("c"), node("d")};
    auto p5 = paidSet(five, cfg), p4 = paidSet(four, cfg);
    int moved = 0;
    for (const auto& c: files(200)) {
        auto before = assignees(c.cid, p5, 1)[0];
        auto after = assignees(c.cid, p4, 1)[0];
        if (before != "e") EXPECT_EQ(before, after) << c.cid;   // untouched unless "e" held it
        if (before != after) moved++;
    }
    EXPECT_GT(moved, 0);
}

TEST(StoragePlan, FewerNodesThanCopiesAssignsToAllOfThem) {
    Config cfg; cfg.replicas = 5;
    auto paid = paidSet({node("a"), node("b")}, cfg);
    EXPECT_EQ(assignees("bafyx", paid, 5).size(), 2u);
}

// ---- weight by bytes -------------------------------------------------------

TEST(StoragePlan, WeighsBytesNotFileCount) {
    Config cfg; cfg.replicas = 1;
    std::vector<Node> nodes = {node("a"), node("b")};
    std::vector<Cid> cids = {{"big", 10'000'000}, {"s1", 1'000}, {"s2", 1'000}, {"s3", 1'000}};
    auto ws = weights(nodes, cids, cfg);
    auto paid = paidSet(nodes, cfg);
    std::string bigHolder = assignees("big", paid, 1)[0];
    EXPECT_GE(find(ws, bigHolder)->assignedBytes, 10'000'000);
    // Whoever holds the big file outweighs a node holding only small ones.
    std::string other = bigHolder == "a" ? "b" : "a";
    EXPECT_GT(find(ws, bigHolder)->weight, find(ws, other)->weight);
}

TEST(StoragePlan, AnUnmeasuredFileStillCountsSomething) {
    Config cfg; cfg.replicas = 1; cfg.unknownSizeBytes = 7;
    auto ws = weights({node("a")}, {{"unsized", 0}}, cfg);
    EXPECT_EQ(ws[0].assignedBytes, 7);
}

// ---- proving beats announcing ----------------------------------------------

TEST(StoragePlan, ANodeThatProvesOutweighsOneOnlyAnnouncing) {
    Config cfg; cfg.replicas = 2; cfg.legacyFactor = 0.5;
    Node prover = node("prover", 1.0, 1, 1.0, 10);
    Node announcer = node("announcer", 1.0, 1, 0.0, 0, 1.0);   // never answered, full DHT coverage
    auto ws = weights({prover, announcer}, files(10), cfg);
    EXPECT_FALSE(find(ws, "prover")->legacy);
    EXPECT_TRUE(find(ws, "announcer")->legacy);
    EXPECT_DOUBLE_EQ(find(ws, "announcer")->held, 0.5);
    EXPECT_DOUBLE_EQ(find(ws, "announcer")->weight * 2.0, find(ws, "prover")->weight);
}

TEST(StoragePlan, OnceANodeHasAnsweredItIsJudgedByItsProofsOnly) {
    Config cfg;
    // Answered challenges and failed them: its DHT coverage no longer counts.
    EXPECT_DOUBLE_EQ(heldScore(node("x", 1.0, 1, 0.0, 3, 1.0), cfg), 0.0);
}

// ---- challenges ------------------------------------------------------------

TEST(StoragePlan, ChallengeRangeStaysInsideTheFile) {
    for (uint64_t seed = 0; seed < 500; seed++) {
        auto r = challengeRange(200'000, seed * 7919);
        EXPECT_EQ(r.length, CHALLENGE_MAX_BYTES);
        EXPECT_GE(r.offset, 0);
        EXPECT_LE(r.offset + r.length, 200'000);
    }
    auto small = challengeRange(100, 12345);
    EXPECT_EQ(small.offset, 0);
    EXPECT_EQ(small.length, 100);
    EXPECT_EQ(challengeRange(0, 1).length, 0);   // unmeasured: nothing to ask
}

TEST(StoragePlan, ProofScoreRisesOnPassAndFallsOnFail) {
    double s = 0.0;
    for (int i = 0; i < 30; i++) s = nextProofScore(s, true);
    EXPECT_GT(s, 0.99);
    double after = nextProofScore(s, false);
    EXPECT_LT(after, s);
    EXPECT_NEAR(after, s * (1.0 - PROOF_EMA), 1e-12);
}

TEST(StoragePlan, HashComparisonIgnoresCaseAndWhitespaceButNotContent) {
    std::string h(64, 'a');
    EXPECT_TRUE(sameHash(h, " " + std::string(64, 'A') + "\n"));
    EXPECT_FALSE(sameHash(h, std::string(64, 'b')));
    EXPECT_FALSE(sameHash("", ""));                 // an empty answer is never right
    EXPECT_FALSE(sameHash(h.substr(0, 63), h.substr(0, 63)));
}

TEST(StoragePlan, ReadsItsKnobsFromPoolCfgAndKeepsDefaultsOnJunk) {
    auto c = configFrom({{"poolreplicas", "4"}, {"poolmaxpaidnodes", "12"}, {"poollegacyweight", "0.25"}});
    EXPECT_EQ(c.replicas, 4u);
    EXPECT_EQ(c.maxPaidNodes, 12u);
    EXPECT_DOUBLE_EQ(c.legacyFactor, 0.25);
    auto d = configFrom({{"poolreplicas", "lots"}});
    EXPECT_EQ(d.replicas, 3u);
    EXPECT_EQ(d.maxPaidNodes, 10u);                 // capped unless the operator says otherwise
    EXPECT_EQ(configFrom({{"poolmaxpaidnodes", "0"}}).maxPaidNodes, 0u);   // 0 still turns it off
}
