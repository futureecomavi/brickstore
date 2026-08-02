// Copyright (C) 2004-2026 Robert Griebl
// SPDX-License-Identifier: GPL-3.0-only

#include <QHeaderView>
#include <QKeyEvent>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QTimer>
#include <QVariant>

#include "brickzap/core.h"
#include "brickzap/orders.h"
#include "common/actionmanager.h"
#include "common/application.h"
#include "common/config.h"
#include "common/currency.h"
#include "common/document.h"
#include "common/documentio.h"
#include "common/humanreadabletimedelta.h"
#include "betteritemdelegate.h"
#include "historylineedit.h"
#include "importbrickzaporderdialog.h"

using namespace std::chrono_literals;


ImportBrickZapOrderDialog::ImportBrickZapOrderDialog(QWidget *parent)
    : QDialog(parent)
{
    setupUi(this);

    w_update->setProperty("iconScaling", true);

    // the status may quote an error message from the server: BrickZap::Orders escapes those,
    // so they have to be rendered as rich text to show up as the plain text they are
    w_lastUpdated->setTextFormat(Qt::RichText);

    w_orders->header()->setStretchLastSection(false);
    auto proxyModel = new QSortFilterProxyModel(this);
    proxyModel->setSortCaseSensitivity(Qt::CaseInsensitive);
    proxyModel->setSortLocaleAware(true);
    proxyModel->setSortRole(BrickZap::Orders::OrderSortRole);
    proxyModel->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxyModel->setFilterKeyColumn(-1);
    proxyModel->setSourceModel(BrickZap::core()->orders());
    w_orders->setModel(proxyModel);
    w_orders->header()->setSectionResizeMode(BrickZap::Orders::Buyer, QHeaderView::Stretch);
    w_orders->setItemDelegate(new BetterItemDelegate(BetterItemDelegate::AlwaysShowSelection
                                                    | BetterItemDelegate::MoreSpacing, w_orders));

    connect(w_filter, &HistoryLineEdit::textChanged,
            proxyModel, &QSortFilterProxyModel::setFilterFixedString);
    connect(new QShortcut(QKeySequence::Find, this), &QShortcut::activated,
            this, [this]() { w_filter->setFocus(); });

    w_importCombined = new QPushButton();
    w_importCombined->setDefault(false);
    w_buttons->addButton(w_importCombined, QDialogButtonBox::ActionRole);
    connect(w_importCombined, &QAbstractButton::clicked,
            this, [this]() { importOrders(w_orders->selectionModel()->selectedRows(), true); });
    w_import = new QPushButton();
    w_import->setDefault(true);
    w_buttons->addButton(w_import, QDialogButtonBox::ActionRole);
    connect(w_import, &QAbstractButton::clicked,
            this, [this]() { importOrders(w_orders->selectionModel()->selectedRows(), false); });

    connect(w_update, &QToolButton::clicked,
            this, &ImportBrickZapOrderDialog::updateOrders);
    connect(w_orders->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &ImportBrickZapOrderDialog::checkSelected);
    connect(w_orders, &QTreeView::activated,
            this, [this]() { w_import->animateClick(); });
    connect(BrickZap::core(), &BrickZap::Core::authenticatedTransferOverallProgress,
            this, [this](int done, int total) {
        w_progress->setVisible(done != total);
        w_progress->setMaximum(total);
        w_progress->setValue(done);
    });
    connect(BrickZap::core()->orders(), &BrickZap::Orders::updateFinished,
            this, [this](bool success, const QString &message) {
        Q_UNUSED(success)

        w_update->setEnabled(true);
        w_orders->setEnabled(true);

        m_updateMessage = message;
        updateStatusLabel();

        if (BrickZap::core()->orders()->rowCount()) {
            auto tl = w_orders->model()->index(0, 0);
            w_orders->selectionModel()->select(tl, QItemSelectionModel::SelectCurrent
                                               | QItemSelectionModel::Rows);
            w_orders->scrollTo(tl);
        }
        w_orders->header()->resizeSections(QHeaderView::ResizeToContents);
        w_orders->setFocus();

        checkSelected();
    });

    languageChange();

    auto t = new QTimer(this);
    t->setInterval(30s);
    connect(t, &QTimer::timeout, this,
            &ImportBrickZapOrderDialog::updateStatusLabel);
    t->start();

    if (!BrickZap::core()->orders()->rowCount()) {
        QMetaObject::invokeMethod(this, &ImportBrickZapOrderDialog::updateOrders,
                                  Qt::QueuedConnection);
    }

    int daysBack = Config::inst()->value(u"MainWindow/ImportBrickZapOrderDialog/DaysBack"_qs, -1).toInt();
    if (daysBack > 0)
        w_daysBack->setValue(daysBack);
    auto ba = Config::inst()->value(u"MainWindow/ImportBrickZapOrderDialog/Filter"_qs).toByteArray();
    if (!ba.isEmpty())
        w_filter->restoreState(ba);
    ba = Config::inst()->value(u"MainWindow/ImportBrickZapOrderDialog/ListState"_qs).toByteArray();
    if (!ba.isEmpty())
        w_orders->header()->restoreState(ba);

    setFocusProxy(w_filter);
}

