/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <functional>

class KdenliveDoc;
class TimelineItemModel;

/** Optional, explicitly enabled local editing API. All calls run on the GUI thread. */
class LiveBridge final : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kdenlive.LiveBridge1")

public:
    explicit LiveBridge(QObject *parent);
    /** Policy runs only for a new, revision-checked request, never for a receipt replay. */
    QString applyAuthorized(const QString &request, const std::function<QJsonObject()> &authorize);
    /** Read-only production helpers; each validates its own arguments. */
    QJsonObject frameCapture(const QJsonObject &arguments);
    QJsonObject effectList(const QJsonObject &arguments);
    QJsonObject titleRead(const QJsonObject &arguments);
    QJsonObject renderStatus();

public Q_SLOTS:
    Q_SCRIPTABLE QString capabilities() const;
    Q_SCRIPTABLE QString state();
    Q_SCRIPTABLE QString apply(const QString &request);

Q_SIGNALS:
    Q_SCRIPTABLE void changed(const QString &event);

private:
    bool bind();
    void contentChanged();
    QJsonObject snapshot() const;
    QJsonObject execute(const QJsonObject &command);
    QJsonObject executeBatch(const QJsonObject &command);
    QJsonObject executeProduction(const QJsonObject &command, bool &handled);
    QJsonObject startRender(const QJsonObject &command);
    QJsonObject editableClip(int clipId) const;
    QString failure(const QString &code, const QString &message) const;

    QPointer<KdenliveDoc> m_document;
    QPointer<TimelineItemModel> m_timeline;
    QList<QMetaObject::Connection> m_connections;
    QString m_session;
    qint64 m_revision{0};
    bool m_emissionPending{false};
    bool m_executing{false};
    struct Receipt
    {
        QByteArray fingerprint;
        QString response;
    };
    QHash<QString, Receipt> m_receipts;
    QQueue<QString> m_receiptOrder;
    struct RenderState
    {
        QString preset;
        QString status;
        int progress{0};
        int frame{0};
        QString error;
        qint64 started{0};
    };
    QMap<QString, RenderState> m_renders;
    bool m_renderTracking{false};
};
