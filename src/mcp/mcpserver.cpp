/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "mcpserver.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QHttpHeaders>
#include <QHttpServer>
#include <QHttpServerConfiguration>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QSaveFile>
#include <QTcpServer>
#include <QUrl>
#include <QUuid>
#include <cmath>

namespace {
using Status = QHttpServerResponse::StatusCode;
const QStringList versions{QStringLiteral("2025-11-25"), QStringLiteral("2025-06-18"), QStringLiteral("2025-03-26")};

QHttpServerResponse response(const QJsonObject &body, Status status = Status::Ok, const QByteArray &session = {})
{
    QHttpServerResponse result(body, status);
    auto headers = result.headers();
    headers.append("Cache-Control", "no-store");
    if (!session.isEmpty()) headers.append("Mcp-Session-Id", session);
    result.setHeaders(std::move(headers));
    return result;
}

QHttpServerResponse rpcError(const QJsonValue &id, int code, const QString &message, Status status = Status::Ok)
{
    return response({{"jsonrpc", "2.0"}, {"id", id}, {"error", QJsonObject{{"code", code}, {"message", message}}}}, status);
}

bool authenticated(const QByteArray &provided, const QByteArray &token)
{
    const QByteArray expected = QByteArray("Bearer ") + token;
    if (provided.size() != expected.size()) return false;
    unsigned char difference = 0;
    for (qsizetype i = 0; i < expected.size(); ++i)
        difference |= static_cast<unsigned char>(provided[i] ^ expected[i]);
    return difference == 0;
}

bool accepts(const QByteArray &header, const QByteArray &mime)
{
    for (const auto &part : header.split(','))
        if (part.split(';').first().trimmed().toLower() == mime) return true;
    return false;
}
} // namespace

McpServer::McpServer(QJsonArray tools, ToolCall callTool, QString credentialFile, QObject *parent)
    : QObject(parent)
    , m_callTool(std::move(callTool))
    , m_tools(std::move(tools))
    , m_credentialFile(std::move(credentialFile))
    , m_status(QStringLiteral("Disabled"))
{
}

McpServer::~McpServer() = default;

void McpServer::setStatus(const QString &status)
{
    m_status = status;
    Q_EMIT statusChanged(m_status);
}

QString McpServer::status() const
{
    return m_status;
}

bool McpServer::tokenRequired() const
{
    return m_access != McpAccess::Local;
}

QString McpServer::endpoint() const
{
    QString host = QStringLiteral("127.0.0.1");
    if (m_access == McpAccess::Network) {
        // Advertise an address other devices can reach.
        for (const auto &address : QNetworkInterface::allAddresses()) {
            if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback() && !address.isLinkLocal()) {
                host = address.toString();
                break;
            }
        }
    }
    return QStringLiteral("http://%1:%2/mcp").arg(host).arg(m_port);
}

QString McpServer::clientConfiguration(McpClientFormat format) const
{
    if (!m_http) return {};
    const QString url = endpoint();
    const QString authorization = tokenRequired() ? QStringLiteral("Bearer ") + QString::fromLatin1(m_token) : QString();
    switch (format) {
    case McpClientFormat::Generic: {
        QJsonObject server{{"type", "http"}, {"url", url}};
        if (!authorization.isEmpty()) server.insert("headers", QJsonObject{{"Authorization", authorization}});
        return QString::fromUtf8(QJsonDocument(QJsonObject{{"mcpServers", QJsonObject{{"kdenlive", server}}}}).toJson(QJsonDocument::Indented));
    }
    case McpClientFormat::ClaudeCode:
        return QStringLiteral("claude mcp add --scope user --transport http kdenlive %1").arg(url) +
               (authorization.isEmpty() ? QString() : QStringLiteral(" --header \"Authorization: %1\"").arg(authorization)) + QLatin1Char('\n');
    case McpClientFormat::Codex:
        return QStringLiteral("[mcp_servers.kdenlive]\nurl = \"%1\"\n").arg(url) +
               (authorization.isEmpty() ? QString() : QStringLiteral("http_headers = { Authorization = \"%1\" }\n").arg(authorization));
    case McpClientFormat::Plain:
        return QStringLiteral("URL: %1\n").arg(url) + (authorization.isEmpty() ? QString() : QStringLiteral("Authorization: %1\n").arg(authorization));
    }
    return {};
}

