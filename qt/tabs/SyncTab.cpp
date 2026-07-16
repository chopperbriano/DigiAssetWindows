#include "SyncTab.h"
#include <QVBoxLayout>
#include <QScrollArea>
#include <QLabel>
#include <QFrame>
#include "../../src/DigiAssetConstants.h"

SyncTab::SyncTab(QWidget *parent) : QWidget(parent), _dgbCore() {
    _dgbCore.setFileName("config.cfg", true);
    _dgbCore.makeConnection();

    QVBoxLayout *layout = new QVBoxLayout(this);
    _syncLabel = new QLabel("Sync Height: Loading...");
    layout->addWidget(_syncLabel);

    // Scrollable widget for exchange rates
    _exchangeRatesScroll = new QScrollArea(this);
    _exchangeRatesContainer = new QWidget();
    _exchangeRatesLayout = new QGridLayout(_exchangeRatesContainer); // Use QGridLayout for 2 columns

    _exchangeRatesContainer->setLayout(_exchangeRatesLayout);
    _exchangeRatesScroll->setWidget(_exchangeRatesContainer);
    _exchangeRatesScroll->setWidgetResizable(true);

    layout->addWidget(_exchangeRatesScroll);

    _timer = new QTimer(this);
    connect(_timer, &QTimer::timeout, this, &SyncTab::updateSyncStatus);
    _timer->start(15000);  // Update every 15 seconds

    updateSyncStatus();
}

void SyncTab::updateSyncStatus() {
    try {
        Json::Value args = Json::arrayValue;
        Json::Value result = _dgbCore.sendcommand("syncstate", args);
        int syncHeight = result["count"].asInt();
        _syncLabel->setText("Sync Height: " + QString::number(syncHeight));

        // Clear old entries
        QLayoutItem *child;
        while ((child = _exchangeRatesLayout->takeAt(0)) != nullptr) {
            delete child->widget();
            delete child;
        }

        Json::Value exchangeRates = _dgbCore.sendcommand("getexchangerates", args);
        int row = 0, col = 0;
        for (const auto &rate : exchangeRates) {
            std::string name;
            for (size_t i = 0; i < DigiAssetConstants::standardExchangeRatesCount; i++) {
                if (DigiAssetConstants::standardExchangeRates[i].index != rate["index"].asInt()) continue;
                if (DigiAssetConstants::standardExchangeRates[i].address != rate["address"].asString()) continue;
                name = DigiAssetConstants::standardExchangeRates[i].name;
                break;
            }

            double dgbValue = rate["value"].asDouble() / 100000000;
            double reverseValue = 100000000 / rate["value"].asDouble();
            int age = (syncHeight - rate["height"].asInt()) * 15;

            QString readableAge;
            if (age > 86400) {
                readableAge = QString("Age: %1 days").arg(QString::number(age/86400.0,'f',1));
            } else if (age > 3600) {
                readableAge = QString("Age: %1 hrs").arg(QString::number(age/3600.0,'f',1));
            } else {
                readableAge = QString("Age: %1 min").arg(QString::number(age/60.0,'f',1));
            }

            // Create a frame for each exchange rate
            QFrame *rateFrame = new QFrame();
            rateFrame->setFrameShape(QFrame::Box);
            rateFrame->setLineWidth(1);

            QVBoxLayout *frameLayout = new QVBoxLayout(rateFrame);

            QLabel *topLabel = new QLabel(QString("%1 DGB/%2").arg(QString::number(dgbValue, 'f', 8)).arg(QString::fromStdString(name)));
            topLabel->setAlignment(Qt::AlignRight);
            QLabel *bottomLabel = new QLabel(QString("%1 %2/DGB").arg(QString::number(reverseValue, 'f', 8)).arg(QString::fromStdString(name)));
            bottomLabel->setAlignment(Qt::AlignRight);
            QLabel *ageLabel = new QLabel(readableAge);
            ageLabel->setAlignment(Qt::AlignRight);

            frameLayout->addWidget(topLabel);
            frameLayout->addWidget(bottomLabel);
            frameLayout->addWidget(ageLabel);

            _exchangeRatesLayout->addWidget(rateFrame, row, col);
            col = (col + 1) % 2;  // Alternate between columns
            if (col == 0) row++;  // Move to next row after 2 columns
        }
    } catch (const DigiByteException &e) {
        _syncLabel->setText("Error fetching sync state.");
    }
}
