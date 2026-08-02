// Copyright (C) 2004-2026 Robert Griebl
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <memory>

#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>

#include "brickzap/global.h"

QT_FORWARD_DECLARE_CLASS(QJsonObject)

class Transfer;
class TransferJob;

namespace BrickZap {

// Talks to the BrickZap marketplace seller API. Authentication uses seller API clients
// (client-credentials): the client id/secret are exchanged for a short-lived JWT, which is
// renewed via the refresh token. Store scoping is embedded in the token, so no tenancy
// headers are ever sent.

class Core : public QObject
{
    Q_OBJECT

public:
    ~Core() override;

    static QString defaultApiBaseUrl();
    // trims whitespace and trailing slashes
    static QString normalizeApiBaseUrl(const QString &baseUrl);
    // an api server has to be https (or http on loopback), as it receives the client secret
    static bool isValidApiBaseUrl(const QString &baseUrl);

    QString apiBaseUrl() const;
    // an invalid url is rejected and replaced by defaultApiBaseUrl()
    void setApiBaseUrl(const QString &baseUrl);

    void setCredentials(const QString &clientId, const QString &clientSecret);
    bool hasCredentials() const;

    bool isAuthenticated() const;
    QString authenticationError() const;

    QUrl urlForPath(const QString &path, const QUrlQuery &query = { }) const;
    QUrl urlForStoreInventory() const;
    QUrl urlForOrders() const;

    TransferJob *createGet(const QString &path, const QUrlQuery &query = { }) const;
    TransferJob *createPost(const QString &path, const QJsonObject &body) const;

    void retrieveAuthenticated(TransferJob *job);
    void authenticate();
    void cancelTransfers();

    Store *store() const    { return m_store.get(); }
    Orders *orders() const  { return m_orders.get(); }

signals:
    void authenticatedTransferOverallProgress(int progress, int total);
    void authenticatedTransferStarted(TransferJob *job);
    void authenticatedTransferProgress(TransferJob *job, int progress, int total);
    void authenticatedTransferFinished(TransferJob *job);

    void authenticationChanged(bool authenticated);
    void authenticationFinished(const QString &error);

private:
    Core();

    static Core *create();
    static inline Core *inst() { return s_inst; }
    static Core *s_inst;

    friend Core *core();
    friend Core *create();

    bool hasValidToken() const;
    void startLogin();
    void loginFinished(TransferJob *job);
    void setAuthenticated(bool authenticated);
    void dispatchWaitingJobs();
    void failWaitingJobs();
    void applyAuthentication(TransferJob *job) const;

    QString m_apiBaseUrl;
    QString m_clientId;
    QString m_clientSecret;

    Transfer *m_transfer = nullptr;
    bool m_authenticated = false;
    QByteArray m_accessToken;
    QString m_refreshToken;
    QDateTime m_accessTokenExpires;
    QString m_authenticationError;

    TransferJob *m_loginJob = nullptr;
    bool m_loginIsRefresh = false;
    QVector<TransferJob *> m_jobsWaitingForAuthentication;
    QHash<TransferJob *, int> m_jobAuthenticationRetries;

    std::unique_ptr<Store> m_store;
    std::unique_ptr<Orders> m_orders;
};

inline Core *core() { return Core::inst(); }
inline Core *create() { return Core::create(); }

} // namespace BrickZap
