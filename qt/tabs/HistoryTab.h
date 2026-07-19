//
// Created by DigiAsset Core on 19/07/26.
//

#ifndef HISTORYTAB_H
#define HISTORYTAB_H

#include "DigiByteCore.h"
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QWidget>

/**
 * Shows the wallet's recent transactions(DigiByte level, newest first).
 * Data comes from the wallet's listtransactions call(forwarded by the daemon).
 */
class HistoryTab : public QWidget {
    Q_OBJECT

public:
    explicit HistoryTab(QWidget *parent = nullptr);

private slots:
    void updateHistory();

private:
    QLabel * _statusLabel;
    QPushButton * _refreshButton;
    QTableWidget * _txTable;
    QTimer * _timer;
    DigiByteCore _dgbCore;
};

#endif // HISTORYTAB_H
