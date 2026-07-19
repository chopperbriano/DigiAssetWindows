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
    _decimalsSpin->setRange(0, 7);
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

    //permanent storage pools.  The metadata must be stored somewhere permanent or the
    //asset won't be recognised by most of the ecosystem, so at least one is required
    QVBoxLayout *pspLayout = new QVBoxLayout();
    for (unsigned int i = 0;; i++) {
        Json::Value args = Json::arrayValue;
        args.append(i);
        Json::Value pool;
        try {
            pool = _dgbCore.sendcommand("getpsp", args);
        } catch (const DigiByteException &) {
            break; //no more pools
        }
        QCheckBox *check = new QCheckBox(QString::fromStdString(pool["name"].asString()));
        check->setToolTip(QString::fromStdString(pool["description"].asString()));
        check->setChecked(i == 1); //public pool on by default
        _pspChecks.push_back(check);
        _pspIndexes.push_back(i);
        pspLayout->addWidget(check);
    }
    form->addRow("Store Metadata In:", pspLayout);

    _descriptionEdit = new QTextEdit();
    _descriptionEdit->setPlaceholderText("Optional description stored in the asset's metadata");
    _descriptionEdit->setMaximumHeight(80);
    form->addRow("Description:", _descriptionEdit);

    //royalty rule: every transfer of the asset must pay this address.  More rule types
    //exist on the RPC side(rules param of issueasset) - the GUI covers the common one
    _royaltyCheck = new QCheckBox("Every transfer must pay a royalty");
    _royaltyAddressEdit = new QLineEdit();
    _royaltyAddressEdit->setPlaceholderText("Address royalties are paid to");
    _royaltyAddressEdit->setEnabled(false);
    _royaltyAmountEdit = new QLineEdit();
    _royaltyAmountEdit->setPlaceholderText("DGB per transfer(min 0.0001)");
    _royaltyAmountEdit->setEnabled(false);
    connect(_royaltyCheck, &QCheckBox::toggled, this, [this](bool on) {
        _royaltyAddressEdit->setEnabled(on);
        _royaltyAmountEdit->setEnabled(on);
    });
    QVBoxLayout *royaltyLayout = new QVBoxLayout();
    royaltyLayout->addWidget(_royaltyCheck);
    royaltyLayout->addWidget(_royaltyAddressEdit);
    royaltyLayout->addWidget(_royaltyAmountEdit);
    form->addRow("Royalty:", royaltyLayout);

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

    //at least one storage pool is required - without permanent storage the metadata can be
    //lost and the asset won't be recognised by most of the ecosystem
    Json::Value pspArray = Json::arrayValue;
    for (size_t i = 0; i < _pspChecks.size(); i++) {
        if (_pspChecks[i]->isChecked()) pspArray.append(_pspIndexes[i]);
    }
    if (pspArray.empty()) {
        _statusLabel->setText("Select at least one storage pool under \"Store Metadata In\".");
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
        config["psp"] = pspArray;

        //royalty rule
        if (_royaltyCheck->isChecked()) {
            QString royaltyAddress = _royaltyAddressEdit->text().trimmed();
            bool amountOk = false;
            double royaltyDgb = _royaltyAmountEdit->text().trimmed().toDouble(&amountOk);
            if (royaltyAddress.isEmpty() || !amountOk || (royaltyDgb < 0.0001)) {
                _statusLabel->setText("Royalty needs an address and a DGB amount of at least 0.0001.");
                return;
            }
            Json::Value addresses = Json::objectValue;
            addresses[royaltyAddress.toStdString()] =
                    static_cast<Json::UInt64>(royaltyDgb * 100000000.0 + 0.5); //DGB -> sats
            Json::Value rules = Json::objectValue;
            rules["royalty"] = Json::objectValue;
            rules["royalty"]["addresses"] = addresses;
            config["rules"] = rules;
        }

        //price the issuance first so the user confirms real numbers
        QString costText = "Costs could not be estimated.";
        try {
            Json::Value dryConfig = config;
            dryConfig["dryrun"] = true;
            Json::Value dryArgs = Json::arrayValue;
            dryArgs.append(dryConfig);
            Json::Value costs = _dgbCore.sendcommand("issueasset", dryArgs);
            costText = QString("Storage pool fee: %1 DGB\nEstimated total cost: %2 DGB")
                               .arg(QString::fromStdString(costs["pspFee"].asString()),
                                    QString::fromStdString(costs["estimatedTotal"].asString()));
        } catch (const DigiByteException&) {} //fall back to generic text

        //confirm - issuance can not be undone
        QString lockedText = _lockedCheck->isChecked() ? "locked(supply can never change)" : "unlocked";
        if (QMessageBox::question(this, "Confirm Create Asset",
                                  QString("Create %1 of \"%2\" (%3)?\n%4\nThis writes to the blockchain and can not be undone.")
                                          .arg(amount, name, lockedText, costText)) != QMessageBox::Yes) {
            return;
        }

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
