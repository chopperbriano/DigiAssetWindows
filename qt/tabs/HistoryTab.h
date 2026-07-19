//
// Created by DigiAsset Core on 19/07/26.
//

#ifndef HISTORYTAB_H
#define HISTORYTAB_H

#include "DigiByteCore.h"
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QWidget>

/**
 * Shows the wallet's recent transactions(DigiByte level, newest first) with a text
 * filter and Older/Newer paging.  Data comes from the wallet's listtransactions call
 * (forwarded by the daemon).
 */
class HistoryTab : public QWidget {
    Q_OBJECT

public:
    explicit HistoryTab(QWidget *parent = nullptr);

private slots:
    void updateHistory();
    void applyFilter();
    void olderPage();
    void newerPage();

private:
    QLabel * _statusLabel;
    QLabel * _pageLabel;
    QLineEdit * _filterEdit;
    QPushButton * _refreshButton;
    QPushButton * _olderButton;
    QPushButton * _newerButton;
    QTableWidget * _txTable;
    QTimer * _timer;
    unsigned int _page = 0;      //0 = most recent transactions
    unsigned int _lastCount = 0; //rows the last update fetched(< PAGE_SIZE means last page)
    DigiByteCore _dgbCore;

    static const unsigned int PAGE_SIZE = 100;
};

#endif // HISTORYTAB_H
