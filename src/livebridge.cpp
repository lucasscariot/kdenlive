/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "livebridge.h"

#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "doc/docundostack.hpp"
#include "doc/kdenlivedoc.h"
#include "mainwindow.h"
#include "timeline2/model/timelineitemmodel.hpp"
#include "timeline2/view/timelinewidget.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusError>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QTimer>
#include <QUuid>
#include <cmath>
#include <limits>
#include <mlt++/MltProfile.h>

namespace {
QString json(const QJsonObject &value)
{
    return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact));
}

QJsonObject error(const QString &code, const QString &message)
{
    return {{"ok", false}, {"error", QJsonObject{{"code", code}, {"message", message}}}};
}

bool integer(const QJsonObject &object, const QString &key, int minimum = 0)
{
    const auto value = object.value(key);
    const double number = value.toDouble(-1);
    return value.isDouble() && std::isfinite(number) && number >= minimum && number <= std::numeric_limits<int>::max() && std::floor(number) == number;
}

bool keys(const QJsonObject &object, const QStringList &allowed)
{
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!allowed.contains(it.key())) return false;
    }
    return true;
}
} // namespace

LiveBridge::LiveBridge(QObject *parent)
    : QObject(parent)
{
    if (!QDBusConnection::sessionBus().registerObject(QStringLiteral("/org/kde/kdenlive/LiveBridge"), this,
                                                      QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals)) {
        qWarning() << "Live bridge registration failed:" << QDBusConnection::sessionBus().lastError().message();
        return;
    }
    // Detect document/sequence switches even when no client is currently calling us.
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] { bind(); });
    timer->start(100);
}

QString LiveBridge::capabilities() const
{
    return json({{"ok", true},
                 {"protocolVersion", 1},
                 {"transport", "session-dbus"},
                 {"operations", QJsonArray{"insert", "move", "trim", "undo", "redo"}},
                 {"insertModes", QJsonArray{"video", "audio"}},
                 {"frameRanges", "sourceOut is exclusive; frames use project FPS"},
                 {"stateScope", "active sequence tracks, clips and project bin; not a full project interchange format"},
                 {"receiptLimit", 64},
                 {"batchEditing", false}});
}

bool LiveBridge::bind()
{
    if (m_executing) return !m_timeline.isNull();
    KdenliveDoc *document = nullptr;
    TimelineItemModel *timeline = nullptr;
    if (pCore->guiReady() && !pCore->closing && pCore->window()->hasTimeline()) {
        auto *widget = pCore->window()->getCurrentTimeline();
        document = pCore->currentDoc();
        if (document && widget && !widget->loading && !document->isBusy()) timeline = widget->model().get();
    }
    if (document == m_document && timeline == m_timeline) return timeline != nullptr;
    for (const auto &connection : std::as_const(m_connections))
        disconnect(connection);
    m_connections.clear();
    m_document = document;
    m_timeline = timeline;
    m_session = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_revision = 0;
    m_receipts.clear();
    m_receiptOrder.clear();
    if (timeline) {
        const auto bump = [this] { contentChanged(); };
        m_connections << connect(document->commandStack().get(), &QUndoStack::indexChanged, this, bump);
        m_connections << connect(document, &KdenliveDoc::docModified, this, bump);
        m_connections << connect(timeline, &QAbstractItemModel::rowsInserted, this, bump);
        m_connections << connect(timeline, &QAbstractItemModel::rowsRemoved, this, bump);
        m_connections << connect(timeline, &QAbstractItemModel::rowsMoved, this, bump);
        m_connections << connect(timeline, &QAbstractItemModel::modelReset, this, bump);
        m_connections << connect(timeline, &QAbstractItemModel::dataChanged, this, [this](const QModelIndex &, const QModelIndex &, const QList<int> &roles) {
            // Thumbnail/progress/selection updates do not edit project content.
            const QList<int> contentRoles{TimelineModel::NameRole,           TimelineModel::ResourceRole, TimelineModel::StartRole,
                                          TimelineModel::DurationRole,       TimelineModel::InPointRole,  TimelineModel::OutPointRole,
                                          TimelineModel::TrackIdRole,        TimelineModel::IsLockedRole, TimelineModel::IsDisabledRole,
                                          TimelineModel::SpeedRole,          TimelineModel::GroupedRole,  TimelineModel::EffectCountRole,
                                          TimelineModel::EffectsEnabledRole, TimelineModel::KeyframesRole};
            if (roles.isEmpty()) {
                contentChanged();
                return;
            }
            for (int role : roles)
                if (contentRoles.contains(role)) {
                    contentChanged();
                    return;
                }
        });
        auto *bin = pCore->projectItemModel().get();
        m_connections << connect(bin, &QAbstractItemModel::rowsInserted, this, bump);
        m_connections << connect(bin, &QAbstractItemModel::rowsRemoved, this, bump);
    }
    contentChanged();
    return timeline != nullptr;
}

