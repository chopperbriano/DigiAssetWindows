#include "BalancesTab.h"
#include <QHBoxLayout>
#include <QHeaderView>
#include <QVBoxLayout>

BalancesTab::BalancesTab(QWidget *parent) : QWidget(parent), _dgbCore() {
    _dgbCore.setFileName("config.cfg", true);
    _dgbCore.makeConnection();

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
    _assetTable = new QTableWidget(0, 4, this);
    _assetTable->setHorizontalHeaderLabels({"Asset ID", "Index", "Amount", "Decimals"});
    _assetTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    _assetTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _assetTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(_assetTable);

    _statusLabel = new QLabel("");
    layout->addWidget(_statusLabel);

    _timer = new QTimer(this);
    connect(_timer, &QTimer::timeout, this, &BalancesTab::updateBalances);
    _timer->start(30000); //update every 30 seconds

    updateBalances();
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
            _assetTable->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(asset["assetId"].asString())));
            _assetTable->setItem(row, 1, new QTableWidgetItem(QString::number(asset["assetIndex"].asUInt64())));
            _assetTable->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(asset["amount"].asString())));
            _assetTable->setItem(row, 3, new QTableWidgetItem(QString::number(asset["decimals"].asUInt())));
            row++;
        }
        _statusLabel->setText(assets.empty() ? "No assets in wallet." : "");
    } catch (const DigiByteException &e) {
        _statusLabel->setText("Error fetching balances: " + QString::fromStdString(e.getMessage()));
    }
}
