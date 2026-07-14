#include "SendAssetTab.h"
#include <QFormLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QVBoxLayout>

SendAssetTab::SendAssetTab(QWidget *parent) : QWidget(parent), _dgbCore() {
    _dgbCore.setFileName("config.cfg", true);
    _dgbCore.makeConnection();

    QVBoxLayout *layout = new QVBoxLayout(this);
    QFormLayout *form = new QFormLayout();

    _addressEdit = new QLineEdit();
    _addressEdit->setPlaceholderText("dgb1... address or DigiByte domain");
    form->addRow("Pay To:", _addressEdit);

    QHBoxLayout *assetRow = new QHBoxLayout();
    _assetCombo = new QComboBox();
    assetRow->addWidget(_assetCombo, 1);
    _refreshButton = new QPushButton("Refresh");
    connect(_refreshButton, &QPushButton::clicked, this, &SendAssetTab::refreshAssets);
    assetRow->addWidget(_refreshButton);
    form->addRow("Asset:", assetRow);

    _amountEdit = new QLineEdit();
    _amountEdit->setPlaceholderText("Amount in display units(e.g. 1.5)");
    form->addRow("Amount:", _amountEdit);

    layout->addLayout(form);

    _sendButton = new QPushButton("Send Asset");
    connect(_sendButton, &QPushButton::clicked, this, &SendAssetTab::sendAsset);
    layout->addWidget(_sendButton);

    _statusLabel = new QLabel("");
    _statusLabel->setWordWrap(true);
    _statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(_statusLabel);
    layout->addStretch();

    refreshAssets();
}

///reloads the asset dropdown from the wallet's holdings
void SendAssetTab::refreshAssets() {
    try {
        Json::Value args = Json::arrayValue;
        Json::Value result = _dgbCore.sendcommand("getwalletbalances", args);
        _assetCombo->clear();
        for (const auto &asset: result["assets"]) {
            QString label = QString::fromStdString(asset["assetId"].asString()) +
                            " (index " + QString::number(asset["assetIndex"].asUInt64()) +
                            ", balance " + QString::fromStdString(asset["amount"].asString()) + ")";
            _assetCombo->addItem(label, QVariant::fromValue((qulonglong) asset["assetIndex"].asUInt64()));
        }
        if (_assetCombo->count() == 0) {
            _statusLabel->setText("No assets in wallet.");
        }
    } catch (const DigiByteException &e) {
        _statusLabel->setText("Error loading wallet assets: " + QString::fromStdString(e.getMessage()));
    }
}

void SendAssetTab::sendAsset() {
    QString address = _addressEdit->text().trimmed();
    QString amount = _amountEdit->text().trimmed();
    if (address.isEmpty() || amount.isEmpty() || (_assetCombo->currentIndex() < 0)) {
        _statusLabel->setText("Please fill in address, asset and amount.");
        return;
    }

    //confirm
    QString assetLabel = _assetCombo->currentText();
    if (QMessageBox::question(this, "Confirm Send",
                              QString("Send %1 of\n%2\nto %3?").arg(amount, assetLabel, address)) != QMessageBox::Yes) {
        return;
    }

    try {
        Json::Value args = Json::arrayValue;
        args.append(address.toStdString());
        args.append((Json::UInt64) _assetCombo->currentData().toULongLong());
        args.append(amount.toStdString());
        Json::Value txid = _dgbCore.sendcommand("sendasset", args);
        _statusLabel->setText("Sent!  txid: " + QString::fromStdString(txid.asString()));
        _amountEdit->clear();
        refreshAssets();
    } catch (const DigiByteException &e) {
        _statusLabel->setText("Send failed: " + QString::fromStdString(e.getMessage()));
    }
}
