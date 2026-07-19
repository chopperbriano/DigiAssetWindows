#include "BalancesTab.h"
#include "Config.h"
#include <QHBoxLayout>
#include <QHeaderView>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QVBoxLayout>

BalancesTab::BalancesTab(QWidget *parent) : QWidget(parent), _dgbCore() {
    _dgbCore.setFileName("config.cfg", true);
    _dgbCore.makeConnection();
    _net = new QNetworkAccessManager(this);

    //the icon bytes come straight from the IPFS node's API(same one the daemon uses)
    try {
        Config config("config.cfg");
        _ipfsApi = QString::fromStdString(config.getString("ipfspath", "http://localhost:5001/api/v0/"));
    } catch (...) {
        _ipfsApi = "http://localhost:5001/api/v0/";
    }

    QVBoxLayout *layout = new QVBoxLayout(this);

    //DigiByte balance + refresh button on one row
    QHBoxLayout *topRow = new QHBoxLayout();
    _digibyteLabel = new QLabel("DigiByte: Loading...");
    topRow->addWidget(_digibyteLabel);
    topRow->addStretch();
    _refreshButton = new QPushButton("Refresh");
    connect(_refreshButton, &QPushButton::clicked, this, &BalancesTab::updateBalances);
    topRow->addWidget(_refreshButton);
    layout->addLayout(topRow);

    //asset table
    _assetTable = new QTableWidget(0, 5, this);
    _assetTable->setHorizontalHeaderLabels({"Name", "Asset ID", "Index", "Amount", "Decimals"});
    _assetTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    _assetTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _assetTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    _assetTable->setIconSize(QSize(24, 24));
    layout->addWidget(_assetTable);

    _statusLabel = new QLabel("");
    layout->addWidget(_statusLabel);

    _timer = new QTimer(this);
    connect(_timer, &QTimer::timeout, this, &BalancesTab::updateBalances);
    _timer->start(30000); //update every 30 seconds

    updateBalances();
}

/**
 * Fetches the asset's metadata once and caches its display name plus starts the icon
 * download if the metadata lists one(data.urls entry named "icon" on ipfs)
 */
void BalancesTab::loadAssetInfo(uint64_t assetIndex) {
    if (_infoLoaded.count(assetIndex)) return;
    _infoLoaded.insert(assetIndex);
    try {
        Json::Value args = Json::arrayValue;
        args.append(static_cast<Json::UInt64>(assetIndex));
        Json::Value data = _dgbCore.sendcommand("getassetdata", args);
        const Json::Value &meta = data["ipfs"]["data"];
        if (meta.isMember("assetName") && meta["assetName"].isString()) {
            _names[assetIndex] = QString::fromStdString(meta["assetName"].asString());
        }
        if (meta.isMember("urls") && meta["urls"].isArray()) {
            for (const auto &url: meta["urls"]) {
                if (!url.isObject() || !url.isMember("name") || !url.isMember("url")) continue;
                if (url["name"].asString() != "icon") continue;
                std::string target = url["url"].asString();
                if (target.rfind("ipfs://", 0) == 0) {
                    fetchIcon(assetIndex, QString::fromStdString(target.substr(7)));
                }
                break;
            }
        }
    } catch (const DigiByteException &) {
        //metadata may not have synced yet - try again on a later refresh
        _infoLoaded.erase(assetIndex);
    }
}

///downloads the icon bytes from the IPFS node(api "cat" call) and caches them as a QIcon
void BalancesTab::fetchIcon(uint64_t assetIndex, const QString &cid) {
    if (_iconInFlight.count(assetIndex) || _icons.count(assetIndex)) return;
    _iconInFlight.insert(assetIndex);
    QNetworkRequest request(QUrl(_ipfsApi + "cat?arg=" + cid));
    QNetworkReply *reply = _net->post(request, QByteArray());
    connect(reply, &QNetworkReply::finished, this, [this, assetIndex, reply]() {
        _iconInFlight.erase(assetIndex);
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;
        QPixmap pixmap;
        if (!pixmap.loadFromData(reply->readAll())) return;
        _icons[assetIndex] = QIcon(pixmap);
        applyIcon(assetIndex);
    });
}

///sets the cached icon on whichever row currently shows the asset
void BalancesTab::applyIcon(uint64_t assetIndex) {
    if (!_icons.count(assetIndex)) return;
    for (int row = 0; row < _assetTable->rowCount(); row++) {
        QTableWidgetItem *indexItem = _assetTable->item(row, 2);
        if ((indexItem != nullptr) && (indexItem->text().toULongLong() == assetIndex)) {
            QTableWidgetItem *nameItem = _assetTable->item(row, 0);
            if (nameItem != nullptr) nameItem->setIcon(_icons[assetIndex]);
        }
    }
}

void BalancesTab::updateBalances() {
    try {
        Json::Value args = Json::arrayValue;
        Json::Value result = _dgbCore.sendcommand("getwalletbalances", args);

        _digibyteLabel->setText("DigiByte: " + QString::fromStdString(result["digibyte"]["amount"].asString()) + " DGB");

        const Json::Value &assets = result["assets"];
        _assetTable->setRowCount(assets.size());
        int row = 0;
        for (const auto &asset: assets) {
            uint64_t assetIndex = asset["assetIndex"].asUInt64();
            loadAssetInfo(assetIndex);
            QString name = _names.count(assetIndex) ? _names[assetIndex] : "";
            _assetTable->setItem(row, 0, new QTableWidgetItem(name));
            _assetTable->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(asset["assetId"].asString())));
            _assetTable->setItem(row, 2, new QTableWidgetItem(QString::number(assetIndex)));
            _assetTable->setItem(row, 3, new QTableWidgetItem(QString::fromStdString(asset["amount"].asString())));
            _assetTable->setItem(row, 4, new QTableWidgetItem(QString::number(asset["decimals"].asUInt())));
            applyIcon(assetIndex);
            row++;
        }
        _statusLabel->setText(assets.empty() ? "No assets in wallet." : "");
    } catch (const DigiByteException &e) {
        _statusLabel->setText("Error fetching balances: " + QString::fromStdString(e.getMessage()));
    }
}
