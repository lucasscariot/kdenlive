/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "mcpserver.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest>

class ProtocolTest : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir directory;
    QNetworkAccessManager network;
    std::unique_ptr<McpServer> server;
    QByteArray token;
    quint16 port;
    int calls{0};
    McpCaller lastCaller;
    struct Reply
    {
        int status;
        QByteArray body;
        QByteArray session;
        QNetworkReply::NetworkError error;
    };

    quint16 freePort()
    {
        QTcpServer probe;
        if (!probe.listen(QHostAddress::LocalHost)) qFatal("Cannot allocate test port");
        return probe.serverPort();
    }
    Reply send(QByteArray body, QByteArray session = {}, QByteArray method = "POST", QByteArray authorization = {}, QByteArray origin = {},
               QByteArray host = {})
    {
        QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1/mcp").arg(port)));
        request.setTransferTimeout(3000);
        request.setRawHeader("Content-Type", "application/json");
        request.setRawHeader("Accept", "application/json, text/event-stream");
        if (!authorization.isNull())
            request.setRawHeader("Authorization", authorization);
        else if (!token.isEmpty())
            request.setRawHeader("Authorization", "Bearer " + token);
        if (!host.isNull()) request.setRawHeader("Host", host);
        if (!session.isEmpty()) request.setRawHeader("Mcp-Session-Id", session);
        if (!origin.isNull()) request.setRawHeader("Origin", origin);
        auto *reply = network.sendCustomRequest(request, method, body);
        QSignalSpy finished(reply, &QNetworkReply::finished);
        if (!finished.wait(5000)) qFatal("HTTP response timed out");
        Reply result{reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), reply->readAll(), reply->rawHeader("Mcp-Session-Id"), reply->error()};
        reply->deleteLater();
        return result;
    }
    QByteArray initialize()
    {
        return send(
                   R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"test","version":"1"}}})")
            .session;
    }