bool McpServer::loadCredential(bool rotate)
{
    const auto permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    QFile existing(m_credentialFile);
    if (!rotate && existing.exists()) {
        if (!existing.setPermissions(permissions) || !existing.open(QIODevice::ReadOnly)) {
            setStatus(QStringLiteral("Cannot read the private MCP credential file."));
            return false;
        }
        const auto token = existing.read(128).trimmed();
        bool valid = token.size() == 64;
        for (char c : token)
            valid = valid && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        if (!valid) {
            setStatus(QStringLiteral("Invalid MCP credential file. Regenerate the token in settings."));
            return false;
        }
        m_token = token;
        return true;
    }
    const QByteArray token = QUuid::createUuid().toRfc4122().toHex() + QUuid::createUuid().toRfc4122().toHex();
    if (!QDir().mkpath(QFileInfo(m_credentialFile).absolutePath())) {
        setStatus(QStringLiteral("Cannot create the MCP credential directory."));
        return false;
    }
    QSaveFile file(m_credentialFile);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(permissions) || file.write(token + '\n') != token.size() + 1 || !file.commit()) {
        setStatus(QStringLiteral("Cannot save the private MCP credential file."));
        return false;
    }
    m_token = token;
    return true;
}

void McpServer::configure(bool enabled, int port, const QString &mediaRoot, McpAccess access)
{
    // KUrlRequester may persist a file URL even when its KConfig entry is a Path.
    const QUrl mediaUrl(mediaRoot);
    const QString localRoot = mediaUrl.isLocalFile() ? mediaUrl.toLocalFile() : mediaRoot;
    if (m_enabled == enabled && m_port == port && m_mediaRoot == localRoot && m_access == access && (m_http || !enabled)) return;
    stop();
    m_enabled = enabled;
    m_port = port;
    m_mediaRoot = localRoot;
    m_access = access;
    if (!enabled) return;
    if (port < 1024 || port > 65535) {
        setStatus(QStringLiteral("MCP port must be between 1024 and 65535."));
        return;
    }
    if (!localRoot.isEmpty() && (!QFileInfo(localRoot).isDir() || !QFileInfo(localRoot).isAbsolute())) {
        setStatus(QStringLiteral("The additional media folder must be an existing absolute directory."));
        return;
    }
    m_token.clear();
    if (tokenRequired() && !loadCredential()) return;
    auto http = std::make_unique<QHttpServer>();
    QHttpServerConfiguration limits;
    limits.setMaximumBodySize(128 * 1024);
    limits.setMaximumTotalHeaderSize(16 * 1024);
    limits.setMaximumHeaderFieldSize(8 * 1024);
    limits.setMaximumHeaderFieldCount(64);
    limits.setMaximumUrlSize(2048);
    limits.setKeepAliveTimeout(std::chrono::seconds(10));
    limits.setRateLimitPerSecond(100);
    http->setConfiguration(limits);
    http->route(QStringLiteral("/mcp"), QHttpServerRequest::Method::AnyKnown, this, [this](const QHttpServerRequest &request) { return respond(request); });
    auto *tcp = new QTcpServer(http.get());
    tcp->setMaxPendingConnections(16);
    const QHostAddress address = access == McpAccess::Network ? QHostAddress(QHostAddress::Any) : QHostAddress(QHostAddress::LocalHost);
    if (!tcp->listen(address, quint16(port)) || !http->bind(tcp)) {
        setStatus(QStringLiteral("Cannot listen on port %1: %2").arg(port).arg(tcp->errorString()));
        return;
    }
    m_http = std::move(http);
    if (access == McpAccess::Network)
        setStatus(QStringLiteral("Listening on all network interfaces at %1 (access token required, unencrypted)").arg(endpoint()));
    else
        setStatus(QStringLiteral("Listening on %1%2").arg(endpoint(), tokenRequired() ? QStringLiteral(" (access token required)") : QString()));
}

