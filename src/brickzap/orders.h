// Copyright (C) 2004-2026 Robert Griebl
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QtCore/QAbstractTableModel>
#include <QtCore/QDateTime>
#include <QtCore/QString>

#include "bricklink/global.h"
#include "bricklink/lot.h"
#include "brickzap/global.h"

QT_FORWARD_DECLARE_CLASS(QJsonObject)

class TransferJob;

namespace BrickZap {

class Core;

class Order
{
public:
    Order() = default;
    ~Order();
    Q_DISABLE_COPY_MOVE(Order)

    static Order *fromJson(const QJsonObject &json, int *unresolvedItems = nullptr);

    QString id() const              { return m_id; }
    QString number() const          { return m_number; }
    QString status() const          { return m_status; }
    QString buyerName() const       { return m_buyerName; }
    QDateTime date() const          { return m_date; }
    double total() const            { return m_total; }
    QString currencyCode() const    { return m_currencyCode; }
    int itemCount() const           { return m_itemCount; }
    int lotCount() const            { return int(m_lots.count()); }

    // ownership is transferred to the caller
    BrickLink::LotList loadLots() const;

private:
    QString m_id;
    QString m_number;
    QString m_status;
    QString m_buyerName;
    QDateTime m_date;
    double m_total = 0;
    QString m_currencyCode;
    int m_itemCount = 0;
    BrickLink::LotList m_lots;
};


class Orders : public QAbstractTableModel
{
    Q_OBJECT

public:
    ~Orders() override;
    Q_DISABLE_COPY_MOVE(Orders)

    enum Column {
        Date = 0,
        Number,
        Status,
        Buyer,
        ItemCount,
        Total,

        ColumnCount,
    };

    enum Role {
        OrderPointerRole = Qt::UserRole + 1,
        OrderSortRole,
    };

    int rowCount(const QModelIndex &parent = { }) const override;
    int columnCount(const QModelIndex &parent = { }) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    BrickLink::UpdateStatus updateStatus() const { return m_updateStatus; }
    QDateTime lastUpdated() const                { return m_lastUpdated; }

    bool startUpdate(int daysBack);
    void cancelUpdate();

signals:
    void updateStarted();
    void updateProgress(int received, int total);
    void updateFinished(bool success, const QString &message);
    void updateStatusChanged(BrickLink::UpdateStatus updateStatus);

private:
    Orders(Core *core);

    void setUpdateStatus(BrickLink::UpdateStatus updateStatus);
    void requestOrderPage(int page);
    bool parseOrderPage(const QByteArray &data, bool *isLastPage, QString *message);
    void finishUpdate(bool success, const QString &message);

    Core *m_core;
    BrickLink::UpdateStatus m_updateStatus = BrickLink::UpdateStatus::UpdateFailed;
    TransferJob *m_job = nullptr;
    QDateTime m_lastUpdated;

    QList<Order *> m_orders;
    QList<Order *> m_pendingOrders;
    QDateTime m_cutoffDate;
    int m_currentPage = 0;
    int m_lastPage = 1;
    int m_unresolvedItems = 0;

    friend class Core;
};

} // namespace BrickZap

Q_DECLARE_METATYPE(BrickZap::Order *)
Q_DECLARE_METATYPE(const BrickZap::Order *)
