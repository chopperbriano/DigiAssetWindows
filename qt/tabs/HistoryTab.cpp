#include "HistoryTab.h"
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QVBoxLayout>

HistoryTab::HistoryTab(QWidget *parent) : QWidget(parent), _dgbCore() {
    _dgbCore.setFileName("config.cfg", true);
    _dgbCore.makeConnection();

    QVBoxLayout *layout = new QVBoxLayout(this);

    QHBoxLayout *topRow = new QHBoxLayout();
    topRow->addWidget(new QLabel("Recent wallet transactions"));
    topRow->addStretch();
    _refreshButton = new QPushButton("Refresh");
    connect(_refreshButton, &QPushButton::clicked, this, &HistoryTab::updateHistory);
    topRow->addWidget(_refreshButton);
    layout->addLayout(topRow);

    _txTable = new QTableWidget(0, 5, this);
    _txTable->setHorizontalHeaderLabels({"Time", "Type", "Amount (DGB)", "Confirmations", "Transaction ID"});
    _txTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    _txTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _txTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(_txTable);

    _statusLabel = new QLabel("Select a row and copy the transaction id to inspect it with getrawtransaction.");
    _statusLabel->setWordWrap(true);
    layout->addWidget(_statusLabel);

    _timer = new QTimer(this);
    connect(_timer, &QTimer::timeout, this, &HistoryTab::updateHistory);
    _timer->start(30000);

    updateHistory();
}

void HistoryTab::updateHistory() {
    try {
        Json::Value args = Json::arrayValue;
        args.append("*");
        args.append(100);
        Json::Value result = _dgbCore.sendcommand("listtransactions", args);

        //newest first
        _txTable->setRowCount(result.size());
        int row = 0;
        for (int i = static_cast<int>(result.size()) - 1; i >= 0; i--) {
            const Json::Value &tx = result[i];
            QString when = QDateTime::fromSecsSinceEpoch(tx["time"].asInt64()).toString("yyyy-MM-dd hh:mm");
            _txTable->setItem(row, 0, new QTableWidgetItem(when));
            _txTable->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(tx["category"].asString())));
            _txTable->setItem(row, 2, new QTableWidgetItem(QString::number(tx["amount"].asDouble(), 'f', 8)));
            _txTable->setItem(row, 3, new QTableWidgetItem(QString::number(tx["confirmations"].asInt())));
            _txTable->setItem(row, 4, new QTableWidgetItem(QString::fromStdString(tx["txid"].asString())));
            row++;
        }
        if (result.empty()) _statusLabel->setText("No wallet transactions yet.");
    } catch (const DigiByteException &e) {
        _statusLabel->setText("Error fetching history: " + QString::fromStdString(e.getMessage()));
    }
}