void LiveBridge::contentChanged()
{
    ++m_revision;
    if (m_emissionPending) return;
    m_emissionPending = true;
    QTimer::singleShot(0, this, [this] {
        m_emissionPending = false;
        Q_EMIT changed(json({{"sessionId", m_session}, {"revision", m_revision}, {"ready", !m_timeline.isNull()}}));
    });
}

QJsonObject LiveBridge::snapshot() const
{
    QJsonArray tracks;
    for (int row = 0; row < m_timeline->rowCount(); ++row) {
        const auto track = m_timeline->index(row, 0);
        const int trackId = int(track.internalId());
        QJsonArray clips;
        for (int i = 0; i < m_timeline->rowCount(track); ++i) {
            const int id = int(m_timeline->index(i, 0, track).internalId());
            if (!m_timeline->isClip(id)) continue;
            const auto range = m_timeline->getClipInOut(id);
            clips.append(QJsonObject{{"id", id},
                                     {"binId", m_timeline->getClipBinId(id)},
                                     {"name", m_timeline->getClipName(id)},
                                     {"position", m_timeline->getClipPosition(id)},
                                     {"duration", m_timeline->getClipPlaytime(id)},
                                     {"sourceIn", range.first},
                                     {"sourceOut", range.second + 1},
                                     {"speed", m_timeline->getClipSpeed(id)},
                                     {"grouped", m_timeline->isInGroup(id)}});
        }
        tracks.append(QJsonObject{{"id", trackId},
                                  {"name", m_timeline->data(track, TimelineModel::NameRole).toString()},
                                  {"audio", m_timeline->isAudioTrack(trackId)},
                                  {"locked", m_timeline->data(track, TimelineModel::IsLockedRole).toBool()},
                                  {"clips", clips}});
    }
    QJsonArray bin;
    for (const auto &id : pCore->projectItemModel()->getAllClipIds()) {
        auto clip = pCore->projectItemModel()->getClipByBinID(id);
        if (clip)
            bin.append(QJsonObject{
                {"id", id}, {"name", clip->clipName()}, {"url", clip->url()}, {"ready", clip->statusReady()}, {"duration", qint64(clip->frameDuration())}});
    }
    auto stack = m_document->commandStack();
    auto &profile = pCore->getProjectProfile();
    return {{"ok", true},
            {"sessionId", m_session},
            {"revision", m_revision},
            {"sequenceId", m_timeline->uuid().toString()},
            {"documentUrl", m_document->url().toString()},
            {"modified", m_document->isModified()},
            {"fps", QJsonObject{{"numerator", profile.frame_rate_num()}, {"denominator", profile.frame_rate_den()}}},
            {"undo", QJsonObject{{"index", stack->index()},
                                 {"canUndo", stack->canUndo()},
                                 {"canRedo", stack->canRedo()},
                                 {"undoText", stack->undoText()},
                                 {"redoText", stack->redoText()}}},
            {"tracks", tracks},
            {"bin", bin}};
}

QString LiveBridge::failure(const QString &code, const QString &message) const
{
    auto result = error(code, message);
    result.insert(QStringLiteral("sessionId"), m_session);
    result.insert(QStringLiteral("revision"), m_revision);
    return json(result);
}

QString LiveBridge::state()
{
    if (!bind()) return failure(QStringLiteral("NOT_READY"), QStringLiteral("No fully loaded active timeline."));
    return json(snapshot());
}

