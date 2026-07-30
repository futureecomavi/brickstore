// Copyright (C) 2004-2026 Robert Griebl
// SPDX-License-Identifier: GPL-3.0-only

#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QUrlQuery>

#include "utility/transfer.h"
#include "brickzap/core.h"
#include "brickzap/mapper.h"
#include "brickzap/store.h"

using namespace BrickLink;

namespace {

// the seller API accepts up to 100 listings per page
constexpr int listingsPerPage = 100;
// the async import accepts far more, but smaller chunks give better progress feedback and
// keep a single failure from taking down a whole store upload
constexpr int importChunkSize = 1000;
constexpr int importPollIntervalMs = 1500;
constexpr int maxReportedImportErrors = 10;

QString errorMessageFromJson(const QByteArray &data, const QString &fallback)
{
    const auto json = QJsonDocument::fromJson(data).object();
    const QString message = json.value(u"message"_qs).toString();
    return message.isEmpty() ? fallback : message;
}

} // namespace


namespace BrickZap {

Store::Store(Core *core)
    : QObject(core)
    , m_core(core)
{
    connect(core, &Core::authenticatedTransferStarted,
            this, [this](TransferJob *job) {
        if (m_job == job)
            emit updateStarted();
        else if (m_uploadJob == job)
            emit uploadStarted();
    });
    connect(core, &Core::authenticatedTransferFinished,
            this, [this](TransferJob *job) {
        if (m_job == job) {
            m_job = nullptr;

            if (!job->isCompleted()) {
                finishUpdate(false, tr("Failed to download the store inventory")
                                        + u": " + job->errorString());
            } else if (job->responseCode() != 200) {
                finishUpdate(false, tr("Failed to download the store inventory") + u": "
                                        + errorMessageFromJson(job->data(),
                                                               tr("HTTP error %1")
                                                                   .arg(job->responseCode())));
            } else {
                QString message;
                if (!parseInventoryPage(job->data(), &message))
                    finishUpdate(false, message);
                else if (m_currentPage < m_lastPage)
                    requestInventoryPage(m_currentPage + 1);
                else
                    finishUpdate(true, message);
            }
        } else if (m_uploadJob == job) {
            m_uploadJob = nullptr;
            const bool isPoll = !m_importJobId.isEmpty();

            if (!job->isCompleted()) {
                finishUpload(false, tr("Failed to upload the inventory to BrickZap")
                                        + u": " + job->errorString());
            } else if ((job->responseCode() != 200) && (job->responseCode() != 202)) {
                finishUpload(false, tr("Failed to upload the inventory to BrickZap") + u": "
                                        + errorMessageFromJson(job->data(),
                                                               tr("HTTP error %1")
                                                                   .arg(job->responseCode())));
            } else if (isPoll) {
                importJobUpdated(job->data());
            } else {
                importChunkDispatched(job->data());
            }
        }
    });
}

Store::~Store()
{
    qDeleteAll(m_lots);
}

void Store::setUpdateStatus(UpdateStatus updateStatus)
{
    if (updateStatus != m_updateStatus) {
        m_updateStatus = updateStatus;
        emit updateStatusChanged(updateStatus);
    }
}

void Store::setUploadStatus(UpdateStatus uploadStatus)
{
    if (uploadStatus != m_uploadStatus) {
        m_uploadStatus = uploadStatus;
        emit uploadStatusChanged(uploadStatus);
    }
}

void Store::setLastUpdated(const QDateTime &lastUpdated)
{
    if (lastUpdated != m_lastUpdated) {
        m_lastUpdated = lastUpdated;
        emit lastUpdatedChanged(lastUpdated);
    }
}

bool Store::startUpdate()
{
    if (m_updateStatus == UpdateStatus::Updating)
        return false;

    Q_ASSERT(!m_job);
    setUpdateStatus(UpdateStatus::Updating);

    qDeleteAll(m_lots);
    m_lots.clear();
    m_unresolvedListings = 0;
    m_currentPage = 0;
    m_lastPage = 1;
    m_currencyCode.clear();

    requestInventoryPage(1);
    return true;
}

void Store::cancelUpdate()
{
    if ((m_updateStatus == UpdateStatus::Updating) && m_job)
        m_job->abort();
}

void Store::requestInventoryPage(int page)
{
    const QUrlQuery query {
        { u"page"_qs, QString::number(page) },
        { u"per_page"_qs, QString::number(listingsPerPage) },
        // a stable order keeps concurrent changes from shifting listings between pages
        { u"sort_by"_qs, u"created_at"_qs },
        { u"sort_order"_qs, u"asc"_qs },
    };

    m_job = m_core->createGet(u"/marketplace/seller/products"_qs, query);
    m_core->retrieveAuthenticated(m_job);
}

bool Store::parseInventoryPage(const QByteArray &data, QString *message)
{
    QJsonParseError parseError;
    const auto json = QJsonDocument::fromJson(data, &parseError).object();
    if (parseError.error != QJsonParseError::NoError) {
        *message = tr("Failed to parse the store inventory") + u": " + parseError.errorString();
        return false;
    }

    const auto listings = json.value(u"data"_qs).toArray();
    for (const auto &listing : listings) {
        if (auto *lot = Mapper::lotFromListing(listing.toObject(), &m_currencyCode))
            m_lots.append(lot);
        else
            ++m_unresolvedListings;
    }

    const auto meta = json.value(u"meta"_qs).toObject();
    m_currentPage = meta.value(u"current_page"_qs).toInt(m_currentPage + 1);
    m_lastPage = std::max(m_currentPage, meta.value(u"last_page"_qs).toInt(m_currentPage));

    emit updateProgress(m_currentPage, m_lastPage);
    return true;
}

void Store::finishUpdate(bool success, const QString &message)
{
    QString fullMessage = message;

    if (success) {
        emit currencyCodeChanged(m_currencyCode);
        emit lotCountChanged(lotCount());

        if (m_unresolvedListings) {
            fullMessage = tr("%Ln listing(s) could not be matched against the BrickLink catalog "
                             "and were skipped.", nullptr, m_unresolvedListings);
        }
    } else {
        qDeleteAll(m_lots);
        m_lots.clear();
    }

    if (success != m_valid) {
        m_valid = success;
        emit isValidChanged(m_valid);
    }
    setUpdateStatus(success ? UpdateStatus::Ok : UpdateStatus::UpdateFailed);
    setLastUpdated(QDateTime::currentDateTime());
    emit updateFinished(success, fullMessage);
}

bool Store::startUpload(const LotList &lots, const QString &currencyCode)
{
    if (m_uploadStatus == UpdateStatus::Updating)
        return false;

    Q_ASSERT(!m_uploadJob);

    m_importChunks.clear();
    m_currentChunk = 0;
    m_uploadedItems = 0;
    m_skippedLots = 0;
    m_importJobId.clear();
    m_importCreated = m_importUpdated = m_importFailed = 0;
    m_importErrors.clear();

    QJsonArray chunk;
    for (const Lot *lot : lots) {
        // excluded lots are not for sale and BrickZap has no equivalent state
        if (!lot->item() || (lot->status() == Status::Exclude) || (lot->quantity() <= 0)) {
            ++m_skippedLots;
            continue;
        }
        chunk.append(Mapper::importItemFromLot(lot, currencyCode));

        if (chunk.size() >= importChunkSize) {
            m_importChunks.append(chunk);
            chunk = { };
        }
    }
    if (!chunk.isEmpty())
        m_importChunks.append(chunk);

    m_totalUploadItems = 0;
    for (const auto &c : std::as_const(m_importChunks))
        m_totalUploadItems += int(c.size());

    if (m_importChunks.isEmpty()) {
        setUploadStatus(UpdateStatus::UpdateFailed);
        emit uploadFinished(false, tr("There are no lots that could be uploaded to BrickZap."));
        return false;
    }

    setUploadStatus(UpdateStatus::Updating);
    dispatchNextImportChunk();
    return true;
}

void Store::cancelUpload()
{
    if (m_uploadStatus != UpdateStatus::Updating)
        return;

    if (m_uploadPollTimer)
        m_uploadPollTimer->stop();

    if (m_uploadJob) {
        m_uploadJob->abort();
    } else {
        // waiting for the next poll: nothing is in flight, so report the cancellation directly
        finishUpload(false, tr("The upload was cancelled."));
    }
}

void Store::dispatchNextImportChunk()
{
    m_importJobId.clear();

    const QJsonObject body {
        { u"items"_qs, m_importChunks.at(m_currentChunk) },
        // BrickZap resolves both the item ids and the colors in the BrickLink namespace
        { u"format"_qs, u"bricklink"_qs },
        { u"id_format"_qs, u"bricklink"_qs },
        { u"source_format"_qs, u"json"_qs },
        // merge quantities into existing listings, but keep one listing per BrickLink lot
        { u"mode"_qs, u"merge"_qs },
        { u"duplicate_handling"_qs, u"separate"_qs },
    };

    m_uploadJob = m_core->createPost(u"/marketplace/seller/products/imports/async"_qs, body);
    m_core->retrieveAuthenticated(m_uploadJob);
}

void Store::importChunkDispatched(const QByteArray &data)
{
    const auto json = QJsonDocument::fromJson(data).object();
    m_importJobId = json.value(u"data"_qs).toObject().value(u"job_id"_qs).toString();

    if (m_importJobId.isEmpty()) {
        finishUpload(false, tr("BrickZap did not return an import job id."));
        return;
    }

    if (!m_uploadPollTimer) {
        m_uploadPollTimer = new QTimer(this);
        m_uploadPollTimer->setSingleShot(true);
        m_uploadPollTimer->setInterval(importPollIntervalMs);
        connect(m_uploadPollTimer, &QTimer::timeout, this, &Store::pollImportJob);
    }
    m_uploadPollTimer->start();
}

void Store::pollImportJob()
{
    if ((m_uploadStatus != UpdateStatus::Updating) || m_importJobId.isEmpty())
        return;

    m_uploadJob = m_core->createGet(u"/marketplace/seller/products/bulk-jobs/"_qs + m_importJobId);
    m_core->retrieveAuthenticated(m_uploadJob);
}

void Store::importJobUpdated(const QByteArray &data)
{
    const auto job = QJsonDocument::fromJson(data).object().value(u"data"_qs).toObject();
    const QString status = job.value(u"status"_qs).toString();

    const int done = job.value(u"progress_current"_qs).toInt();
    emit uploadProgress(m_uploadedItems + done, m_totalUploadItems);

    if ((status == u"pending") || (status == u"running")) {
        m_uploadPollTimer->start();
        return;
    }

    if (status == u"failed") {
        finishUpload(false, tr("The BrickZap import failed") + u": "
                                + job.value(u"failure_message"_qs).toString());
        return;
    }

    const auto result = job.value(u"result"_qs).toObject();
    const auto meta = result.value(u"meta"_qs).toObject();
    m_importCreated += meta.value(u"created"_qs).toInt();
    m_importUpdated += meta.value(u"updated"_qs).toInt();
    m_importFailed += meta.value(u"failed"_qs).toInt();

    const auto errors = result.value(u"errors"_qs).toArray();
    for (const auto &errorValue : errors) {
        // { "index": 0, "status": 422, "error": { "message": ..., "catalog_sku": ... } }
        const auto error = errorValue.toObject().value(u"error"_qs).toObject();
        const QString message = error.value(u"message"_qs).toString();
        const QString sku = error.value(u"bricklink_sku"_qs).toString().isEmpty()
                                ? error.value(u"catalog_sku"_qs).toString()
                                : error.value(u"bricklink_sku"_qs).toString();

        if (!message.isEmpty() && (m_importErrors.size() < maxReportedImportErrors))
            m_importErrors.append(sku.isEmpty() ? message : (sku + u": " + message));
    }

    if (status == u"cancelled") {
        finishUpload(false, tr("The BrickZap import was cancelled."));
        return;
    }

    m_uploadedItems += int(m_importChunks.at(m_currentChunk).size());
    ++m_currentChunk;

    if (m_currentChunk < m_importChunks.size()) {
        dispatchNextImportChunk();
        return;
    }

    QString message = tr("%1 listings created, %2 updated, %3 failed.")
                          .arg(m_importCreated).arg(m_importUpdated).arg(m_importFailed);
    if (m_skippedLots) {
        message += u"<br>" + tr("%Ln lot(s) were skipped, because they are excluded or empty.",
                                nullptr, m_skippedLots);
    }
    if (!m_importErrors.isEmpty())
        message += u"<br><br>" + m_importErrors.join(u"<br>"_qs);

    finishUpload(true, message);
}

void Store::finishUpload(bool success, const QString &message)
{
    if (m_uploadPollTimer)
        m_uploadPollTimer->stop();
    m_importJobId.clear();
    m_importChunks.clear();

    setUploadStatus(success ? UpdateStatus::Ok : UpdateStatus::UpdateFailed);
    emit uploadFinished(success, message);
}

} // namespace BrickZap

#include "moc_store.cpp"
