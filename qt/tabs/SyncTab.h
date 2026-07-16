//
// Created by mctrivia on 27/12/24.
//

#ifndef SYNCTAB_H
#define SYNCTAB_H

#include "DigiByteCore.h"
#include <QLabel>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

class SyncTab : public QWidget {
    Q_OBJECT

public:
    explicit SyncTab(QWidget *parent = nullptr);

private slots:
    void updateSyncStatus();

private:
    QLabel * _syncLabel;
    QLabel * _exchangeRatesLabel;
    QTimer * _timer;
    DigiByteCore _dgbCore;

    QScrollArea * _exchangeRatesScroll;
    QWidget * _exchangeRatesContainer;
    QGridLayout * _exchangeRatesLayout;

};

#endif // SYNCTAB_H
