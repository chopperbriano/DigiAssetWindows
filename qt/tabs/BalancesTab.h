//
// Created by DigiAsset Core on 14/07/26.
//

#ifndef BALANCESTAB_H
#define BALANCESTAB_H

#include "DigiByteCore.h"
#include <QLabel>
#include <QNetworkAccessManager>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QWidget>
#include <map>
#include <set>

/**
 * Shows the wallet's DigiByte balance and all DigiAssets it holds.
 * Data comes from the getwalletbalances RPC method; asset names and icons come from the
 * asset metadata(getassetdata + the metadata's data.urls "icon" entry fetched over IPFS).
 */
class BalancesTab : public QWidget {
    Q_OBJECT

public:
    explicit BalancesTab(QWidget *parent = nullptr);

private slots:
    void updateBalances();

private:
    void loadAssetInfo(uint64_t assetIndex);
    void fetchIcon(uint64_t assetIndex, const QString& cid);
    void applyIcon(uint64_t assetIndex);

    QLabel * _digibyteLabel;
    QLabel * _statusLabel;
    QPushButton * _refreshButton;
    QTableWidget * _assetTable;
    QTimer * _timer;
    QNetworkAccessManager * _net;
    QString _ipfsApi; //e.g. http://localhost:5001/api/v0/
    std::map<uint64_t, QString> _names;      //assetIndex -> asset name(from metadata)
    std::map<uint64_t, QIcon> _icons;        //assetIndex -> fetched icon
    std::set<uint64_t> _infoLoaded;          //assetIndexes whose metadata was fetched
    std::set<uint64_t> _iconInFlight;        //icon downloads in progress
    DigiByteCore _dgbCore;
};

#endif // BALANCESTAB_H