QString LiveBridge::apply(const QString &request)
{
    if (m_executing || QApplication::activeModalWidget() || QApplication::mouseButtons() != Qt::NoButton)
        return failure(QStringLiteral("EDITOR_BUSY"), QStringLiteral("Editor is busy, dragging or has a modal dialog."));
    if (!bind()) return failure(QStringLiteral("NOT_READY"), QStringLiteral("No fully loaded active timeline."));
    if (request.size() > 65536) return failure(QStringLiteral("INVALID_REQUEST"), QStringLiteral("Request exceeds 64 KiB."));
    QJsonParseError parseError;
    const auto parsed = QJsonDocument::fromJson(request.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject())
        return failure(QStringLiteral("INVALID_REQUEST"), QStringLiteral("Expected a JSON object."));
    const auto object = parsed.object();
    const QString requestId = object.value(QStringLiteral("requestId")).toString();
    if (!keys(object, {QStringLiteral("sessionId"), QStringLiteral("requestId"), QStringLiteral("expectedRevision"), QStringLiteral("command")}) ||
        requestId.isEmpty() || requestId.size() > 128 || !object.value(QStringLiteral("command")).isObject()) {
        return failure(QStringLiteral("INVALID_REQUEST"), QStringLiteral("Expected sessionId, requestId, expectedRevision and command."));
    }
    if (object.value(QStringLiteral("sessionId")).toString() != m_session)
        return failure(QStringLiteral("SESSION_CHANGED"), QStringLiteral("Read the current timeline before editing."));
    const auto fingerprint = QCryptographicHash::hash(QJsonDocument(object).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
    if (m_receipts.contains(requestId)) {
        const auto receipt = m_receipts.value(requestId);
        if (receipt.fingerprint != fingerprint)
            return failure(QStringLiteral("REQUEST_ID_REUSED"), QStringLiteral("Use a new requestId for a different command."));
        return receipt.response;
    }
    const auto expected = object.value(QStringLiteral("expectedRevision"));
    if (!expected.isDouble() || expected.toDouble(-1) != double(m_revision))
        return failure(QStringLiteral("REVISION_CONFLICT"), QStringLiteral("Project changed; read current state before editing."));
    QScopedValueRollback<bool> executing(m_executing, true);
    auto result = execute(object.value(QStringLiteral("command")).toObject());
    if (result.value(QStringLiteral("ok")).toBool()) {
        // Even a successful no-op gets a fresh revision, preventing replay after receipt eviction.
        contentChanged();
        pCore->refreshProjectMonitorOnce();
        result.insert(QStringLiteral("state"), snapshot());
    }
    result.insert(QStringLiteral("requestId"), requestId);
    const QString response = json(result);
    m_receipts.insert(requestId, {fingerprint, response});
    m_receiptOrder.enqueue(requestId);
    if (m_receiptOrder.size() > 64) m_receipts.remove(m_receiptOrder.dequeue());
    return response;
}

QJsonObject LiveBridge::execute(const QJsonObject &command)
{
    const QString type = command.value(QStringLiteral("type")).toString();
    const auto invalid = [] { return error(QStringLiteral("INVALID_COMMAND"), QStringLiteral("Invalid command fields or frame range.")); };
    auto stack = m_document->commandStack();
    if (type == QLatin1String("undo") || type == QLatin1String("redo")) {
        if (!keys(command, {QStringLiteral("type")})) return invalid();
        const bool undo = type == QLatin1String("undo");
        if (undo ? !stack->canUndo() : !stack->canRedo()) return error(QStringLiteral("EMPTY_HISTORY"), QStringLiteral("No operation to undo or redo."));
        if (undo)
            stack->undo();
        else
            stack->redo();
        return {{"ok", true}};
    }
    if (type == QLatin1String("insert") || type == QLatin1String("move")) {
        if (!integer(command, QStringLiteral("trackId")) || !integer(command, QStringLiteral("position"))) return invalid();
        const int trackId = command.value(QStringLiteral("trackId")).toInt();
        if (!m_timeline->isTrack(trackId)) return error(QStringLiteral("UNKNOWN_TRACK"), QStringLiteral("Track does not exist."));
        if (m_timeline->data(m_timeline->makeTrackIndexFromID(trackId), TimelineModel::IsLockedRole).toBool())
            return error(QStringLiteral("TRACK_LOCKED"), QStringLiteral("Target track is locked."));
    }
    if (type == QLatin1String("insert")) {
        if (!keys(command, {QStringLiteral("type"), QStringLiteral("binId"), QStringLiteral("trackId"), QStringLiteral("position"), QStringLiteral("sourceIn"),
                            QStringLiteral("sourceOut"), QStringLiteral("media")}) ||
            !integer(command, QStringLiteral("sourceIn")) || !integer(command, QStringLiteral("sourceOut"), 1))
            return invalid();
        const auto binId = command.value(QStringLiteral("binId")).toString();
        const auto mode = command.value(QStringLiteral("media")).toString();
        if (!QRegularExpression(QStringLiteral("^[0-9]+$")).match(binId).hasMatch() || (mode != QLatin1String("video") && mode != QLatin1String("audio")))
            return invalid();
        auto clip = pCore->projectItemModel()->getClipByBinID(binId);
        if (!clip || !clip->statusReady()) return error(QStringLiteral("MEDIA_NOT_READY"), QStringLiteral("Bin clip is missing or still loading."));
        const int start = command.value(QStringLiteral("sourceIn")).toInt();
        const int end = command.value(QStringLiteral("sourceOut")).toInt();
        const int position = command.value(QStringLiteral("position")).toInt();
        if (end <= start || size_t(end) > clip->frameDuration() || qint64(position) + end - start > std::numeric_limits<int>::max()) return invalid();
        const auto nativeId =
            QStringLiteral("%1%2/%3/%4").arg(mode == QLatin1String("video") ? QStringLiteral("V") : QStringLiteral("A"), binId).arg(start).arg(end - 1);
        int id = -1;
        // finalMove=0 suppresses prompts to create missing audio tracks; native validation still applies.
        if (!m_timeline->requestClipInsertion(nativeId, command.value(QStringLiteral("trackId")).toInt(), position, id, true, true, false, 0) || id < 0)
            return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native insertion rejected the range, track type or collision."));
        return {{"ok", true}, {"clipId", id}};
    }
    if (type == QLatin1String("move") || type == QLatin1String("trim")) {
        if (!integer(command, QStringLiteral("clipId"))) return invalid();
        const int id = command.value(QStringLiteral("clipId")).toInt();
        if (!m_timeline->isClip(id)) return error(QStringLiteral("UNKNOWN_CLIP"), QStringLiteral("Timeline clip does not exist."));
        const int sourceTrack = m_timeline->getClipTrackId(id);
        if (m_timeline->data(m_timeline->makeTrackIndexFromID(sourceTrack), TimelineModel::IsLockedRole).toBool())
            return error(QStringLiteral("TRACK_LOCKED"), QStringLiteral("Source track is locked."));
        if (type == QLatin1String("move")) {
            if (!keys(command, {QStringLiteral("type"), QStringLiteral("clipId"), QStringLiteral("trackId"), QStringLiteral("position")})) return invalid();
            const int position = command.value(QStringLiteral("position")).toInt();
            if (qint64(position) + m_timeline->getClipPlaytime(id) > std::numeric_limits<int>::max()) return invalid();
            if (!m_timeline->requestClipMove(id, command.value(QStringLiteral("trackId")).toInt(), position, true, true, true, true))
                return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native move rejected the track, group or collision."));
        } else {
            const auto edge = command.value(QStringLiteral("edge")).toString();
            if (!keys(command, {QStringLiteral("type"), QStringLiteral("clipId"), QStringLiteral("duration"), QStringLiteral("edge")}) ||
                !integer(command, QStringLiteral("duration"), 1) || (edge != QLatin1String("left") && edge != QLatin1String("right")))
                return invalid();
            if (qint64(m_timeline->getClipPosition(id)) + command.value(QStringLiteral("duration")).toInt() > std::numeric_limits<int>::max()) return invalid();
            const int duration =
                m_timeline->requestItemResize(id, command.value(QStringLiteral("duration")).toInt(), edge == QLatin1String("right"), true, -1, false);
            if (duration < 0) return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native trim rejected the range, group or collision."));
            return {{"ok", true}, {"clipId", id}, {"actualDuration", duration}};
        }
        return {{"ok", true}, {"clipId", id}};
    }
    return error(QStringLiteral("UNSUPPORTED_COMMAND"), QStringLiteral("Use insert, move, trim, undo or redo."));
}
