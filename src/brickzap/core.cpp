// Copyright (C) 2004-2026 Robert Griebl
// SPDX-License-Identifier: GPL-3.0-only

#include <QJsonDocument>
#include <QJsonObject>

#include "utility/transfer.h"
#include "brickzap/core.h"
#include "brickzap/orders.h"
#include "brickzap/store.h"

Q_LOGGING_CATEGORY(LogBrickZap, "bs.brickzap", QtWarningMsg)

namespace BrickZap {

Core *Core::s_inst = nullptr;

Core *Core::create()
{
    if (!s_inst)
        s_inst = new Core();
    return s_inst;
}

Core::Core()
    : m_apiBaseUrl(defaultApiBaseUrl())
    , m_transfer(new Transfer(this))
    , m_store(new Store(this))
    , m_orders(new Orders(this))
{
    connect(m_transfer, &Transfer::finished,
            this, [this](TransferJob *job) {
        if (!job)
            return;

        if (job == m_loginJob) {
            loginFinished(job);
            return;
        }

        if (job->responseCode() == 401) {
            // the access token was rejected (expired or revoked): re-authenticate and retry once
            int retries = m_jobAuthenticationRetries.value(job);
            if (retries < 1) {
                m_jobAuthenticationRetries.insert(job, retries + 1);
                setAuthenticated(false);
                m_accessToken.clear();
                job->resetForReuse();

                QMetaObject::invokeMethod(this, [this, job]() {
                    retrieveAuthenticated(job);
                }, Qt::QueuedConnection);
                return;
            }
        }
        m_jobAuthenticationRetries.remove(job);
        emit authenticatedTransferFinished(job);
    });
    connect(m_transfer, &Transfer::overallProgress,
            this, &Core::authenticatedTransferOverallProgress);
    connect(m_transfer, &Transfer::progress,
            this, &Core::authenticatedTransferProgress);
    connect(m_transfer, &Transfer::started,
            this, &Core::authenticatedTransferStarted);
}

Core::~Core()
{
    cancelTransfers();
    s_inst = nullptr;
}

QString Core::defaultApiBaseUrl()
{
    return u"https://api.brickzap.com"_qs;
}

QString Core::apiBaseUrl() const
{
    return m_apiBaseUrl;
}

void Core::setApiBaseUrl(const QString &baseUrl)
{
    QString url = baseUrl.trimmed();
    if (url.isEmpty())
        url = defaultApiBaseUrl();
    while (url.endsWith(u'/'))
        url.chop(1);

    if (url != m_apiBaseUrl) {
        m_apiBaseUrl = url;
        setAuthenticated(false);
        m_accessToken.clear();
        m_refreshToken.clear();
        m_transfer->abortAllJobs();
    }
}

void Core::setCredentials(const QString &clientId, const QString &clientSecret)
{
    if ((clientId == m_clientId) && (clientSecret == m_clientSecret))
        return;

    m_clientId = clientId;
    m_clientSecret = clientSecret;
    m_accessToken.clear();
    m_refreshToken.clear();
    m_accessTokenExpires = { };
    m_authenticationError.clear();
    setAuthenticated(false);
    m_transfer->abortAllJobs();
}

bool Core::hasCredentials() const
{
    return !m_clientId.isEmpty() && !m_clientSecret.isEmpty();
}

bool Core::isAuthenticated() const
{
    return m_authenticated;
}

QString Core::authenticationError() const
{
    return m_authenticationError;
}

QUrl Core::urlForPath(const QString &path, const QUrlQuery &query) const
{
    QUrl url(m_apiBaseUrl + (path.startsWith(u'/') ? path : (u"/" + path)));
    if (!query.isEmpty())
        url.setQuery(query);
    return url;
}

QUrl Core::urlForStoreInventory() const
{
    return urlForPath(u"/marketplace/seller/products"_qs);
}

QUrl Core::urlForOrders() const
{
    return urlForPath(u"/marketplace/seller/orders"_qs);
}

static void prepareApiJob(TransferJob *job)
{
    if (!job)
        return;
    job->setSendBrickLinkHeaders(false);
    job->setAcceptAllResponseCodes(true);
    job->setRawHeader("Accept", "application/json");
}

TransferJob *Core::createGet(const QString &path, const QUrlQuery &query) const
{
    auto *job = TransferJob::get(urlForPath(path, query).toString());
    prepareApiJob(job);
    return job;
}

TransferJob *Core::createPost(const QString &path, const QJsonObject &body) const
{
    auto *job = TransferJob::post(urlForPath(path).toString(), { }, u"application/json"_qs,
                                  QJsonDocument(body).toJson(QJsonDocument::Compact));
    prepareApiJob(job);
    return job;
}

bool Core::hasValidToken() const
{
    if (!m_authenticated || m_accessToken.isEmpty())
        return false;
    return !m_accessTokenExpires.isValid()
           || (QDateTime::currentDateTimeUtc() < m_accessTokenExpires);
}

void Core::retrieveAuthenticated(TransferJob *job)
{
    if (hasValidToken()) {
        if (job) {
            applyAuthentication(job);
            m_transfer->retrieve(job);
        }
        return;
    }

    if (job)
        m_jobsWaitingForAuthentication << job;

    if (!m_loginJob) {
        if (!hasCredentials()) {
            m_authenticationError = tr("No BrickZap API credentials found.");
            qCWarning(LogBrickZap) << "Aborting transfer due to missing credentials";
            emit authenticationFinished(m_authenticationError);
            failWaitingJobs();
            return;
        }
        startLogin();
    }
}

void Core::authenticate()
{
    retrieveAuthenticated(nullptr);
}

void Core::startLogin()
{
    m_loginIsRefresh = !m_refreshToken.isEmpty();

    QJsonObject body;
    QString path;

    if (m_loginIsRefresh) {
        path = u"/auth/api-refresh"_qs;
        body[u"refresh_token"_qs] = m_refreshToken;
    } else {
        path = u"/auth/api-login"_qs;
        body[u"client_id"_qs] = m_clientId;
        body[u"client_secret"_qs] = m_clientSecret;
    }

    m_loginJob = createPost(path, body);
    m_transfer->retrieve(m_loginJob, true /*highPriority*/);
}

void Core::loginFinished(TransferJob *job)
{
    m_loginJob = nullptr;

    QString error;

    if (job->isCompleted() && (job->responseCode() == 200)) {
        const auto json = QJsonDocument::fromJson(job->data()).object();
        const QString accessToken = json.value(u"access_token"_qs).toString();

        if (accessToken.isEmpty()) {
            error = tr("BrickZap did not return an access token.");
        } else {
            m_accessToken = accessToken.toUtf8();
            m_refreshToken = json.value(u"refresh_token"_qs).toString();

            const int expiresIn = json.value(u"expires_in"_qs).toInt();
            // renew a minute early, so that queued jobs cannot race the expiry
            m_accessTokenExpires = (expiresIn > 60)
                                       ? QDateTime::currentDateTimeUtc().addSecs(expiresIn - 60)
                                       : QDateTime { };
        }
    } else if (m_loginIsRefresh) {
        // the refresh token is gone or was revoked: start over with the client credentials
        qCDebug(LogBrickZap) << "Refreshing the BrickZap token failed, retrying with a full login";
        m_refreshToken.clear();
        startLogin();
        return;
    } else {
        const auto json = QJsonDocument::fromJson(job->data()).object();
        error = json.value(u"error_description"_qs).toString();
        if (error.isEmpty())
            error = json.value(u"message"_qs).toString();
        if (error.isEmpty()) {
            error = job->errorString();
            if (error.isEmpty())
                error = tr("HTTP error %1").arg(job->responseCode());
        }
    }

    m_authenticationError = error;
    setAuthenticated(error.isEmpty());
    emit authenticationFinished(error);

    if (m_authenticated)
        dispatchWaitingJobs();
    else
        failWaitingJobs();
}

void Core::setAuthenticated(bool authenticated)
{
    if (authenticated != m_authenticated) {
        m_authenticated = authenticated;
        emit authenticationChanged(m_authenticated);
    }
}

void Core::applyAuthentication(TransferJob *job) const
{
    job->setRawHeader("Authorization", "Bearer " + m_accessToken);
}

void Core::dispatchWaitingJobs()
{
    const auto jobs = m_jobsWaitingForAuthentication;
    m_jobsWaitingForAuthentication.clear();

    for (TransferJob *job : jobs) {
        applyAuthentication(job);
        m_transfer->retrieve(job);
    }
}

void Core::failWaitingJobs()
{
    const auto jobs = m_jobsWaitingForAuthentication;
    m_jobsWaitingForAuthentication.clear();

    for (TransferJob *job : jobs) {
        // these jobs were never handed to the Transfer, so we have to abort, report and
        // delete them ourselves
        job->abort();
        m_jobAuthenticationRetries.remove(job);
        emit authenticatedTransferFinished(job);
        delete job;
    }
}

void Core::cancelTransfers()
{
    failWaitingJobs();
    m_transfer->abortAllJobs();
}

} // namespace BrickZap

#include "moc_core.cpp"