ImportBrickZapOrderDialog::~ImportBrickZapOrderDialog()
{
    Config::inst()->setValue(u"MainWindow/ImportBrickZapOrderDialog/DaysBack"_qs, w_daysBack->value());
    Config::inst()->setValue(u"MainWindow/ImportBrickZapOrderDialog/Filter"_qs, w_filter->saveState());
    Config::inst()->setValue(u"MainWindow/ImportBrickZapOrderDialog/ListState"_qs,
                             w_orders->header()->saveState());
}

void ImportBrickZapOrderDialog::keyPressEvent(QKeyEvent *e)
{
    // simulate QDialog behavior
    if (e->matches(QKeySequence::Cancel)) {
        close();
        return;
    } else if ((!e->modifiers() && (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter))
               || ((e->modifiers() & Qt::KeypadModifier) && (e->key() == Qt::Key_Enter))) {
        // we need the animateClick here instead of triggering directly: otherwise we
        // get interference from the QTreeView::activated signal, resulting in double triggers
        if (w_import->isVisible() && w_import->isEnabled())
            w_import->animateClick();
        return;
    }

    QWidget::keyPressEvent(e);
}

void ImportBrickZapOrderDialog::showEvent(QShowEvent *e)
{
    QByteArray ba = Config::inst()->value(u"MainWindow/ImportBrickZapOrderDialog/Geometry"_qs).toByteArray();
    if (!ba.isEmpty())
        restoreGeometry(ba);

    QDialog::showEvent(e);
    activateWindow();
}

void ImportBrickZapOrderDialog::closeEvent(QCloseEvent *e)
{
    Config::inst()->setValue(u"MainWindow/ImportBrickZapOrderDialog/Geometry"_qs, saveGeometry());
    QDialog::closeEvent(e);
}

void ImportBrickZapOrderDialog::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::LanguageChange)
        languageChange();
    QDialog::changeEvent(e);
}

void ImportBrickZapOrderDialog::languageChange()
{
    retranslateUi(this);

    setWindowTitle(tr("Import BrickZap Order"));
    w_import->setText(tr("Import"));
    w_importCombined->setText(tr("Import combined"));
    w_filter->setToolTip(ActionManager::toolTipLabel(tr("Filter the list for lines containing these words"),
                                                     QKeySequence::Find, w_filter->instructionToolTip()));
    updateStatusLabel();
}

QCoro::Task<> ImportBrickZapOrderDialog::updateOrders()
{
    if (BrickZap::core()->orders()->updateStatus() == BrickLink::UpdateStatus::Updating)
        co_return;

    if (!co_await Application::inst()->checkBrickZapLogin())
        co_return;

    m_updateMessage.clear();
    w_update->setEnabled(false);
    w_import->setEnabled(false);
    w_orders->setEnabled(false);

    BrickZap::core()->orders()->startUpdate(w_daysBack->value());
    updateStatusLabel();
}

