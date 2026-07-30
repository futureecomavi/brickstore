// Copyright (C) 2004-2026 Robert Griebl
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QDialog>
#include <QSet>

#include <QCoro/QCoroTask>

// the BrickLink order importer has exactly the same layout, so we re-use its .ui file
#include "ui_importorderdialog.h"


class ImportBrickZapOrderDialog : public QDialog, private Ui::ImportOrderDialog
{
    Q_OBJECT
public:
    ImportBrickZapOrderDialog(QWidget *parent = nullptr);
    ~ImportBrickZapOrderDialog() override;

    QCoro::Task<> updateOrders();

protected:
    void changeEvent(QEvent *e) override;
    void showEvent(QShowEvent *) override;
    void closeEvent(QCloseEvent *) override;
    void keyPressEvent(QKeyEvent *e) override;
    void languageChange();

protected slots:
    void checkSelected();
    void updateStatusLabel();

    void importOrders(const QModelIndexList &rows, bool combined);

private:
    QPushButton *w_import;
    QPushButton *w_importCombined;
    QSet<QString> m_selectedCurrencyCodes;
    QString m_updateMessage;
};
