//
// Created by DigiAsset Core on 14/07/26.
//

#ifndef BALANCESTAB_H
#define BALANCESTAB_H

#include "AssetIconProvider.h"
#include "DigiByteCore.h"
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QWidget>

/**
 * Shows the wallet's DigiByte balance, its DigiDollar balance, and all DigiAssets it holds.
 * Data comes from the getwalletbalances RPC method; asset names and icons come from the
 * asset metadata via the shared AssetIconProvider.
 *
 * DigiDollar is shown beside DigiByte rather than in the asset table because it is a native
 * balance rather than a DigiAsset - it has no assetIndex, assetId, icon or decimals.
 */
class BalancesTab : public QWidget {
    Q_OBJECT

public:
    explicit BalancesTab(QWidget *parent = nullptr);

private slots:
    void updateBalances();

private:
    void applyIcon(uint64_t assetIndex);
    void setDgbIcon();

    QLabel * _dgbIconLabel;
    QLabel * _digibyteLabel;
    QLabel * _digidollarLabel;
    QLabel * _statusLabel;
    QPushButton * _refreshButton;
    QTableWidget * _assetTable;
    QTimer * _timer;
    AssetIconProvider * _icons;
    DigiByteCore _dgbCore;
};

#endif // BALANCESTAB_H