private Q_SLOTS:
    void init()
    {
        port = freePort();
        calls = 0;
        server = std::make_unique<McpServer>(
            QJsonArray{QJsonObject{{"name", "probe"}, {"inputSchema", QJsonObject{{"type", "object"}}}},
                       QJsonObject{{"name", "picture"}, {"inputSchema", QJsonObject{{"type", "object"}}}}},
            [this](const QString &name, const QJsonObject &, const QString &, const McpCaller &caller) {
                ++calls;
                lastCaller = caller;
                if (name == QLatin1String("picture"))
                    return QJsonObject{{"ok", true}, {"width", 1}, {"image", QJsonObject{{"data", "iVBORw0KGgo="}, {"mimeType", "image/png"}}}};
                return QJsonObject{{"ok", true}, {"calls", calls}};
            },
            directory.filePath("token"));
        server->configure(true, port, {}, McpAccess::LocalWithToken);
        QVERIFY(server->status().startsWith("Listening"));
        QFile file(directory.filePath("token"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        token = file.readAll().trimmed();
        QCOMPARE(token.size(), 64);
    }
    void cleanup() { server.reset(); }
    void lifecycleAndTools()
    {
        auto session = initialize();
        QVERIFY(!session.isEmpty());
        auto early = send(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})", session);
        QVERIFY(QJsonDocument::fromJson(early.body).object().contains("error"));
        QCOMPARE(send(R"({"jsonrpc":"2.0","method":"notifications/initialized"})", session).status, 202);
        const auto listed = send(R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})", session);
        QCOMPARE(listed.status, 200);
        QCOMPARE(QJsonDocument::fromJson(listed.body).object()["result"].toObject()["tools"].toArray().size(), 2);
        const auto tool = send(R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"probe","arguments":{}}})", session);
        QCOMPARE(tool.status, 200);
        QCOMPARE(calls, 1);
        QVERIFY(QJsonDocument::fromJson(tool.body).object()["result"].toObject()["structuredContent"].toObject()["ok"].toBool());
        // Tools learn which connection called them through an opaque digest and the clientInfo name, never the session id.
        QCOMPARE(lastCaller.client.size(), 12);
        QVERIFY(!QString::fromLatin1(session).contains(lastCaller.client));
        QCOMPARE(lastCaller.clientName, QStringLiteral("test"));
        const auto firstClient = lastCaller.client;
        const auto other = initialize();
        QCOMPARE(send(R"({"jsonrpc":"2.0","method":"notifications/initialized"})", other).status, 202);
        send(R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"probe","arguments":{}}})", other);
        QVERIFY(lastCaller.client != firstClient);
        send(R"({"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"probe","arguments":{}}})", session);
        QCOMPARE(lastCaller.client, firstClient);
        const auto picture =
            QJsonDocument::fromJson(send(R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"picture","arguments":{}}})", session).body)
                .object()["result"]
                .toObject();
        // Images become MCP image content and are not duplicated in the structured result.
        const auto content = picture["content"].toArray();
        QCOMPARE(content.size(), 2);
        QCOMPARE(content.at(1).toObject()["type"].toString(), QString("image"));
        QCOMPARE(content.at(1).toObject()["mimeType"].toString(), QString("image/png"));
        QVERIFY(!picture["structuredContent"].toObject()["data"].toObject().contains("image"));
        QCOMPARE(send({}, session, "GET").status, 405);
        QCOMPARE(send({}, session, "DELETE").status, 204);
        QCOMPARE(send(R"({"jsonrpc":"2.0","id":5,"method":"ping"})", session).status, 404);
    }
    void rejectsUnauthenticatedAndBrowserCalls()
    {
        QCOMPARE(send("{}", {}, "POST", "Bearer incorrect").status, 401);
        QCOMPARE(send("{}", {}, "POST", {}, "https://example.com").status, 403);
        QCOMPARE(calls, 0);
        QCOMPARE(QFile::permissions(directory.filePath("token")) & (QFile::ReadGroup | QFile::ReadOther | QFile::WriteGroup | QFile::WriteOther),
                 QFile::Permissions{});
    }
    void malformedAndLimits()
    {
        QCOMPARE(send("not json").status, 400);
        QCOMPARE(send("[]").status, 400);
        QCOMPARE(send(R"({"jsonrpc":"2.0","id":null,"method":"ping"})").status, 400);
        const auto oversized = send(QByteArray(129 * 1024, 'x'));
        // Qt closes oversized requests; a concurrent client write may report EPIPE as UnknownNetworkError.
        QVERIFY2(oversized.status == 413 || (oversized.status == 0 && (oversized.error == QNetworkReply::RemoteHostClosedError ||
                                                                       oversized.error == QNetworkReply::UnknownNetworkError)),
                 qPrintable(QString("status=%1 error=%2 body=%3").arg(oversized.status).arg(oversized.error).arg(QString::fromUtf8(oversized.body))));
        QVERIFY(!initialize().isEmpty());
        QCOMPARE(calls, 0);
    }
    void localWithoutToken()
    {
        server->configure(true, port, {});
        QVERIFY(server->status().startsWith("Listening on http://127.0.0.1"));
        token.clear();
        const auto session = initialize();
        QVERIFY(!session.isEmpty());
        QCOMPARE(send(R"({"jsonrpc":"2.0","id":2,"method":"ping"})", session, "POST", {}, {}, "localhost:" + QByteArray::number(port)).status, 200);
        // Web pages and DNS rebinding stay blocked without a token.
        QCOMPARE(send("{}", {}, "POST", {}, "https://example.com").status, 403);
        QCOMPARE(send("{}", {}, "POST", {}, {}, "attacker.example:" + QByteArray::number(port)).status, 403);
        QVERIFY(!server->clientConfiguration(McpClientFormat::Generic).contains("Authorization"));
        QCOMPARE(calls, 0);
    }
    void networkRequiresToken()
    {
        server->configure(true, port, {}, McpAccess::Network);
        QVERIFY(server->status().contains("access token required"));
        QCOMPARE(send("{}", {}, "POST", "Bearer incorrect").status, 401);
        const auto host = "kdenlive.lan:" + QByteArray::number(port);
        const auto session =
            send(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"test","version":"1"}}})",
                {}, "POST", {}, {}, host)
                .session;
        QVERIFY(!session.isEmpty());
        QCOMPARE(send("{}", {}, "POST", {}, "https://example.com").status, 403);
    }
    void clientConfigurations()
    {
        const auto url = QStringLiteral("http://127.0.0.1:%1/mcp").arg(port);
        const auto bearer = QStringLiteral("Bearer ") + QString::fromLatin1(token);
        const auto generic = QJsonDocument::fromJson(server->clientConfiguration(McpClientFormat::Generic).toUtf8()).object();
        const auto entry = generic["mcpServers"].toObject()["kdenlive"].toObject();
        QCOMPARE(entry["type"].toString(), QString("http"));
        QCOMPARE(entry["url"].toString(), url);
        QCOMPARE(entry["headers"].toObject()["Authorization"].toString(), bearer);
        const auto claude = server->clientConfiguration(McpClientFormat::ClaudeCode);
        QVERIFY(claude.startsWith("claude mcp add --scope user --transport http kdenlive " + url));
        QVERIFY(claude.contains(bearer));
        QVERIFY(server->clientConfiguration(McpClientFormat::Codex).startsWith("[mcp_servers.kdenlive]\nurl = \"" + url));
        QVERIFY(server->clientConfiguration(McpClientFormat::Plain).contains("Authorization: " + bearer));
    }
    void settingsLifecycle()
    {
        const auto first = initialize();
        server->configure(true, port, {}, McpAccess::LocalWithToken);
        QCOMPARE(send(R"({"jsonrpc":"2.0","id":2,"method":"ping"})", first).status, 200);
        QTcpServer occupied;
        QVERIFY(occupied.listen(QHostAddress::LocalHost));
        server->configure(true, occupied.serverPort(), {}, McpAccess::LocalWithToken);
        QVERIFY(server->status().startsWith("Cannot listen"));
        QVERIFY(server->clientConfiguration(McpClientFormat::Generic).isEmpty());
        QCOMPARE(send(R"({"jsonrpc":"2.0","id":3,"method":"ping"})", first).status, 0);
        port = freePort();
        server->configure(true, port, {}, McpAccess::LocalWithToken);
        QVERIFY(server->status().startsWith("Listening"));
        QCOMPARE(send(R"({"jsonrpc":"2.0","id":4,"method":"ping"})", first).status, 404);
        server->rotateToken();
        QCOMPARE(send("{}").status, 401);
        server->configure(false, port, {}, McpAccess::LocalWithToken);
        QCOMPARE(server->status(), QString("Disabled"));
        QVERIFY(server->clientConfiguration(McpClientFormat::Generic).isEmpty());
    }
    void folderPickerFileUrl()
    {
        const QString media = directory.filePath("media folder");
        QVERIFY(QDir().mkpath(media));
        server->configure(true, port, media, McpAccess::LocalWithToken);
        const auto session = initialize();
        QVERIFY(!session.isEmpty());
        server->configure(true, port, QUrl::fromLocalFile(media).toString(QUrl::FullyEncoded), McpAccess::LocalWithToken);
        QVERIFY(server->status().startsWith("Listening"));
        // Applying the equivalent folder-picker URL must preserve the live connection.
        QCOMPARE(send(R"({"jsonrpc":"2.0","id":2,"method":"ping"})", session).status, 200);
        server->configure(true, port, QStringLiteral("https://example.com/media"), McpAccess::LocalWithToken);
        QVERIFY(server->status().startsWith("The additional media folder"));
        QVERIFY(server->clientConfiguration(McpClientFormat::Generic).isEmpty());
    }
};
QTEST_GUILESS_MAIN(ProtocolTest)
#include "protocoltest.moc"
