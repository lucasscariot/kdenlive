/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <functional>
#include <memory>

class QHttpServer;
class QHttpServerRequest;
class QHttpServerResponse;

/** Who may connect. Network access always requires the bearer token. */
enum class McpAccess { Local, LocalWithToken, Network };
/** Client configuration snippets offered for copying. */
enum class McpClientFormat { Generic, ClaudeCode, Codex, Plain };
/** The MCP connection behind a tool call: a short opaque id derived from its session (never the session id itself) and its clientInfo name. */
struct McpCaller
{
    QString client;
    QString clientName;
};

class McpServer final : public QObject
{
    Q_OBJECT
public:
    using ToolCall = std::function<QJsonObject(const QString &, const QJsonObject &, const QString &, const McpCaller &)>;
    McpServer(QJsonArray tools, ToolCall callTool, QString credentialFile, QObject *parent = nullptr);
    ~McpServer() override;
    void configure(bool enabled, int port, const QString &mediaRoot, McpAccess access = McpAccess::Local);
    void stop();
    void rotateToken();
    QString status() const;
    QString clientConfiguration(McpClientFormat format) const;

Q_SIGNALS:
    void statusChanged(const QString &status);

private:
    struct Session
    {
        QString version;
        bool initialized{false};
        qint64 lastSeen{0};
        McpCaller caller;
    };
    QHttpServerResponse respond(const QHttpServerRequest &request);
    bool loadCredential(bool rotate = false);
    bool tokenRequired() const;
    QString endpoint() const;
    void setStatus(const QString &status);
    ToolCall m_callTool;
    QJsonArray m_tools;
    QString m_credentialFile;
    QByteArray m_token;
    std::unique_ptr<QHttpServer> m_http;
    QHash<QByteArray, Session> m_sessions;
    QString m_status;
    QString m_mediaRoot;
    int m_port{8765};
    McpAccess m_access{McpAccess::Local};
    bool m_enabled{false};
};