void McpServer::stop()
{
    m_http.reset();
    m_sessions.clear();
    m_enabled = false;
    setStatus(QStringLiteral("Disabled"));
}

void McpServer::rotateToken()
{
    const bool enabled = m_enabled;
    stop();
    if (loadCredential(true)) configure(enabled, m_port, m_mediaRoot, m_access);
}

QHttpServerResponse McpServer::respond(const QHttpServerRequest &request)
{
    const QJsonValue nullId(QJsonValue::Null);
    // Browsers always send Origin. Without a token, the Host check also defeats DNS rebinding.
    const QByteArray host = request.value("Host").toLower();
    const QByteArray port = ':' + QByteArray::number(m_port);
    const bool localHost = host == "127.0.0.1" + port || host == "localhost" + port;
    if (request.headers().contains("Origin") || (m_access != McpAccess::Network && !localHost))
        return response({{"error", "Only direct MCP clients are allowed."}}, Status::Forbidden);
    if (tokenRequired() && !authenticated(request.value("Authorization"), m_token)) {
        auto reply = response({{"error", "A valid MCP bearer token is required."}}, Status::Unauthorized);
        auto headers = reply.headers();
        headers.append("WWW-Authenticate", "Bearer realm=\"Kdenlive\"");
        reply.setHeaders(std::move(headers));
        return reply;
    }
    if (request.method() != QHttpServerRequest::Method::Post && request.method() != QHttpServerRequest::Method::Delete) {
        auto reply = response({{"error", "Use POST for MCP messages or DELETE to end a session."}}, Status::MethodNotAllowed);
        auto headers = reply.headers();
        headers.append("Allow", "POST, DELETE");
        reply.setHeaders(std::move(headers));
        return reply;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        if (now - it->lastSeen > 30 * 60 * 1000)
            it = m_sessions.erase(it);
        else
            ++it;
    }
    const auto sessionId = request.value("Mcp-Session-Id");
    const QString headerVersion = QString::fromLatin1(request.value("MCP-Protocol-Version"));
    if (!headerVersion.isEmpty() && !versions.contains(headerVersion)) return rpcError(nullId, -32600, "Unsupported MCP protocol version.", Status::BadRequest);
    if (request.method() == QHttpServerRequest::Method::Delete) {
        if (sessionId.isEmpty()) return response({{"error", "Mcp-Session-Id is required."}}, Status::BadRequest);
        if (m_sessions.remove(sessionId) == 0) return response({{"error", "Session not found."}}, Status::NotFound);
        return QHttpServerResponse(Status::NoContent);
    }
    if (request.value("Content-Type").split(';').first().trimmed().toLower() != "application/json")
        return rpcError(nullId, -32600, "Content-Type must be application/json.", Status::UnsupportedMediaType);
    if (!accepts(request.value("Accept"), "application/json") || !accepts(request.value("Accept"), "text/event-stream"))
        return rpcError(nullId, -32600, "Accept must include application/json and text/event-stream.", Status::NotAcceptable);
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(request.body(), &parseError);
    if (parseError.error != QJsonParseError::NoError) return rpcError(nullId, -32700, "Invalid JSON.", Status::BadRequest);
    if (!document.isObject()) return rpcError(nullId, -32600, "Expected one JSON-RPC object.", Status::BadRequest);
    const auto message = document.object();
    const bool notification = !message.contains("id");
    const auto id = message.value("id");
    const bool validId = id.isString() || (id.isDouble() && std::isfinite(id.toDouble()) && std::floor(id.toDouble()) == id.toDouble() &&
                                           std::abs(id.toDouble()) <= 9007199254740991.);
    if (message.value("jsonrpc") != QLatin1String("2.0") || !message.value("method").isString() || (!notification && !validId) ||
        (message.contains("params") && !message.value("params").isObject()))
        return rpcError(nullId, -32600, "Invalid JSON-RPC message.", Status::BadRequest);
    const QString method = message.value("method").toString();
    const auto params = message.value("params").toObject();
    if (method == QLatin1String("initialize")) {
        if (notification || !sessionId.isEmpty() || !params.value("protocolVersion").isString() || !params.value("capabilities").isObject() ||
            !params.value("clientInfo").isObject())
            return rpcError(notification ? nullId : id, -32602, "Invalid initialization parameters.", Status::BadRequest);
        if (m_sessions.size() >= 32) return response({{"error", "Too many MCP sessions."}}, Status::ServiceUnavailable);
        const QString requested = params.value("protocolVersion").toString();
        const QString version = versions.contains(requested) ? requested : versions.first();
        const QByteArray allocated = QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1();
        m_sessions.insert(allocated, {version, false, now});
        return response(
            {{"jsonrpc", "2.0"},
             {"id", id},
             {"result",
              QJsonObject{{"protocolVersion", version},
                          {"capabilities", QJsonObject{{"tools", QJsonObject{{"listChanged", false}}}}},
                          {"serverInfo", QJsonObject{{"name", "kdenlive-native"}, {"version", "1.0.0"}}},
                          {"instructions", "This server edits the visible Kdenlive project. Read desktop_state first and preserve its session/revision. Use a "
                                           "new requestId for each edit and retry uncertain outcomes only with the identical payload. Import/replacement are "
                                           "asynchronous; wait for bin readiness. Edits share native Undo with the user. Open and save a local project before "
                                           "editing. Media imports are restricted to the project folder and configured additional media folder."}}}},
            Status::Ok, allocated);
    }
    if (sessionId.isEmpty()) return rpcError(notification ? nullId : id, -32600, "Mcp-Session-Id is required.", Status::BadRequest);
    auto session = m_sessions.find(sessionId);
    if (session == m_sessions.end()) return response({{"error", "Session not found. Initialize again."}}, Status::NotFound);
    if (!headerVersion.isEmpty() && headerVersion != session->version)
        return rpcError(notification ? nullId : id, -32600, "Protocol version does not match this session.", Status::BadRequest);
    session->lastSeen = now;
    if (notification) {
        if (method == QLatin1String("notifications/initialized")) session->initialized = true;
        return QHttpServerResponse(Status::Accepted);
    }
    if (method == QLatin1String("ping")) return response({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{}}});
    if (!session->initialized) return rpcError(id, -32000, "Send notifications/initialized before using tools.");
    if (method == QLatin1String("tools/list")) return response({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{{"tools", m_tools}}}});
    if (method != QLatin1String("tools/call")) return rpcError(id, -32601, "Method not found.");
    const auto name = params.value("name").toString();
    bool known = false;
    for (const auto &tool : m_tools)
        if (tool.toObject().value("name").toString() == name) known = true;
    if (!known || (params.contains("arguments") && !params.value("arguments").isObject())) return rpcError(id, -32602, "Unknown tool or invalid arguments.");
    const auto result = m_callTool(name, params.value("arguments").toObject(), m_mediaRoot);
    const bool ok = result.value("ok").toBool();
    const QJsonObject envelope = ok ? QJsonObject{{"ok", true}, {"data", result}} : result;
    const QString text = QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact));
    return response(
        {{"jsonrpc", "2.0"},
         {"id", id},
         {"result", QJsonObject{{"isError", !ok}, {"structuredContent", envelope}, {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}}}}});
}
