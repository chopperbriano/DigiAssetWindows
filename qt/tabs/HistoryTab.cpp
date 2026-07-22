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
    _filterEdit = new QLineEdit();
    _filterEdit->setPlaceholderText("Filter(txid, type, address, amount...)");
    _filterEdit->setClearButtonEnabled(true);
    connect(_filterEdit, &QLineEdit::textChanged, this, &HistoryTab::applyFilter);
    topRow->addWidget(_filterEdit, 1);
    _refreshButton = new QPushButton("Refresh");
    connect(_refreshButton, &QPushButton::clicked, this, &HistoryTab::updateHistory);
    topRow->addWidget(_refreshButton);
    layout->addLayout(topRow);

    _txTable = new QTableWidget(0, 6, this);
    _txTable->setHorizontalHeaderLabels({"Time", "Type", "Amount (DGB)", "Confirmations", "Address", "Transaction ID"});
    _txTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    _txTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _txTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(_txTable);

    QHBoxLayout *pageRow = new QHBoxLayout();
    _newerButton = new QPushButton("< Newer");
    connect(_newerButton, &QPushButton::clicked, this, &HistoryTab::newerPage);
    pageRow->addWidget(_newerButton);
    _pageLabel = new QLabel("");
    pageRow->addWidget(_pageLabel);
    _olderButton = new QPushButton("Older >");
    connect(_olderButton, &QPushButton::clicked, this, &HistoryTab::olderPage);
    pageRow->addWidget(_olderButton);
    pageRow->addStretch();
    layout->addLayout(pageRow);

    _statusLabel = new QLabel("Select a row and copy the transaction id to inspect it with getrawtransaction.");
    _statusLabel->setWordWrap(true);
    layout->addWidget(_statusLabel);

    _timer = new QTimer(this);
    connect(_timer, &QTimer::timeout, this, [this]() {
        if (_page == 0) updateHistory(); //don't yank the user off an older page
    });
    _timer->start(30000);

    updateHistory();
}

void HistoryTab::updateHistory() {
    try {
        Json::Value args = Json::arrayValue;
        args.append("*");
        args.append(PAGE_SIZE);
        args.append(_page * PAGE_SIZE); //skip
        Json::Value result = _dgbCore.sendcommand("listtransactions", args);
        _lastCount = result.size();

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
            _txTable->setItem(row, 4, new QTableWidgetItem(QString::fromStdString(tx["address"].asString())));
            _txTable->setItem(row, 5, new QTableWidgetItem(QString::fromStdString(tx["txid"].asString())));
            row++;
        }
        //a successful fetch clears any earlier error message
        if (result.empty() && (_page == 0)) {
            _statusLabel->setText("No wallet transactions yet.");
        } else {
            _statusLabel->setText("Select a row and copy the transaction id to inspect it with getrawtransaction.");
        }

        _pageLabel->setText(QString("page %1").arg(_page + 1));
        _newerButton->setEnabled(_page > 0);
        _olderButton->setEnabled(_lastCount == PAGE_SIZE); //a short page is the oldest one

        applyFilter();
    } catch (const DigiByteException &e) {
        _statusLabel->setText("Error fetching history: " + QString::fromStdString(e.getMessage()));
    }
}

///hides rows that don't contain the filter text in any column(case insensitive)
void HistoryTab::applyFilter() {
    QString needle = _filterEdit->text().trimmed();
    for (int row = 0; row < _txTable->rowCount(); row++) {
        bool match = needle.isEmpty();
        for (int col = 0; !match && (col < _txTable->columnCount()); col++) {
            QTableWidgetItem *item = _txTable->item(row, col);
            if ((item != nullptr) && item->text().contains(needle, Qt::CaseInsensitive)) match = true;
        }
        _txTable->setRowHidden(row, !match);
    }
}

void HistoryTab::olderPage() {
    if (_lastCount < PAGE_SIZE) return; //already on the oldest page
    _page++;
    updateHistory();
}

void HistoryTab::newerPage() {
    if (_page == 0) return;
    _page--;
    updateHistory();
}
