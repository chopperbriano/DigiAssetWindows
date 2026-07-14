#include "CreateAssetTab.h"
#include <QFormLayout>
#include <QMessageBox>
#include <QVBoxLayout>

CreateAssetTab::CreateAssetTab(QWidget *parent) : QWidget(parent), _dgbCore() {
    _dgbCore.setFileName("config.cfg", true);
    _dgbCore.makeConnection();

    QVBoxLayout *layout = new QVBoxLayout(this);
    QFormLayout *form = new QFormLayout();

    _nameEdit = new QLineEdit();
    _nameEdit->setPlaceholderText("My Asset");
    form->addRow("Name:", _nameEdit);

    _amountEdit = new QLineEdit();
    _amountEdit->setPlaceholderText("Number of assets to create(e.g. 1000)");
    form->addRow("Amount:", _amountEdit);

    _decimalsSpin = new QSpinBox();
    _decimalsSpin->setRange(0, 8);
    _decimalsSpin->setValue(0);
    form->addRow("Decimals:", _decimalsSpin);

    _lockedCheck = new QCheckBox("No more can ever be issued");
    _lockedCheck->setChecked(true);
    form->addRow("Locked:", _lockedCheck);

    _aggregationCombo = new QComboBox();
    _aggregationCombo->addItem("aggregable");
    _aggregationCombo->addItem("hybrid");
    _aggregationCombo->addItem("dispersed");
    form->addRow("Aggregation:", _aggregationCombo);

    _descriptionEdit = new QTextEdit();
    _descriptionEdit->setPlaceholderText("Optional description stored in the asset's metadata");
    _descriptionEdit->setMaximumHeight(80);
    form->addRow("Description:", _descriptionEdit);

    _toAddressEdit = new QLineEdit();
    _toAddressEdit->setPlaceholderText("Optional - defaults to a new wallet address");
    form->addRow("Send To:", _toAddressEdit);

    layout->addLayout(form);

    _createButton = new QPushButton("Create Asset");
    connect(_createButton, &QPushButton::clicked, this, &CreateAssetTab::createAsset);
    layout->addWidget(_createButton);

    _statusLabel = new QLabel("");
    _statusLabel->setWordWrap(true);
    _statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(_statusLabel);
    layout->addStretch();
}

void CreateAssetTab::createAsset() {
    QString name = _nameEdit->text().trimmed();
    QString amount = _amountEdit->text().trimmed();
    if (name.isEmpty() || amount.isEmpty()) {
        _statusLabel->setText("Please fill in name and amount.");
        return;
    }

    //confirm - issuance can not be undone
    QString lockedText = _lockedCheck->isChecked() ? "locked(supply can never change)" : "unlocked";
    if (QMessageBox::question(this, "Confirm Create Asset",
                              QString("Create %1 of \"%2\" (%3)?\nThis writes to the blockchain and can not be undone.")
                                      .arg(amount, name, lockedText)) != QMessageBox::Yes) {
        return;
    }

    try {
        Json::Value config = Json::objectValue;
        config["name"] = name.toStdString();
        config["amount"] = amount.toStdString();
        config["decimals"] = _decimalsSpin->value();
        config["locked"] = _lockedCheck->isChecked();
        config["aggregation"] = _aggregationCombo->currentText().toStdString();
        QString description = _descriptionEdit->toPlainText().trimmed();
        if (!description.isEmpty()) config["description"] = description.toStdString();
        QString toAddress = _toAddressEdit->text().trimmed();
        if (!toAddress.isEmpty()) config["toAddress"] = toAddress.toStdString();

        Json::Value args = Json::arrayValue;
        args.append(config);
        Json::Value result = _dgbCore.sendcommand("issueasset", args);
        _statusLabel->setText("Asset created!\ntxid: " + QString::fromStdString(result["txid"].asString()) +
                              "\nassetId: " + QString::fromStdString(result["assetId"].asString()) +
                              "\nmetadata cid: " + QString::fromStdString(result["cid"].asString()));
    } catch (const DigiByteException &e) {
        _statusLabel->setText("Create failed: " + QString::fromStdString(e.getMessage()));
    }
}
