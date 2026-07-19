//
// Created by DigiAsset Core on 14/07/26.
//

#ifndef CREATEASSETTAB_H
#define CREATEASSETTAB_H

#include "DigiByteCore.h"
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTextEdit>
#include <QWidget>
#include <vector>

/**
 * Form for creating(issuing) a new DigiAsset.  Uses the issueasset RPC method.
 */
class CreateAssetTab : public QWidget {
    Q_OBJECT

public:
    explicit CreateAssetTab(QWidget *parent = nullptr);

private slots:
    void createAsset();

private:
    QLineEdit * _nameEdit;
    QLineEdit * _amountEdit;
    QSpinBox * _decimalsSpin;
    QCheckBox * _lockedCheck;
    QComboBox * _aggregationCombo;
    QTextEdit * _descriptionEdit;
    QLineEdit * _toAddressEdit;
    QPushButton * _createButton;
    QLabel * _statusLabel;
    std::vector<QCheckBox*> _pspChecks;         //one per storage pool, same order as _pspIndexes
    std::vector<unsigned int> _pspIndexes;      //pool indexes for the psp param of issueasset
    DigiByteCore _dgbCore;
};

#endif // CREATEASSETTAB_H
