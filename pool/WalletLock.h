//
// One process-wide lock that serializes every pool-wallet SPEND sequence
// (walletpassphrase -> sign/send -> walletlock). Two subsystems drive the same
// DigiByte wallet from different threads:
//   - the payout path (PoolDashboard, on the dashboard thread), and
//   - the on-chain pool announce (PoolServer, on the discovery / peer worker).
// Without a shared lock, an announce can walletlock in the middle of a payout and
// the sendmany fails with -13 ("walletpassphrase first") - a spurious payout
// failure. Every unlock->spend->lock block must hold this lock.
//
// Construct-on-first-use, so there is no static-initialization-order dependency.
//

#ifndef DIGIASSET_POOL_WALLETLOCK_H
#define DIGIASSET_POOL_WALLETLOCK_H

#include <mutex>

inline std::mutex& poolWalletMutex() {
    static std::mutex m;
    return m;
}

#endif // DIGIASSET_POOL_WALLETLOCK_H
