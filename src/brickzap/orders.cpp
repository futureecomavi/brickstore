// Copyright (C) 2004-2026 Robert Griebl
// SPDX-License-Identifier: GPL-3.0-only

#include <algorithm>
#include <limits>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QUrlQuery>

#include "common/currency.h"
#include "utility/transfer.h"
#include "brickzap/core.h"
#include "brickzap/mapper.h"
#include "brickzap/orders.h"

using namespace BrickLink;

namespace {

constexpr int ordersPerPage = 100;
// safety net: the pagination has to end, even if the server keeps claiming there is more
constexpr int maxOrderPages = 1000;

QString humanizedStatus(const QString &status)
{
    QString text = status;
    text.replace(u'_', u' ');
    if (!text.isEmpty())
        text[0] = text.at(0).toUpper();
    return text;
}

} // namespace


namespace BrickZap {

Order::~Order()
{
    qDeleteAll(m_lots);
}

Order *Order::fromJson(const QJsonObject &json, int *unresolvedItems)
{
    auto *order = new Order();

    order->m_id = json.value(u"id"_qs).toString();
    order->m_number = json.value(u"number"_qs).toString();
    order->m_status = json.value(u"status"_qs).toString();
    order->m_date = QDateTime::fromString(json.value(u"created_at"_qs).toString(), Qt::ISODate);

    const auto customer = json.value(u"customer"_qs).toObject();
    order->m_buyerName = QString(customer.value(u"firstname"_qs).toString() + u' '
                                 + customer.value(u"lastname"_qs).toString()).trimmed();
    if (order->m_buyerName.isEmpty())
        order->m_buyerName = customer.value(u"email"_qs).toString();

    const auto summary = json.value(u"summary"_qs).toObject();
    order->m_total = Mapper::priceFromMoney(summary, u"total_due"_qs, &order->m_currencyCode);
    order->m_itemCount = Mapper::toInt(summary.value(u"total_items_count"_qs));

    const auto items = json.value(u"items"_qs).toArray();
    for (const auto &item : items) {
        if (auto *lot = Mapper::lotFromOrderItem(item.toObject(), &order->m_currencyCode))
            order->m_lots.append(lot);
        else if (unresolvedItems)
            ++(*unresolvedItems);
    }
    if (!order->m_itemCount) {
        qint64 itemCount = 0;
        for (const Lot *lot : std::as_const(order->m_lots))
            itemCount += lot->quantity();
        order->m_itemCount = int(std::min<qint64>(itemCount, (std::numeric_limits<int>::max)()));
    }
    return order;
}

LotList Order::loadLots() const
{
    LotList lots;
    lots.reserve(m_lots.size());
    for (const Lot *lot : m_lots)
        lots.append(new Lot(*lot));
    return lots;
}


Orders::Orders(Core *core)
    : QAbstractTableModel(core)
    , m_core(core)
{
    connect(core, &Core::authenticatedTransferStarted,
            this, [this](TransferJob *job) {
        if (m_job == job)
            emit updateStarted();
    });
    connect(core, &Core::authenticatedTransferFinished,
            this, [this](TransferJob *job) {
        if (m_job != job)
            return;
        m_job = nullptr;

        if (!job->isCompleted()) {
            finishUpdate(false, tr("Failed to download the orders") + u": "
                                    + job->errorString().toHtmlEscaped());
        } else if (job->responseCode() != 200) {
            const auto json = QJsonDocument::fromJson(job->data()).object();
            // this ends up in a rich text label, so the server's message has to be escaped
            QString error = json.value(u"message"_qs).toString().toHtmlEscaped();
            if (error.isEmpty())
                error = tr("HTTP error %1").arg(job->responseCode());
            finishUpdate(false, tr("Failed to download the orders") + u": " + error);
        } else {
            bool isLastPage = true;
            QString message;

            if (!parseOrderPage(job->data(), &isLastPage, &message))
                finishUpdate(false, message);
            else if (!isLastPage)
                requestOrderPage(m_currentPage + 1);
            else
                finishUpdate(true, message);
        }
    });
}

Orders::~Orders()
{
    qDeleteAll(m_orders);
    qDeleteAll(m_pendingOrders);
}

int Orders::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_orders.count());
}