void ImportBrickZapOrderDialog::importOrders(const QModelIndexList &rows, bool combined)
{
    bool combineCCode = false;
    if (combined && (m_selectedCurrencyCodes.size() > 1)) {
        if (QMessageBox::question(this, tr("Import Order"),
                                  tr("You have selected multiple orders with differing currencies, which cannot be combined as-is.")
                                  + u"<br><br>"
                                  + tr("Do you want to continue and convert all prices to your default currency (%1)?")
                                        .arg(Config::inst()->defaultCurrencyCode())) == QMessageBox::No) {
            return;
        }
        combineCCode = true;
    }

    const QString defaultCCode = Config::inst()->defaultCurrencyCode();

    BrickLink::IO::ParseResult combinedPr;
    int orderCount = 0;

    for (const auto &idx : rows) {
        const auto *order = idx.data(BrickZap::Orders::OrderPointerRole)
                                .value<const BrickZap::Order *>();
        if (!order)
            continue;

        if (combined) {
            double crate = 0;

            if (combineCCode && (order->currencyCode() != defaultCCode))
                crate = Currency::inst()->crossRate(order->currencyCode(), defaultCCode);

            LotList orderLots = order->loadLots(); // we own the Lots now
            if (!orderLots.isEmpty()) {
                QColor col = QColor::fromHsl(360 * orderCount / int(rows.size()), 128, 128);
                for (auto *orderLot : std::as_const(orderLots)) {
                    orderLot->setMarkerText(order->number() + u' ' + order->buyerName());
                    orderLot->setMarkerColor(col);

                    if (!qFuzzyIsNull(crate)) {
                        orderLot->setCost(orderLot->cost() * crate);
                        orderLot->setPrice(orderLot->price() * crate);
                        for (int i = 0; i < 3; ++i)
                            orderLot->setTierPrice(i, orderLot->tierPrice(i) * crate);
                    }
                    combinedPr.addLot(std::move(orderLot));
                }
                combinedPr.setCurrencyCode(combineCCode ? defaultCCode : order->currencyCode());
            }
            orderLots.clear();
        } else {
            DocumentIO::importBrickZapOrder(order);
        }
        ++orderCount;
    }
    if (combined) {
        auto *doc = Document::create(new DocumentModel(std::move(combinedPr)));
        doc->setTitle(tr("Multiple BrickZap Orders"));
        doc->setThumbnail(u"brickzap"_qs);
    }
}

void ImportBrickZapOrderDialog::checkSelected()
{
    const auto rows = w_orders->selectionModel()->selectedRows();
    m_selectedCurrencyCodes.clear();

    for (const auto &idx : rows) {
        if (const auto *order = idx.data(BrickZap::Orders::OrderPointerRole)
                                    .value<const BrickZap::Order *>()) {
            m_selectedCurrencyCodes.insert(order->currencyCode());
        }
    }

    w_import->setEnabled(!rows.isEmpty());
    w_importCombined->setEnabled(rows.size() > 1);
}

void ImportBrickZapOrderDialog::updateStatusLabel()
{
    QString s;

    switch (BrickZap::core()->orders()->updateStatus()) {
    case BrickLink::UpdateStatus::Ok:
        s = tr("Last updated %1").arg(
                HumanReadableTimeDelta::toString(QDateTime::currentDateTime(),
                                                 BrickZap::core()->orders()->lastUpdated()));
        break;

    case BrickLink::UpdateStatus::Updating:
        s = tr("Currently updating orders");
        break;

    case BrickLink::UpdateStatus::UpdateFailed:
        s = tr("Last update failed") + u": " + m_updateMessage;
        break;

    default:
        break;
    }
    w_lastUpdated->setText(s);
}

#include "moc_importbrickzaporderdialog.cpp"
