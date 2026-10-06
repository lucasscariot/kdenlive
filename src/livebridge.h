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

class KdenliveDoc;
class TimelineItemModel;

/** Optional, explicitly enabled local editing API. All calls run on the GUI thread. */
class LiveBridge final : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kdenlive.LiveBridge1")

public:
    explicit LiveBridge(QObject *parent);

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
};