int Orders::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant Orders::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || (index.row() >= m_orders.size()))
        return { };

    const Order *order = m_orders.at(index.row());

    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case Date:      return QLocale().toString(order->date().toLocalTime().date(),
                                                  QLocale::ShortFormat);
        case Number:    return order->number();
        case Status:    return humanizedStatus(order->status());
        case Buyer:     return order->buyerName();
        case ItemCount: return order->itemCount();
        case Total:     return Currency::toDisplayString(order->total(), order->currencyCode(), 2);
        default:        return { };
        }

    case Qt::TextAlignmentRole:
        switch (index.column()) {
        case ItemCount:
        case Total:  return int(Qt::AlignRight | Qt::AlignVCenter);
        default:     return int(Qt::AlignLeft | Qt::AlignVCenter);
        }

    case OrderSortRole:
        switch (index.column()) {
        case Date:      return order->date();
        case ItemCount: return order->itemCount();
        case Total:     return order->total();
        default:        return data(index, Qt::DisplayRole);
        }

    case OrderPointerRole:
        return QVariant::fromValue(order);

    default:
        return { };
    }
}

QVariant Orders::headerData(int section, Qt::Orientation orientation, int role) const
{
    if ((orientation != Qt::Horizontal) || (role != Qt::DisplayRole))
        return { };

    switch (section) {
    case Date:      return tr("Date");
    case Number:    return tr("Order ID");
    case Status:    return tr("Status");
    case Buyer:     return tr("Buyer");
    case ItemCount: return tr("Items");
    case Total:     return tr("Total");
    default:        return { };
    }
}

void Orders::setUpdateStatus(UpdateStatus updateStatus)
{
    if (updateStatus != m_updateStatus) {
        m_updateStatus = updateStatus;
        emit updateStatusChanged(updateStatus);
    }
}

bool Orders::startUpdate(int daysBack)
{
    if (m_updateStatus == UpdateStatus::Updating)
        return false;

    Q_ASSERT(!m_job);
    setUpdateStatus(UpdateStatus::Updating);

    qDeleteAll(m_pendingOrders);
    m_pendingOrders.clear();
    m_cutoffDate = QDateTime(QDate::currentDate().addDays(-daysBack), QTime(0, 0));
    m_currentPage = 0;
    m_lastPage = 1;
    m_unresolvedItems = 0;

    requestOrderPage(1);
    return true;
}

void Orders::cancelUpdate()
{
    if ((m_updateStatus == UpdateStatus::Updating) && m_job)
        m_job->abort();
}

void Orders::requestOrderPage(int page)
{
    // the page we asked for is authoritative: a server echoing a page number that never
    // advances would otherwise keep this loop requesting the same page forever
    m_currentPage = page;

    const QUrlQuery query {
        { u"page"_qs, QString::number(page) },
        { u"perPage"_qs, QString::number(ordersPerPage) },
        { u"include"_qs, u"items"_qs },
        { u"sort"_qs, u"-created_at"_qs },
    };

    m_job = m_core->createGet(u"/marketplace/seller/orders"_qs, query);
    m_core->retrieveAuthenticated(m_job);
}

bool Orders::parseOrderPage(const QByteArray &data, bool *isLastPage, QString *message)
{
    QJsonParseError parseError;
    const auto json = QJsonDocument::fromJson(data, &parseError).object();
    if (parseError.error != QJsonParseError::NoError) {
        *message = tr("Failed to parse the orders") + u": " + parseError.errorString();
        return false;
    }

    // the newest orders come first, so the page containing the cutoff date is the last one
    // we are interested in
    bool reachedCutoff = false;

    const auto orders = json.value(u"data"_qs).toArray();
    for (const auto &orderValue : orders) {
        auto *order = Order::fromJson(orderValue.toObject(), &m_unresolvedItems);

        if (order->date().isValid() && (order->date() < m_cutoffDate)) {
            reachedCutoff = true;
            delete order;
        } else {
            m_pendingOrders.append(order);
        }
    }

    const auto meta = json.value(u"meta"_qs).toObject();
    const int reportedLastPage = meta.value(u"last_page"_qs).toInt(m_currentPage);
    m_lastPage = std::max(m_currentPage, std::min(reportedLastPage, maxOrderPages));

    emit updateProgress(m_currentPage, m_lastPage);

    // an empty page ends the pagination, no matter what the server claims
    *isLastPage = reachedCutoff || orders.isEmpty() || (m_currentPage >= m_lastPage);
    return true;
}

void Orders::finishUpdate(bool success, const QString &message)
{
    QString fullMessage = message;

    if (success) {
        beginResetModel();
        qDeleteAll(m_orders);
        m_orders = m_pendingOrders;
        m_pendingOrders.clear();
        endResetModel();

        m_lastUpdated = QDateTime::currentDateTime();

        if (m_unresolvedItems) {
            fullMessage = tr("%Ln order item(s) could not be matched against the BrickLink "
                             "catalog and were skipped.", nullptr, m_unresolvedItems);
        }
    } else {
        qDeleteAll(m_pendingOrders);
        m_pendingOrders.clear();
    }

    setUpdateStatus(success ? UpdateStatus::Ok : UpdateStatus::UpdateFailed);
    emit updateFinished(success, fullMessage);
}

} // namespace BrickZap

#include "moc_orders.cpp"
