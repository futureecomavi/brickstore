// Copyright (C) 2004-2026 Robert Griebl
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QObject>
#include <QtCore/QStringList>

#include "bricklink/global.h"
#include "bricklink/lot.h"
#include "brickzap/global.h"

QT_FORWARD_DECLARE_CLASS(QTimer)

class TransferJob;

namespace BrickZap {

class Core;

// The seller's BrickZap inventory: downloaded page by page and uploaded through the
// asynchronous bulk import, which is dispatched in chunks and then polled until it finishes.

class Store : public QObject
{
    Q_OBJECT

public:
    ~Store() override;
    Q_DISABLE_COPY_MOVE(Store)

    bool isValid() const               { return m_valid; }
    QDateTime lastUpdated() const      { return m_lastUpdated; }
    BrickLink::UpdateStatus updateStatus() const { return m_updateStatus; }
    int lotCount() const               { return int(m_lots.count()); }
    const BrickLink::LotList &lots() const { return m_lots; }
    QString currencyCode() const       { return m_currencyCode; }
    // listings that could not be matched against the BrickLink catalog
    int unresolvedListingCount() const { return m_unresolvedListings; }

    bool startUpdate();
    void cancelUpdate();

    BrickLink::UpdateStatus uploadStatus() const { return m_uploadStatus; }
    bool startUpload(const BrickLink::LotList &lots, const QString &currencyCode);
    void cancelUpload();

signals:
    void updateStarted();
    void updateProgress(int received, int total);
    void updateFinished(bool success, const QString &message);
    void updateStatusChanged(BrickLink::UpdateStatus updateStatus);
    void isValidChanged(bool valid);
    void lastUpdatedChanged(const QDateTime &lastUpdated);
    void currencyCodeChanged(const QString &currencyCode);
    void lotCountChanged(int lotCount);

    void uploadStarted();
    void uploadProgress(int received, int total);
    void uploadFinished(bool success, const QString &message);
    void uploadStatusChanged(BrickLink::UpdateStatus uploadStatus);

private:
    Store(Core *core);

    void setUpdateStatus(BrickLink::UpdateStatus updateStatus);
    void setUploadStatus(BrickLink::UpdateStatus uploadStatus);
    void setLastUpdated(const QDateTime &lastUpdated);

    void requestInventoryPage(int page);
    bool parseInventoryPage(const QByteArray &data, QString *message);
    void finishUpdate(bool success, const QString &message);

    void dispatchNextImportChunk();
    void importChunkDispatched(const QByteArray &data);
    void pollImportJob();
    void importJobUpdated(const QByteArray &data);
    void finishUpload(bool success, const QString &message);

    Core *m_core;

    bool m_valid = false;
    BrickLink::UpdateStatus m_updateStatus = BrickLink::UpdateStatus::UpdateFailed;
    TransferJob *m_job = nullptr;
    BrickLink::LotList m_lots;
    QDateTime m_lastUpdated;
    QString m_currencyCode;
    int m_currentPage = 0;
    int m_lastPage = 1;
    int m_unresolvedListings = 0;

    BrickLink::UpdateStatus m_uploadStatus = BrickLink::UpdateStatus::Ok;
    TransferJob *m_uploadJob = nullptr;
    QTimer *m_uploadPollTimer = nullptr;
    QList<QJsonArray> m_importChunks;
    int m_currentChunk = 0;
    int m_uploadedItems = 0;
    int m_totalUploadItems = 0;
    int m_skippedLots = 0;
    QString m_importJobId;
    int m_importCreated = 0;
    int m_importUpdated = 0;
    int m_importFailed = 0;
    QStringList m_importErrors;

    friend class Core;
};

} // namespace BrickZap
