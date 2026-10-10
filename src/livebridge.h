/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QStringList>
#include <QUndoStack>
#include <functional>
#include <limits>

class KdenliveDoc;
class TimelineItemModel;

/** Optional, explicitly enabled local editing API. All calls run on the GUI thread. */
class LiveBridge final : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kdenlive.LiveBridge1")

public:
    explicit LiveBridge(QObject *parent);
    /** Who submitted an edit, recorded on the history entries it creates. client identifies one connection (an MCP session digest, or "dbus"). */
    struct Caller
    {
        QString tool;
        QString client;
        QString clientName;
    };
    /** Policy runs only for a new, revision-checked request, never for a receipt replay. */
    QString applyAuthorized(const QString &request, const std::function<QJsonObject()> &authorize, const Caller &caller);
    /** The shared Undo history with the origin of each entry (mcp, user or unknown); see docs/native-mcp.md "Change log". */
    QJsonObject history(const QJsonObject &arguments);
    /** Read-only production helpers; each validates its own arguments. */
    QJsonObject frameCapture(const QJsonObject &arguments);
    QJsonObject effectList(const QJsonObject &arguments);
    QJsonObject titleRead(const QJsonObject &arguments);
    QJsonObject renderStatus();
    /** Sequence guides or a bin clip's markers as json, csv or Kdenlive's native guide JSON text. */
    QJsonObject markerExport(const QJsonObject &arguments);
    /** Scoped state read: optional include (section names), trackId and range {start, end}; see docs/native-mcp.md. */
    QJsonObject stateFor(const QJsonObject &arguments);
    /** Every section name stateFor accepts in include. */
    static const QStringList &stateSectionNames();

public Q_SLOTS:
    Q_SCRIPTABLE QString capabilities() const;
    Q_SCRIPTABLE QString state();
    Q_SCRIPTABLE QString apply(const QString &request);

Q_SIGNALS:
    Q_SCRIPTABLE void changed(const QString &event);

private:
    bool bind();
    void contentChanged();
    /** Which parts of the active sequence a snapshot contains; frames in [start, end). */
    struct StateScope
    {
        QStringList sections;
        int trackId{-1};
        int start{0};
        int end{std::numeric_limits<int>::max()};
    };
    /** The sections embedded in every mutation result and returned by a plain state read. */
    static StateScope defaultScope();
    QJsonObject snapshot(const StateScope &scope) const;
    QJsonObject execute(const QJsonObject &command);
    QJsonObject executeBatch(const QJsonObject &command);
    QJsonObject executeProduction(const QJsonObject &command, bool &handled);
    QJsonObject executeMarker(const QJsonObject &command);
    QJsonObject startRender(const QJsonObject &command);
    QJsonObject editableClip(int clipId) const;
    QString failure(const QString &code, const QString &message) const;

    /** One QUndoStack command as seen by the bridge; row i of m_history is stack->command(i). */
    struct HistoryEntry
    {
        const QUndoCommand *command{nullptr};
        QString text;
        QString origin;
        qint64 time{0};
        // Only for origin "mcp".
        QString sessionId;
        QString requestId;
        QString type;
        Caller caller;
        qint64 revisionBefore{0};
        qint64 revisionAfter{0};
        QJsonObject summary;
        QJsonArray children;
    };
    /** The edit request being executed; history entries pushed meanwhile belong to it. */
    struct Request
    {
        QString requestId;
        QString type;
        Caller caller;
        qint64 revisionBefore{0};
        QList<const QUndoCommand *> created;
    };
    void attachHistory(KdenliveDoc *document);
    void syncHistory();
    void finishRequest(const QJsonObject &command, const QJsonObject &result);
    QJsonObject historyJson(int row) const;
    QJsonObject executeHistoryStep(const QJsonObject &command);
    QString logPath() const;
    void writeLog(const QJsonObject &line);

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
    QList<HistoryEntry> m_history;
    QPointer<QUndoStack> m_historyStack;
    QList<QMetaObject::Connection> m_historyConnections;
    bool m_historySeeded{false};
    Request *m_request{nullptr};
};
