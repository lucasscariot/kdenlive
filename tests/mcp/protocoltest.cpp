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
    Reply send(QByteArray body, QByteArray session = {}, QByteArray method = "POST", QByteArray authorization = {}, QByteArray origin = {})
    {
        QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1/mcp").arg(port)));
        request.setTransferTimeout(3000);
        request.setRawHeader("Content-Type", "application/json");
        request.setRawHeader("Accept", "application/json, text/event-stream");
        request.setRawHeader("Authorization", authorization.isNull() ? "Bearer " + token : authorization);
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
            QJsonArray{QJsonObject{{"name", "probe"}, {"inputSchema", QJsonObject{{"type", "object"}}}}},
            [this](const QString &, const QJsonObject &, const QString &) {
                ++calls;
                return QJsonObject{{"ok", true}, {"calls", calls}};
            },
            directory.filePath("token"));
        server->configure(true, port, {});
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
        QCOMPARE(QJsonDocument::fromJson(listed.body).object()["result"].toObject()["tools"].toArray().size(), 1);
        const auto tool = send(R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"probe","arguments":{}}})", session);
        QCOMPARE(tool.status, 200);
        QCOMPARE(calls, 1);
        QVERIFY(QJsonDocument::fromJson(tool.body).object()["result"].toObject()["structuredContent"].toObject()["ok"].toBool());
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
    void settingsLifecycle()
    {
        const auto first = initialize();
        server->configure(true, port, {});
        QCOMPARE(send(R"({"jsonrpc":"2.0","id":2,"method":"ping"})", first).status, 200);
        QTcpServer occupied;
        QVERIFY(occupied.listen(QHostAddress::LocalHost));
        server->configure(true, occupied.serverPort(), {});
        QVERIFY(server->status().startsWith("Cannot listen"));
        QVERIFY(server->clientConfiguration().isEmpty());
        QCOMPARE(send(R"({"jsonrpc":"2.0","id":3,"method":"ping"})", first).status, 0);
        port = freePort();
        server->configure(true, port, {});
        QVERIFY(server->status().startsWith("Listening"));
        QCOMPARE(send(R"({"jsonrpc":"2.0","id":4,"method":"ping"})", first).status, 404);
        server->rotateToken();
        QCOMPARE(send("{}").status, 401);
        server->configure(false, port, {});
        QCOMPARE(server->status(), QString("Disabled"));
        QVERIFY(server->clientConfiguration().isEmpty());
    }
    void folderPickerFileUrl()
    {
        const QString media = directory.filePath("media folder");
        QVERIFY(QDir().mkpath(media));
        server->configure(true, port, media);
        const auto session = initialize();
        QVERIFY(!session.isEmpty());
        server->configure(true, port, QUrl::fromLocalFile(media).toString(QUrl::FullyEncoded));
        QVERIFY(server->status().startsWith("Listening"));
        // Applying the equivalent folder-picker URL must preserve the live connection.
        QCOMPARE(send(R"({"jsonrpc":"2.0","id":2,"method":"ping"})", session).status, 200);
        server->configure(true, port, QStringLiteral("https://example.com/media"));
        QVERIFY(server->status().startsWith("The additional media folder"));
        QVERIFY(server->clientConfiguration().isEmpty());
    }
};
QTEST_GUILESS_MAIN(ProtocolTest)
#include "protocoltest.moc"
