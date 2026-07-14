//
// Created by DigiAsset Core on 14/07/26.
//

#ifndef BALANCESTAB_H
#define BALANCESTAB_H

#include "DigiByteCore.h"
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QWidget>

/**
 * Shows the wallet's DigiByte balance and all DigiAssets it holds.
 * Data comes from the getwalletbalances RPC method.
 */
class BalancesTab : public QWidget {
    Q_OBJECT

public:
    explicit BalancesTab(QWidget *parent = nullptr);

private slots:
    void updateBalances();

private:
    QLabel * _digibyteLabel;
    QLabel * _statusLabel;
    QPushButton * _refreshButton;
    QTableWidget * _assetTable;
    QTimer * _timer;
    DigiByteCore _dgbCore;
};

#endif // BALANCESTAB_H
