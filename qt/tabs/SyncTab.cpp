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

    //DigiDollar system state.  Hidden entirely until the chain reaches activation or if the
    //daemon is running with trackdigidollar=0, so nodes that do not care never see it.
    _digidollarLabel = new QLabel();
    _digidollarLabel->setWordWrap(true);
    _digidollarLabel->setVisible(false);
    layout->addWidget(_digidollarLabel);

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

        updateDigiDollarStatus();

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
            std::string source;
            if (rate["address"].asString() == DigiAssetConstants::DIGIDOLLAR_RATE_ADDRESS) {
                //The DigiDollar oracle rate is not published by an address, so it will never match
                //the standardExchangeRates table.  Without this it would render with a blank
                //currency name.
                name = "USD";
                source = "DigiDollar oracle";
            } else {
                for (size_t i = 0; i < DigiAssetConstants::standardExchangeRatesCount; i++) {
                    if (DigiAssetConstants::standardExchangeRates[i].index != rate["index"].asInt()) continue;
                    if (DigiAssetConstants::standardExchangeRates[i].address != rate["address"].asString()) continue;
                    name = DigiAssetConstants::standardExchangeRates[i].name;
                    break;
                }
                //an address we do not have a name for is still worth showing, labelled by index
                if (name.empty()) name = "index " + std::to_string(rate["index"].asInt());
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

            //say where a rate came from when it is not one of the standard published addresses
            if (!source.empty()) {
                QLabel *sourceLabel = new QLabel(QString::fromStdString(source));
                sourceLabel->setAlignment(Qt::AlignRight);
                frameLayout->addWidget(sourceLabel);
            }

            _exchangeRatesLayout->addWidget(rateFrame, row, col);
            col = (col + 1) % 2;  // Alternate between columns
            if (col == 0) row++;  // Move to next row after 2 columns
        }
    } catch (const DigiByteException &e) {
        _syncLabel->setText("Error fetching sync state.");
    }
}

/**
 * Shows the DigiDollar oracle price, circulating supply and collateralization.
 *
 * Everything here is best effort - a daemon running with trackdigidollar=0 does not serve
 * getdigidollarinfo, and a chain below the activation height has nothing to report.  Either way
 * the panel simply stays hidden rather than showing an error, because neither case is a fault.
 */
void SyncTab::updateDigiDollarStatus() {
    try {
        Json::Value args = Json::arrayValue;
        Json::Value info = _dgbCore.sendcommand("getdigidollarinfo", args);

        if (!info["active"].asBool()) {
            _digidollarLabel->setVisible(false);
            return;
        }

        QString text = "<b>DigiDollar</b>";

        if (info.isMember("price")) {
            //price is micro USD per DGB, so 4216 means $0.004216
            double usdPerDgb = info["price"]["microUSDPerDGB"].asDouble() / 1e6;
            text += QString(" &nbsp; Oracle: $%1/DGB").arg(QString::number(usdPerDgb, 'f', 8));
            text += QString(" (%1 of 35 oracles)").arg(info["price"]["oracles"].asInt());
        } else {
            text += " &nbsp; Oracle: no price recorded yet";
        }

        text += QString(" &nbsp; Supply: $%1").arg(QString::fromStdString(info["supply"]["amount"].asString()));
        text += QString(" across %1 addresses").arg(info["supply"]["holders"].asUInt());

        text += QString(" &nbsp; Collateral: %1 DGB in %2 vaults")
                        .arg(QString::fromStdString(info["collateral"]["amount"].asString()))
                        .arg(info["collateral"]["vaults"].asUInt());
        if (info["collateral"].isMember("ratio")) {
            text += QString(" (%1%)").arg(QString::number(info["collateral"]["ratio"].asDouble() * 100, 'f', 1));
        }

        //a node still backfilling reports partial figures - say so rather than letting them read
        //as the real state of the system
        if (!info["indexed"].asBool()) {
            text += "<br/><i>DigiDollar history is still being indexed - these figures are incomplete.</i>";
        }

        _digidollarLabel->setText(text);
        _digidollarLabel->setVisible(true);
    } catch (...) {
        //Most likely trackdigidollar=0 or an older daemon, which forwards the unknown method on to
        //DigiByte Core and gets a method-not-found back.  Swallow everything rather than only
        //DigiByteException: this panel is supplementary, and letting it fail would make the whole
        //sync tab report an error it did not actually have.
        _digidollarLabel->setVisible(false);
    }
}
