/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "livebridge.h"
#include "config-kdenlive.h"

#include "assets/model/assetcommand.hpp"
#include "bin/bin.h"
#include "bin/clipcreator.hpp"
#include "bin/model/markerlistmodel.hpp"
#include "bin/model/subtitlemodel.hpp"
#include "bin/projectclip.h"
#include "bin/projectfolder.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "dialogs/wizard.h"
#include "doc/docundostack.hpp"
#include "doc/kdenlivedoc.h"
#include "doc/kthumb.h"
#include "effects/effectsrepository.hpp"
#include "effects/effectstack/model/effectitemmodel.hpp"
#include "effects/effectstack/model/effectstackmodel.hpp"
#include "kdenlivesettings.h"
#include "macros.hpp"
#include "mainwindow.h"
#include "profiles/profilemodel.hpp"
#include "profiles/profilerepository.hpp"
#include "project/projectmanager.h"
#include "render/renderrequest.h"
#include "render/renderserver.h"
#include "renderpresets/renderpresetmodel.hpp"
#include "renderpresets/renderpresetrepository.hpp"
#include "timeline2/model/compositionmodel.hpp"
#include "timeline2/model/timelinefunctions.hpp"
#include "timeline2/model/timelineitemmodel.hpp"
#include "timeline2/view/timelinecontroller.h"
#include "timeline2/view/timelinewidget.h"

#include <QApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDomDocument>
#include <QProcess>
#ifdef USE_DBUS
#include <QDBusConnection>
#include <QDBusError>
#endif
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QSet>
#include <QStandardPaths>
#include <QTimeZone>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <cmath>
#include <limits>
#include <mlt++/MltProfile.h>
#include <numeric>
#include <tuple>

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

const QStringList &stateSections()
{
    static const QStringList result{QStringLiteral("tracks"),    QStringLiteral("clips"),  QStringLiteral("compositions"), QStringLiteral("markers"),
                                    QStringLiteral("subtitles"), QStringLiteral("bin"),    QStringLiteral("sequences"),    QStringLiteral("effects"),
                                    QStringLiteral("media"),     QStringLiteral("history")};
    return result;
}

const QStringList &defaultStateSections()
{
    static const QStringList result{QStringLiteral("tracks"),  QStringLiteral("clips"), QStringLiteral("compositions"),
                                    QStringLiteral("markers"), QStringLiteral("bin"),   QStringLiteral("sequences")};
    return result;
}
} // namespace

LiveBridge::LiveBridge(QObject *parent)
    : QObject(parent)
{
#ifdef USE_DBUS
    if (qEnvironmentVariableIntValue("KDENLIVE_MCP_BRIDGE") == 1 &&
        !QDBusConnection::sessionBus().registerObject(QStringLiteral("/org/kde/kdenlive/LiveBridge"), this,
                                                      QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals)) {
        qWarning() << "Live bridge registration failed:" << QDBusConnection::sessionBus().lastError().message();
    }
#endif
    // Detect document/sequence switches even when no client is currently calling us.
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] { bind(); });
    timer->start(100);
}

QString LiveBridge::capabilities() const
{
    // Marker categories belong to the open project; they name the values marker commands accept as category.
    QJsonArray categories;
    for (auto it = pCore->markerTypes.cbegin(); it != pCore->markerTypes.cend(); ++it)
        categories.append(QJsonObject{{"index", it.key()}, {"name", it.value().displayName}, {"color", it.value().color.name()}});
    const int defaultCategory = KdenliveSettings::default_marker_type();
    return json({{"ok", true},
                 {"protocolVersion", 1},
                 {"transport", "native-editor"},
                 {"operations", QJsonArray{"import",       "remove_asset",  "remove_clip", "replace_media", "audio_envelope", "rename_track",  "save",
                                           "save_as",      "set_profile",   "insert",      "move",          "trim",           "reframe",       "effect_add",
                                           "effect_set",   "effect_remove", "title_edit",  "marker_add",    "marker_edit",    "marker_remove", "marker_import",
                                           "render",       "batch",         "undo",        "redo",          "split",          "remove_range",  "remove_gap",
                                           "insert_space", "group",         "ungroup",     "speed",         "enable"}},
                 {"insertModes", QJsonArray{"video", "audio"}},
                 {"editModes", QJsonArray{"normal", "overwrite", "insert"}},
                 {"removeModes", QJsonArray{"lift", "extract"}},
                 {"markerCategories", categories},
                 {"defaultMarkerCategory", pCore->markerTypes.contains(defaultCategory) ? QJsonValue(defaultCategory)
                                           : pCore->markerTypes.isEmpty()               ? QJsonValue(QJsonValue::Null)
                                                                                        : QJsonValue(pCore->markerTypes.firstKey())},
                 {"markerFormats", QJsonArray{"json", "csv", "kdenlive"}},
                 {"frameRanges", "sourceOut is exclusive; frames use project FPS"},
                 {"stateScope", "active sequence tracks, clips, compositions, markers, subtitles, project bin and sequence list; not a full project "
                                "interchange format"},
                 {"stateSections", QJsonArray::fromStringList(stateSections())},
                 {"defaultStateSections", QJsonArray::fromStringList(defaultStateSections())},
                 {"receiptLimit", 64},
                 {"batchEditing", true},
                 {"batchLimit", 200}});
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
    attachHistory(document);
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

namespace {
QString clipTypeName(ClipType::ProducerType type)
{
    switch (type) {
    case ClipType::Audio:
        return QStringLiteral("audio");
    case ClipType::Video:
        return QStringLiteral("video");
    case ClipType::AV:
        return QStringLiteral("av");
    case ClipType::Color:
        return QStringLiteral("color");
    case ClipType::Image:
        return QStringLiteral("image");
    case ClipType::Text:
    case ClipType::TextTemplate:
        return QStringLiteral("title");
    case ClipType::SlideShow:
        return QStringLiteral("slideshow");
    case ClipType::Playlist:
        return QStringLiteral("playlist");
    case ClipType::QText:
        return QStringLiteral("text");
    case ClipType::Qml:
    case ClipType::Animation:
        return QStringLiteral("animation");
    case ClipType::Timeline:
        return QStringLiteral("sequence");
    default:
        return QStringLiteral("other");
    }
}

/** Undo text without the hh:mm prefix that pCore->pushUndo and asset commands add. */
QString historyLabel(const QString &text)
{
    static const QRegularExpression prefix(QStringLiteral("^\\d{1,2}:\\d{2} "));
    return QString(text).remove(prefix);
}

QJsonValue optionalId(int id)
{
    return id < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(id);
}

/** The marker shape shared by sequence guides, bin-clip markers and the json export. */
QJsonObject markerJson(const CommentedTime &marker, double fps)
{
    const auto category = pCore->markerTypes.value(marker.markerType());
    return {{"position", marker.time().frames(fps)}, {"duration", marker.duration().frames(fps)}, {"comment", marker.comment()},
            {"category", marker.markerType()},       {"categoryName", category.displayName},      {"color", category.color.name()}};
}

bool overlaps(int position, int duration, int start, int end)
{
    return position < end && position + qMax(duration, 1) > start;
}
} // namespace

const QStringList &LiveBridge::stateSectionNames()
{
    return stateSections();
}

LiveBridge::StateScope LiveBridge::defaultScope()
{
    StateScope scope;
    scope.sections = defaultStateSections();
    return scope;
}

QJsonObject LiveBridge::stateFor(const QJsonObject &arguments)
{
    if (!bind()) return QJsonDocument::fromJson(failure(QStringLiteral("NOT_READY"), QStringLiteral("No fully loaded active timeline.")).toUtf8()).object();
    syncHistory();
    const auto invalid = [] {
        return error(QStringLiteral("INVALID_ARGUMENTS"),
                     QStringLiteral("Expected optional include (section names), trackId and range {start, end} with start < end."));
    };
    if (!keys(arguments, {QStringLiteral("include"), QStringLiteral("trackId"), QStringLiteral("range")})) return invalid();
    StateScope scope = defaultScope();
    if (arguments.contains(QStringLiteral("include"))) {
        if (!arguments.value(QStringLiteral("include")).isArray()) return invalid();
        scope.sections.clear();
        for (const auto &value : arguments.value(QStringLiteral("include")).toArray()) {
            if (!stateSections().contains(value.toString()))
                return error(QStringLiteral("INVALID_ARGUMENTS"),
                             QStringLiteral("Unknown state section. Use: %1.").arg(stateSections().join(QStringLiteral(", "))));
            scope.sections.append(value.toString());
        }
    }
    if (arguments.contains(QStringLiteral("trackId"))) {
        if (!integer(arguments, QStringLiteral("trackId"))) return invalid();
        scope.trackId = arguments.value(QStringLiteral("trackId")).toInt();
        if (!m_timeline->isTrack(scope.trackId)) return error(QStringLiteral("UNKNOWN_TRACK"), QStringLiteral("Track does not exist."));
    }
    if (arguments.contains(QStringLiteral("range"))) {
        const auto range = arguments.value(QStringLiteral("range")).toObject();
        if (!arguments.value(QStringLiteral("range")).isObject() || !keys(range, {QStringLiteral("start"), QStringLiteral("end")}) ||
            !integer(range, QStringLiteral("start")) || !integer(range, QStringLiteral("end"), 1) ||
            range.value(QStringLiteral("start")).toInt() >= range.value(QStringLiteral("end")).toInt())
            return invalid();
        scope.start = range.value(QStringLiteral("start")).toInt();
        scope.end = range.value(QStringLiteral("end")).toInt();
    }
    return snapshot(scope);
}

QJsonObject LiveBridge::snapshot(const StateScope &requested) const
{
    // Dependent sections: clips, compositions and effects live inside tracks, effects inside clips, media inside bin items.
    QStringList sections;
    const auto wants = [&requested](const QString &name) { return requested.sections.contains(name); };
    const bool effects = wants(QStringLiteral("effects"));
    const bool clips = effects || wants(QStringLiteral("clips"));
    const bool compositions = wants(QStringLiteral("compositions"));
    const bool tracksWanted = clips || compositions || wants(QStringLiteral("tracks"));
    const bool media = wants(QStringLiteral("media"));
    const bool binWanted = media || wants(QStringLiteral("bin"));
    for (const auto &name : stateSections()) {
        const bool on = name == QLatin1String("tracks")         ? tracksWanted
                        : name == QLatin1String("clips")        ? clips
                        : name == QLatin1String("bin")          ? binWanted
                        : name == QLatin1String("compositions") ? compositions
                                                                : wants(name);
        if (on) sections.append(name);
    }
    const int start = requested.start;
    const int end = requested.end;
    const double fps = pCore->getCurrentFps();
    auto *widget = pCore->window()->getCurrentTimeline();
    const int activeTrack = widget && m_timeline->isTrack(widget->controller()->activeTrack()) ? widget->controller()->activeTrack() : -1;

    QJsonArray tracks;
    for (int row = 0; tracksWanted && row < m_timeline->rowCount(); ++row) {
        const auto track = m_timeline->index(row, 0);
        const int trackId = int(track.internalId());
        if (requested.trackId >= 0 && trackId != requested.trackId) continue;
        const bool audio = m_timeline->isAudioTrack(trackId);
        const bool disabled = m_timeline->data(track, TimelineModel::IsDisabledRole).toBool();
        QJsonObject entry{{"id", trackId},
                          {"name", m_timeline->data(track, TimelineModel::NameRole).toString()},
                          {"tag", m_timeline->getTrackTagById(trackId)},
                          {"type", audio ? "audio" : "video"},
                          {"audio", audio},
                          {"locked", m_timeline->data(track, TimelineModel::IsLockedRole).toBool()},
                          {"muted", audio && disabled},
                          {"hidden", !audio && disabled},
                          {"active", trackId == activeTrack}};
        QList<QJsonObject> clipList;
        QJsonArray compositionList;
        for (int i = 0; i < m_timeline->rowCount(track); ++i) {
            const auto item = m_timeline->index(i, 0, track);
            const int id = int(item.internalId());
            if (clips && m_timeline->isClip(id)) {
                const auto range = m_timeline->getClipInOut(id);
                const int position = m_timeline->getClipPosition(id);
                const int duration = m_timeline->getClipPlaytime(id);
                const auto state = m_timeline->getClipState(id);
                const int groupId = m_timeline->getItemGroupId(id);
                auto stack = m_timeline->getClipEffectStackModel(id);
                QJsonArray mixes;
                if (const int mix = m_timeline->data(item, TimelineModel::MixRole).toInt(); mix > 0)
                    mixes.append(QJsonObject{
                        {"edge", "start"}, {"position", position}, {"duration", mix}, {"offset", m_timeline->data(item, TimelineModel::MixCutRole).toInt()}});
                if (const int mix = m_timeline->data(item, TimelineModel::MixEndDurationRole).toInt(); mix > 0)
                    mixes.append(QJsonObject{{"edge", "end"}, {"position", position + duration - mix}, {"duration", mix}});
                QJsonObject clip{{"id", id},
                                 {"binId", m_timeline->getClipBinId(id)},
                                 {"name", m_timeline->getClipName(id)},
                                 {"type", clipTypeName(state.second)},
                                 {"position", position},
                                 {"duration", duration},
                                 {"sourceIn", range.first},
                                 {"sourceOut", range.second + 1},
                                 {"speed", m_timeline->getClipSpeed(id)},
                                 {"enabled", state.first != PlaylistState::Disabled},
                                 {"effectCount", stack->rowCount()},
                                 {"grouped", groupId >= 0},
                                 {"groupId", optionalId(groupId)},
                                 {"linkedClipId", optionalId(m_timeline->getClipSplitPartner(id))},
                                 {"mixes", mixes}};
                if (effects) {
                    QJsonArray list;
                    for (int effectRow = 0; effectRow < stack->rowCount(); ++effectRow) {
                        auto effect = std::static_pointer_cast<EffectItemModel>(stack->getEffectStackRow(effectRow));
                        const QString effectId = effect->getAssetId();
                        list.append(QJsonObject{{"effectId", effectId},
                                                {"name", EffectsRepository::get()->exists(effectId) ? EffectsRepository::get()->getName(effectId) : effectId},
                                                {"enabled", effect->isAssetEnabled()}});
                    }
                    clip.insert(QStringLiteral("effects"), list);
                }
                clipList.append(clip);
            } else if (compositions && m_timeline->isComposition(id)) {
                const int position = m_timeline->getCompositionPosition(id);
                const int duration = m_timeline->getCompositionPlaytime(id);
                if (!overlaps(position, duration, start, end)) continue;
                auto composition = std::dynamic_pointer_cast<CompositionModel>(m_timeline->getCompositionParameterModel(id));
                if (!composition) continue;
                // MLT track indexes count the black background track as 0.
                const int aTrack = composition->getATrack();
                const int groupId = m_timeline->getItemGroupId(id);
                compositionList.append(QJsonObject{{"id", id},
                                                   {"compositionId", composition->getAssetId()},
                                                   {"name", composition->displayName()},
                                                   {"position", position},
                                                   {"duration", duration},
                                                   {"aTrack", aTrack},
                                                   {"aTrackId", aTrack > 0 && aTrack <= m_timeline->getTracksCount()
                                                                    ? QJsonValue(m_timeline->getTrackIndexFromPosition(aTrack - 1))
                                                                    : QJsonValue(QJsonValue::Null)},
                                                   {"bTrack", m_timeline->getTrackMltIndex(trackId)},
                                                   {"forcedATrack", composition->getForcedTrack() >= 0},
                                                   {"grouped", groupId >= 0},
                                                   {"groupId", optionalId(groupId)}});
            }
        }
        if (clips) {
            std::sort(clipList.begin(), clipList.end(), [](const QJsonObject &a, const QJsonObject &b) {
                const int left = a.value(QStringLiteral("position")).toInt(), right = b.value(QStringLiteral("position")).toInt();
                return left != right ? left < right : a.value(QStringLiteral("id")).toInt() < b.value(QStringLiteral("id")).toInt();
            });
            QJsonArray clipArray;
            QJsonArray gaps;
            int cursor = 0;
            for (const auto &clip : std::as_const(clipList)) {
                const int position = clip.value(QStringLiteral("position")).toInt();
                const int duration = clip.value(QStringLiteral("duration")).toInt();
                if (position > cursor && overlaps(cursor, position - cursor, start, end))
                    gaps.append(QJsonObject{{"position", cursor}, {"duration", position - cursor}});
                cursor = qMax(cursor, position + duration);
                if (overlaps(position, duration, start, end)) clipArray.append(clip);
            }
            entry.insert(QStringLiteral("clips"), clipArray);
            entry.insert(QStringLiteral("gaps"), gaps);
        }
        if (compositions) entry.insert(QStringLiteral("compositions"), compositionList);
        tracks.append(entry);
    }

    auto stack = m_document->commandStack();
    auto &profile = pCore->getProjectProfile();
    auto guides = m_timeline->getGuideModel();
    auto subtitles = m_timeline->hasSubtitleModel() ? m_timeline->getSubtitleModel() : nullptr;
    const auto binIds = pCore->projectItemModel()->getAllClipIds();
    QJsonObject result{{"ok", true},
                       {"sessionId", m_session},
                       {"revision", m_revision},
                       {"sequenceId", m_timeline->uuid().toString()},
                       {"documentUrl", m_document->url().toString()},
                       {"modified", m_document->isModified()},
                       {"fps", QJsonObject{{"numerator", profile.frame_rate_num()}, {"denominator", profile.frame_rate_den()}}},
                       {"profile", QJsonObject{{"width", profile.width()},
                                               {"height", profile.height()},
                                               {"description", pCore->getCurrentProfile()->description()},
                                               {"duration", pCore->projectDuration()}}},
                       {"playhead", pCore->getMonitorPosition()},
                       {"activeTrackId", optionalId(activeTrack)},
                       {"undo", QJsonObject{{"index", stack->index()},
                                            {"canUndo", stack->canUndo()},
                                            {"canRedo", stack->canRedo()},
                                            {"undoText", stack->undoText()},
                                            {"redoText", stack->redoText()}}},
                       {"sections", QJsonArray::fromStringList(sections)},
                       {"counts", QJsonObject{{"tracks", m_timeline->getTracksCount()},
                                              {"clips", m_timeline->getClipsCount()},
                                              {"compositions", m_timeline->getCompositionsCount()},
                                              {"markers", guides ? guides->rowCount() : 0},
                                              {"subtitles", subtitles ? subtitles->count() : 0},
                                              {"bin", int(binIds.size())}}}};
    if (requested.trackId >= 0 || start > 0 || end < std::numeric_limits<int>::max()) {
        QJsonObject filter;
        if (requested.trackId >= 0) filter.insert(QStringLiteral("trackId"), requested.trackId);
        if (start > 0 || end < std::numeric_limits<int>::max()) filter.insert(QStringLiteral("range"), QJsonObject{{"start", start}, {"end", end}});
        result.insert(QStringLiteral("filter"), filter);
    }
    // The newest MCP-made entry still on the stack, so an agent can confirm what its last call did.
    QJsonValue lastChange = QJsonValue::Null;
    for (int row = qMin(int(m_history.size()), stack->count()) - 1; row >= 0; --row) {
        const auto &entry = m_history.at(row);
        if (entry.origin != QLatin1String("mcp") || entry.command != stack->command(row)) continue;
        lastChange = QJsonObject{{"undoIndex", row + 1},
                                 {"command", entry.type},
                                 {"text", historyLabel(entry.text)},
                                 {"requestId", entry.requestId},
                                 {"applied", row < stack->index()}};
        break;
    }
    result.insert(QStringLiteral("lastChange"), lastChange);
    if (wants(QStringLiteral("history"))) {
        QJsonArray entries;
        for (int row = qMax(0, int(m_history.size()) - 10); row < m_history.size(); ++row)
            entries.append(historyJson(row));
        result.insert(QStringLiteral("history"), entries);
    }
    if (tracksWanted) result.insert(QStringLiteral("tracks"), tracks);
    if (wants(QStringLiteral("markers"))) {
        QJsonArray markers;
        for (const auto &marker : guides ? guides->getAllMarkers() : QList<CommentedTime>()) {
            if (!overlaps(marker.time().frames(fps), marker.duration().frames(fps), start, end)) continue;
            markers.append(markerJson(marker, fps));
        }
        result.insert(QStringLiteral("markers"), markers);
    }
    if (wants(QStringLiteral("subtitles"))) {
        QList<QJsonObject> list;
        if (subtitles)
            for (int id : subtitles->getAllSubIds()) {
                const auto range = subtitles->getInOut(id);
                if (!overlaps(range.first, range.second - range.first, start, end)) continue;
                list.append(QJsonObject{
                    {"id", id}, {"layer", subtitles->getLayerForId(id)}, {"start", range.first}, {"end", range.second}, {"text", subtitles->getText(id)}});
            }
        std::sort(list.begin(), list.end(), [](const QJsonObject &a, const QJsonObject &b) {
            const auto key = [](const QJsonObject &item) {
                return std::make_tuple(item.value(QStringLiteral("start")).toInt(), item.value(QStringLiteral("layer")).toInt(),
                                       item.value(QStringLiteral("id")).toInt());
            };
            return key(a) < key(b);
        });
        QJsonArray array;
        for (const auto &item : std::as_const(list))
            array.append(item);
        result.insert(QStringLiteral("subtitles"), array);
    }
    if (binWanted) {
        const auto parentOf = [](const std::shared_ptr<AbstractProjectItem> &item) -> QJsonValue {
            const auto parent = item->parent();
            if (!parent || parent->clipId() == QLatin1String("-1")) return QJsonValue::Null;
            return parent->clipId();
        };
        QJsonArray bin;
        for (const auto &id : binIds) {
            auto clip = pCore->projectItemModel()->getClipByBinID(id);
            if (!clip) continue;
            const bool ready = clip->statusReady();
            const auto type = clip->clipType();
            // A loading producer holds its write lock until its GUI-thread completion.
            // Reading its duration or media properties here would prevent that completion from running.
            QJsonObject item{{"id", id},
                             {"name", clip->clipName()},
                             {"type", clipTypeName(type)},
                             {"parentId", parentOf(clip)},
                             // Generated clips have no media file; their resource is not a path.
                             {"url", type == ClipType::Color || type == ClipType::Timeline ? QString() : clip->url()},
                             {"ready", ready},
                             {"inUse", clip->isIncludedInTimeline()},
                             {"duration", ready ? QJsonValue(qint64(clip->frameDuration())) : QJsonValue::Null}};
            if (media && ready) {
                const QSize size = clip->getFrameSize();
                const double clipFps = clip->getOriginalFps();
                const bool sized = size.width() > 0 && size.height() > 0;
                item.insert(QStringLiteral("hasVideo"), clip->hasVideo());
                item.insert(QStringLiteral("hasAudio"), clip->hasAudio());
                item.insert(QStringLiteral("width"), sized ? QJsonValue(size.width()) : QJsonValue(QJsonValue::Null));
                item.insert(QStringLiteral("height"), sized ? QJsonValue(size.height()) : QJsonValue(QJsonValue::Null));
                item.insert(QStringLiteral("fps"), clipFps > 0 ? QJsonValue(clipFps) : QJsonValue(QJsonValue::Null));
            }
            // Clip markers live in their own model, not in the producer, so a loading clip can report them too.
            // A sequence clip's marker model is that sequence's guides model, already reported as markers.
            if (auto clipMarkers = clip->getMarkerModel(); type != ClipType::Timeline && clipMarkers && clipMarkers->rowCount() > 0) {
                QJsonArray list;
                for (const auto &marker : clipMarkers->getAllMarkers())
                    list.append(markerJson(marker, fps));
                item.insert(QStringLiteral("markers"), list);
            }
            bin.append(item);
        }
        QJsonArray folders;
        std::function<void(const std::shared_ptr<TreeItem> &)> walk = [&](const std::shared_ptr<TreeItem> &node) {
            for (int i = 0; i < node->childCount(); ++i) {
                auto child = std::static_pointer_cast<AbstractProjectItem>(node->child(i));
                if (child->itemType() != AbstractProjectItem::FolderItem) continue;
                folders.append(QJsonObject{{"id", child->clipId()}, {"name", child->name()}, {"parentId", parentOf(child)}});
                walk(child);
            }
        };
        walk(pCore->projectItemModel()->getRootFolder());
        result.insert(QStringLiteral("bin"), bin);
        result.insert(QStringLiteral("folders"), folders);
    }
    if (wants(QStringLiteral("sequences"))) {
        QJsonArray sequences;
        const auto all = pCore->projectItemModel()->getAllSequenceClips();
        for (auto it = all.cbegin(); it != all.cend(); ++it) {
            auto clip = pCore->projectItemModel()->getClipByBinID(it.value());
            sequences.append(QJsonObject{{"id", it.key().toString()},
                                         {"binId", it.value()},
                                         {"name", clip ? clip->clipName() : QString()},
                                         {"active", it.key() == m_timeline->uuid()},
                                         {"open", m_document->getTimeline(it.key(), true) != nullptr}});
        }
        result.insert(QStringLiteral("sequences"), sequences);
    }
    return result;
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
    syncHistory();
    return json(snapshot(defaultScope()));
}

QString LiveBridge::apply(const QString &request)
{
    return applyAuthorized(request, {}, {{}, QStringLiteral("dbus"), QStringLiteral("D-Bus")});
}

QString LiveBridge::applyAuthorized(const QString &request, const std::function<QJsonObject()> &authorize, const Caller &caller)
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
    if (authorize) {
        const auto rejection = authorize();
        if (!rejection.isEmpty()) return json(rejection);
    }
    QScopedValueRollback<bool> executing(m_executing, true);
    const auto command = object.value(QStringLiteral("command")).toObject();
    syncHistory();
    Request context{requestId, command.value(QStringLiteral("type")).toString(), caller, m_revision, {}};
    QScopedValueRollback<Request *> current(m_request, &context);
    // Insert and overwrite are explicit per command; the GUI's edit mode toggle must not change what a command does.
    const auto editMode = m_timeline->editMode();
    m_timeline->setEditMode(TimelineMode::NormalEdit);
    auto result = execute(command);
    if (m_timeline) m_timeline->setEditMode(editMode);
    const bool succeeded = result.value(QStringLiteral("ok")).toBool();
    // Even a successful no-op gets a fresh revision, preventing replay after receipt eviction.
    if (succeeded) contentChanged();
    finishRequest(command, result);
    // historySummary is for the change log only.
    result.remove(QStringLiteral("historySummary"));
    if (result.value(QStringLiteral("results")).isArray()) {
        QJsonArray results;
        for (const auto &item : result.value(QStringLiteral("results")).toArray()) {
            auto inner = item.toObject();
            inner.remove(QStringLiteral("historySummary"));
            results.append(inner);
        }
        result.insert(QStringLiteral("results"), results);
    }
    if (succeeded) {
        pCore->refreshProjectMonitorOnce();
        result.insert(QStringLiteral("state"), snapshot(defaultScope()));
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
    if (type == QLatin1String("batch")) return executeBatch(command);
    bool handled = false;
    auto production = executeProduction(command, handled);
    if (handled) return production;
    if (type.startsWith(QLatin1String("marker_"))) return executeMarker(command);
    auto timeline = executeTimeline(command, handled);
    if (handled) return timeline;
    if (type == QLatin1String("remove_asset")) {
        if (!keys(command, {QStringLiteral("type"), QStringLiteral("binId")})) return invalid();
        const QString id = command.value(QStringLiteral("binId")).toString();
        if (!QRegularExpression(QStringLiteral("^[0-9]+$")).match(id).hasMatch()) return invalid();
        auto clip = pCore->projectItemModel()->getClipByBinID(id);
        if (!clip || !clip->statusReady()) return error(QStringLiteral("MEDIA_NOT_READY"), QStringLiteral("Bin clip is missing or still loading."));
        if (clip->clipType() == ClipType::Timeline)
            return error(QStringLiteral("SEQUENCE_PROTECTED"), QStringLiteral("This command removes media assets, not sequences."));
        if (clip->isIncludedInTimeline())
            return error(QStringLiteral("ASSET_IN_USE"), QStringLiteral("Remove timeline instances first. The asset is still used in a sequence."));
        Fun undo = [] { return true; };
        Fun redo = [] { return true; };
        if (!pCore->projectItemModel()->requestBinClipDeletion(clip, undo, redo))
            return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native asset removal was rejected."));
        pCore->pushUndo(undo, redo, QStringLiteral("Remove media asset"));
        return {{"ok", true}};
    }
    if (type == QLatin1String("rename_track")) {
        if (!keys(command, {QStringLiteral("type"), QStringLiteral("trackId"), QStringLiteral("name")}) || !integer(command, QStringLiteral("trackId")) ||
            !command.value(QStringLiteral("name")).isString())
            return invalid();
        const int id = command.value(QStringLiteral("trackId")).toInt();
        const QString name = command.value(QStringLiteral("name")).toString();
        if (!m_timeline->isTrack(id)) return error(QStringLiteral("UNKNOWN_TRACK"), QStringLiteral("Track does not exist."));
        if (name.trimmed().isEmpty() || name.size() > 256) return invalid();
        m_timeline->setTrackName(id, name);
        return {{"ok", true}};
    }
    if (type == QLatin1String("audio_envelope")) {
        if (!keys(command, {QStringLiteral("type"), QStringLiteral("clipId"), QStringLiteral("fadeIn"), QStringLiteral("fadeOut"), QStringLiteral("gainDb")}) ||
            !integer(command, QStringLiteral("clipId")) || !integer(command, QStringLiteral("fadeIn")) || !integer(command, QStringLiteral("fadeOut")))
            return invalid();
        const int id = command.value(QStringLiteral("clipId")).toInt();
        if (!m_timeline->isClip(id)) return error(QStringLiteral("UNKNOWN_CLIP"), QStringLiteral("Timeline clip does not exist."));
        const int trackId = m_timeline->getClipTrackId(id);
        if (!m_timeline->isAudioTrack(trackId))
            return error(QStringLiteral("INCOMPATIBLE_MEDIA"), QStringLiteral("Audio envelope requires a clip on an audio track."));
        if (m_timeline->data(m_timeline->makeTrackIndexFromID(trackId), TimelineModel::IsLockedRole).toBool())
            return error(QStringLiteral("TRACK_LOCKED"), QStringLiteral("Track is locked."));
        const auto gain = command.value(QStringLiteral("gainDb"));
        if (!gain.isDouble() || !std::isfinite(gain.toDouble()) || gain.toDouble() < -60 || gain.toDouble() > 0) return invalid();
        const int fadeIn = command.value(QStringLiteral("fadeIn")).toInt();
        const int fadeOut = command.value(QStringLiteral("fadeOut")).toInt();
        if (qint64(fadeIn) + fadeOut >= m_timeline->getClipPlaytime(id)) return invalid();
        auto effects = m_timeline->getClipEffectStackModel(id);
        if (effects->hasFilter(QStringLiteral("volume")))
            return error(QStringLiteral("EFFECT_EXISTS"), QStringLiteral("Clip already has a volume effect; undo it before adding a new envelope."));
        const auto range = m_timeline->getClipInOut(id);
        const QString gainText = QString::number(gain.toDouble(), 'g', 15);
        QStringList levels;
        if (fadeIn > 0) levels << QStringLiteral("%1=-60").arg(range.first);
        levels << QStringLiteral("%1=%2").arg(range.first + fadeIn).arg(gainText);
        if (fadeOut > 0) {
            levels << QStringLiteral("%1=%2").arg(range.second - fadeOut).arg(gainText);
            levels << QStringLiteral("%1=-60").arg(range.second);
        }
        if (!effects->appendEffect(QStringLiteral("volume"), false, {{QStringLiteral("level"), levels.join(QLatin1Char(';'))}}))
            return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native audio envelope was rejected."));
        return {{"ok", true}, {"clipId", id}};
    }
    if (type == QLatin1String("import")) {
        if (!keys(command, {QStringLiteral("type"), QStringLiteral("path")})) return invalid();
        const QFileInfo file(command.value(QStringLiteral("path")).toString());
        if (!file.isAbsolute() || !file.isFile() || !file.isReadable())
            return error(QStringLiteral("INVALID_MEDIA"), QStringLiteral("Import requires a readable absolute local file path."));
        auto model = pCore->projectItemModel();
        const QString path = file.canonicalFilePath();
        if (path == QFileInfo(m_document->url().toLocalFile()).canonicalFilePath())
            return error(QStringLiteral("INVALID_MEDIA"), QStringLiteral("A project cannot import itself."));
        const auto existing = model->getClipByUrl(QFileInfo(path));
        if (!existing.isEmpty()) return {{"ok", true}, {"binId", existing.first()}, {"existing", true}};
        Fun undo = [] { return true; };
        Fun redo = [] { return true; };
        const QString id = ClipCreator::createClipFromFile(path, QStringLiteral("-1"), model, undo, redo);
        if (id == QLatin1String("-1")) return error(QStringLiteral("IMPORT_REJECTED"), QStringLiteral("Native media import rejected the file."));
        pCore->pushUndo(undo, redo, QStringLiteral("Import media"));
        return {{"ok", true}, {"binId", id}, {"existing", false}};
    }
    if (type == QLatin1String("replace_media")) {
        if (!keys(command, {QStringLiteral("type"), QStringLiteral("binId"), QStringLiteral("replacementBinId")})) return invalid();
        const auto binId = command.value(QStringLiteral("binId")).toString();
        const auto replacementId = command.value(QStringLiteral("replacementBinId")).toString();
        if (!QRegularExpression(QStringLiteral("^[0-9]+$")).match(binId).hasMatch() ||
            !QRegularExpression(QStringLiteral("^[0-9]+$")).match(replacementId).hasMatch())
            return invalid();
        auto original = pCore->projectItemModel()->getClipByBinID(binId);
        auto replacement = pCore->projectItemModel()->getClipByBinID(replacementId);
        if (!original || !replacement || !original->statusReady() || !replacement->statusReady())
            return error(QStringLiteral("MEDIA_NOT_READY"), QStringLiteral("Both bin clips must be loaded before replacement."));
        const QFileInfo file(replacement->url());
        if (!file.isAbsolute() || !file.isFile() || !file.isReadable() || !original->hasLimitedDuration() || !replacement->hasLimitedDuration() ||
            original->hasAudio() != replacement->hasAudio() || original->hasVideo() != replacement->hasVideo())
            return error(QStringLiteral("INCOMPATIBLE_MEDIA"), QStringLiteral("Replacement requires local media with matching audio/video streams."));
        // Native Replace Clip otherwise prompts for shorter media. Reject before invoking it.
        if (replacement->frameDuration() < original->frameDuration())
            return error(QStringLiteral("MEDIA_TOO_SHORT"), QStringLiteral("Replacement must cover the original media duration."));
        if (binId == replacementId || original->url() == replacement->url()) return {{"ok", true}, {"binId", binId}};
        const int before = stack->index();
        pCore->bin()->replaceSingleClip(binId, file.canonicalFilePath());
        if (stack->index() == before) return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native media replacement was rejected."));
        return {{"ok", true}, {"binId", binId}};
    }
    if (type == QLatin1String("save")) {
        if (!keys(command, {QStringLiteral("type")})) return invalid();
        const QFileInfo file(m_document->url().toLocalFile());
        if (!m_document->url().isLocalFile() || !file.isFile() || !file.isWritable())
            return error(QStringLiteral("SAVE_REQUIRED"), QStringLiteral("Save the project to a writable local file first."));
        for (const auto &id : pCore->projectItemModel()->getAllClipIds()) {
            auto clip = pCore->projectItemModel()->getClipByBinID(id);
            if (clip && !clip->statusReady())
                return error(QStringLiteral("MEDIA_NOT_READY"), QStringLiteral("Wait for bin media to finish loading before saving."));
        }
        if (!pCore->projectManager()->saveFile()) return error(QStringLiteral("SAVE_FAILED"), QStringLiteral("Native project save failed."));
        return {{"ok", true}};
    }
    if (type == QLatin1String("undo") || type == QLatin1String("redo")) return executeHistoryStep(command);
    if (type == QLatin1String("move")) {
        if (!integer(command, QStringLiteral("trackId")) || !integer(command, QStringLiteral("position"))) return invalid();
        const int trackId = command.value(QStringLiteral("trackId")).toInt();
        if (!m_timeline->isTrack(trackId)) return error(QStringLiteral("UNKNOWN_TRACK"), QStringLiteral("Track does not exist."));
        if (m_timeline->data(m_timeline->makeTrackIndexFromID(trackId), TimelineModel::IsLockedRole).toBool())
            return error(QStringLiteral("TRACK_LOCKED"), QStringLiteral("Target track is locked."));
    }
    if (type == QLatin1String("move") || type == QLatin1String("trim")) {
        if (!integer(command, QStringLiteral("clipId"))) return invalid();
        const int id = command.value(QStringLiteral("clipId")).toInt();
        if (!m_timeline->isClip(id)) return error(QStringLiteral("UNKNOWN_CLIP"), QStringLiteral("Timeline clip does not exist."));
        const int sourceTrack = m_timeline->getClipTrackId(id);
        if (m_timeline->data(m_timeline->makeTrackIndexFromID(sourceTrack), TimelineModel::IsLockedRole).toBool())
            return error(QStringLiteral("TRACK_LOCKED"), QStringLiteral("Source track is locked."));
        // A GUI selection groups its clips; it must not widen the edit.
        m_timeline->requestClearSelection();
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
    return error(QStringLiteral("UNSUPPORTED_COMMAND"), QStringLiteral("Read capabilities() for supported commands."));
}

QJsonObject LiveBridge::editableClip(int clipId) const
{
    if (!m_timeline->isClip(clipId)) return error(QStringLiteral("UNKNOWN_CLIP"), QStringLiteral("Timeline clip does not exist."));
    if (m_timeline->data(m_timeline->makeTrackIndexFromID(m_timeline->getClipTrackId(clipId)), TimelineModel::IsLockedRole).toBool())
        return error(QStringLiteral("TRACK_LOCKED"), QStringLiteral("Track is locked."));
    return {};
}

namespace {
const QStringList batchable{QStringLiteral("import"),        QStringLiteral("remove_asset"), QStringLiteral("remove_clip"),  QStringLiteral("audio_envelope"),
                            QStringLiteral("rename_track"),  QStringLiteral("insert"),       QStringLiteral("move"),         QStringLiteral("trim"),
                            QStringLiteral("reframe"),       QStringLiteral("effect_add"),   QStringLiteral("effect_set"),   QStringLiteral("effect_remove"),
                            QStringLiteral("title_edit"),    QStringLiteral("marker_add"),   QStringLiteral("marker_edit"),  QStringLiteral("marker_remove"),
                            QStringLiteral("marker_import"), QStringLiteral("split"),        QStringLiteral("remove_range"), QStringLiteral("remove_gap"),
                            QStringLiteral("insert_space"),  QStringLiteral("group"),        QStringLiteral("ungroup"),      QStringLiteral("speed"),
                            QStringLiteral("enable")};

bool unitInterval(const QJsonObject &object, const QString &key)
{
    if (!object.contains(key)) return true;
    const auto value = object.value(key);
    return value.isDouble() && value.toDouble() >= 0 && value.toDouble() <= 1;
}

std::shared_ptr<EffectItemModel> effectAt(const std::shared_ptr<EffectStackModel> &stack, int row)
{
    if (row < 0 || row >= stack->rowCount()) return nullptr;
    return std::static_pointer_cast<EffectItemModel>(stack->getEffectStackRow(row));
}

QJsonObject stringParameters(const QJsonValue &value, stringMap &result)
{
    if (!value.isObject()) return error(QStringLiteral("INVALID_COMMAND"), QStringLiteral("params must be an object of strings."));
    const auto params = value.toObject();
    if (params.size() > 64) return error(QStringLiteral("INVALID_COMMAND"), QStringLiteral("At most 64 parameters per call."));
    for (auto it = params.begin(); it != params.end(); ++it) {
        if (!it.value().isString() || it.key().isEmpty() || it.value().toString().size() > 16384)
            return error(QStringLiteral("INVALID_COMMAND"), QStringLiteral("Parameter values must be MLT strings."));
        result.insert(it.key(), it.value().toString());
    }
    return {};
}

/** Resolves exactly one of clipId (timeline clip) or binId (bin clip, shared by all its instances). */
QJsonObject effectTarget(const QJsonObject &command, TimelineItemModel *timeline, std::shared_ptr<EffectStackModel> &stack, int &origin)
{
    const bool clip = command.contains(QStringLiteral("clipId"));
    if (clip == command.contains(QStringLiteral("binId")))
        return error(QStringLiteral("INVALID_COMMAND"), QStringLiteral("Pass exactly one of clipId or binId."));
    if (clip) {
        if (!integer(command, QStringLiteral("clipId"))) return error(QStringLiteral("INVALID_COMMAND"), QStringLiteral("Invalid clipId."));
        const int id = command.value(QStringLiteral("clipId")).toInt();
        if (!timeline->isClip(id)) return error(QStringLiteral("UNKNOWN_CLIP"), QStringLiteral("Timeline clip does not exist."));
        if (timeline->data(timeline->makeTrackIndexFromID(timeline->getClipTrackId(id)), TimelineModel::IsLockedRole).toBool())
            return error(QStringLiteral("TRACK_LOCKED"), QStringLiteral("Track is locked."));
        stack = timeline->getClipEffectStackModel(id);
        origin = timeline->getClipInOut(id).first;
        return {};
    }
    auto binClip = pCore->projectItemModel()->getClipByBinID(command.value(QStringLiteral("binId")).toString());
    if (!binClip || !binClip->statusReady() || binClip->clipType() == ClipType::Timeline)
        return error(QStringLiteral("MEDIA_NOT_READY"), QStringLiteral("binId must be a loaded media clip."));
    stack = binClip->getEffectStack();
    origin = 0;
    return stack ? QJsonObject{} : error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Bin clip has no effect stack."));
}

QJsonObject effectJson(int row, const std::shared_ptr<EffectItemModel> &effect)
{
    QJsonObject params;
    for (const auto &param : effect->getAllParameters())
        params.insert(param.first, param.second.toString());
    const QString id = effect->getAssetId();
    return {{"index", row},
            {"effectId", id},
            {"name", EffectsRepository::get()->exists(id) ? EffectsRepository::get()->getName(id) : id},
            {"enabled", effect->isAssetEnabled()},
            {"builtIn", effect->isBuiltIn()},
            {"params", params}};
}

QString rect(double x, double y, double w, double h)
{
    return QStringLiteral("%1 %2 %3 %4 1").arg(qRound(x)).arg(qRound(y)).arg(qRound(w)).arg(qRound(h));
}
} // namespace

QJsonObject LiveBridge::executeBatch(const QJsonObject &command)
{
    const auto list = command.value(QStringLiteral("commands"));
    if (!keys(command, {QStringLiteral("type"), QStringLiteral("commands")}) || !list.isArray() || list.toArray().isEmpty() || list.toArray().size() > 200)
        return error(QStringLiteral("INVALID_COMMAND"), QStringLiteral("batch requires 1 to 200 commands."));
    const auto commands = list.toArray();
    for (const auto &item : commands) {
        // Commands that save, render, change the profile or detect rejection through the Undo index cannot nest in one Undo entry.
        if (!item.isObject() || !batchable.contains(item.toObject().value(QStringLiteral("type")).toString()))
            return error(QStringLiteral("INVALID_COMMAND"), QStringLiteral("batch accepts only: %1.").arg(batchable.join(QStringLiteral(", "))));
    }
    auto stack = m_document->commandStack();
    const int before = stack->index();
    QJsonArray results;
    QJsonObject failed;
    int failedIndex = -1;
    stack->beginMacro(QStringLiteral("MCP batch (%1 edits)").arg(commands.size()));
    for (int i = 0; i < commands.size(); ++i) {
        const auto result = execute(commands.at(i).toObject());
        if (!result.value(QStringLiteral("ok")).toBool()) {
            failed = result;
            failedIndex = i;
            break;
        }
        results.append(result);
    }
    stack->endMacro();
    if (failedIndex >= 0) {
        // Roll back the partial macro so the batch is all-or-nothing, and drop it so Redo cannot re-apply half a batch.
        if (stack->index() > before) {
            auto *macro = const_cast<QUndoCommand *>(stack->command(stack->index() - 1));
            macro->undo();
            // QUndoStack deletes an obsolete command instead of undoing it again.
            macro->setObsolete(true);
            stack->undo();
        }
        failed.insert(QStringLiteral("failedIndex"), failedIndex);
        return failed;
    }
    return {{"ok", true}, {"results", results}};
}

QJsonObject LiveBridge::executeProduction(const QJsonObject &command, bool &handled)
{
    handled = true;
    const QString type = command.value(QStringLiteral("type")).toString();
    const auto invalid = [] { return error(QStringLiteral("INVALID_COMMAND"), QStringLiteral("Invalid command fields or frame range.")); };
    if (type == QLatin1String("save_as")) {
        if (!keys(command, {QStringLiteral("type"), QStringLiteral("path")})) return invalid();
        const QFileInfo file(command.value(QStringLiteral("path")).toString());
        if (!file.isAbsolute() || file.suffix() != QLatin1String("kdenlive") || file.exists() || !QFileInfo(file.absolutePath()).isDir() ||
            !QFileInfo(file.absolutePath()).isWritable())
            return error(QStringLiteral("INVALID_PATH"), QStringLiteral("save_as needs a new absolute .kdenlive path in a writable folder."));
        for (const auto &id : pCore->projectItemModel()->getAllClipIds()) {
            auto clip = pCore->projectItemModel()->getClipByBinID(id);
            if (clip && !clip->statusReady())
                return error(QStringLiteral("MEDIA_NOT_READY"), QStringLiteral("Wait for bin media to finish loading before saving."));
        }
        if (!pCore->projectManager()->saveFileAs(file.absoluteFilePath(), true, false))
            return error(QStringLiteral("SAVE_FAILED"), QStringLiteral("Native project save failed."));
        return {{"ok", true}, {"documentUrl", m_document->url().toString()}};
    }
    if (type == QLatin1String("set_profile")) {
        if (!keys(command, {QStringLiteral("type"), QStringLiteral("width"), QStringLiteral("height")}) || !integer(command, QStringLiteral("width"), 16) ||
            !integer(command, QStringLiteral("height"), 16))
            return invalid();
        const int width = command.value(QStringLiteral("width")).toInt();
        const int height = command.value(QStringLiteral("height")).toInt();
        if (width > 8192 || height > 8192 || width % 2 || height % 2)
            return error(QStringLiteral("INVALID_PROFILE"), QStringLiteral("Width and height must be even and at most 8192."));
        auto &current = pCore->getProjectProfile();
        if (current.width() == width && current.height() == height) return {{"ok", true}, {"changed", false}};
        const int divisor = std::gcd(width, height);
        // Keep the frame rate: a different rate would need Kdenlive's save-and-reload conversion.
        ProfileParam wanted(width, height, current.frame_rate_num(), current.frame_rate_den(), width / divisor, height / divisor, 1, 1, current.colorspace(),
                            !current.progressive());
        QString path = ProfileRepository::get()->findMatchingProfile(&wanted);
        if (path.isEmpty()) {
            wanted.m_description = QStringLiteral("%1x%2 %3 fps").arg(width).arg(height).arg(QString::number(current.fps(), 'f', 2));
            path = ProfileRepository::get()->saveProfile(&wanted);
        }
        if (path.isEmpty()) return error(QStringLiteral("INVALID_PROFILE"), QStringLiteral("Cannot create a matching project profile."));
        const bool darChanged = !qFuzzyCompare(pCore->getCurrentProfile()->dar(), ProfileRepository::get()->getProfile(path)->dar());
        if (!pCore->setCurrentProfile(path)) return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native profile switch was rejected."));
        pCore->projectManager()->slotResetProfiles(darChanged);
        m_document->setModified(true);
        return {{"ok", true}, {"changed", true}, {"profile", pCore->getCurrentProfile()->description()}};
    }
    if (type == QLatin1String("reframe")) {
        if (!keys(command, {QStringLiteral("type"), QStringLiteral("clipId"), QStringLiteral("mode"), QStringLiteral("focusX"), QStringLiteral("focusY"),
                            QStringLiteral("endFocusX"), QStringLiteral("endFocusY"), QStringLiteral("zoom")}) ||
            !integer(command, QStringLiteral("clipId")) || !unitInterval(command, QStringLiteral("focusX")) ||
            !unitInterval(command, QStringLiteral("focusY")) || !unitInterval(command, QStringLiteral("endFocusX")) ||
            !unitInterval(command, QStringLiteral("endFocusY")))
            return invalid();
        const QString mode = command.value(QStringLiteral("mode")).toString();
        const double zoom = command.value(QStringLiteral("zoom")).toDouble(1);
        if ((mode != QLatin1String("fill") && mode != QLatin1String("fit")) ||
            (command.contains(QStringLiteral("zoom")) && !command.value(QStringLiteral("zoom")).isDouble()) || zoom < 0.1 || zoom > 10)
            return invalid();
        const int id = command.value(QStringLiteral("clipId")).toInt();
        if (auto rejected = editableClip(id); !rejected.isEmpty()) return rejected;
        if (m_timeline->isAudioTrack(m_timeline->getClipTrackId(id)))
            return error(QStringLiteral("INCOMPATIBLE_MEDIA"), QStringLiteral("Reframing requires a clip on a video track."));
        const QSize source = m_timeline->getClipFrameSize(id);
        if (source.width() <= 0 || source.height() <= 0) return error(QStringLiteral("MEDIA_NOT_READY"), QStringLiteral("Clip frame size is unknown."));
        const QSize frame = pCore->getCurrentFrameSize();
        const double scale =
            zoom * (mode == QLatin1String("fill") ? std::max(double(frame.width()) / source.width(), double(frame.height()) / source.height())
                                                  : std::min(double(frame.width()) / source.width(), double(frame.height()) / source.height()));
        const double w = source.width() * scale;
        const double h = source.height() * scale;
        // Focus 0 aligns the source's left/top edge with the frame, 1 its right/bottom edge.
        const auto place = [&](double fx, double fy) { return rect((frame.width() - w) * fx, (frame.height() - h) * fy, w, h); };
        const double fx = command.value(QStringLiteral("focusX")).toDouble(0.5);
        const double fy = command.value(QStringLiteral("focusY")).toDouble(0.5);
        const auto range = m_timeline->getClipInOut(id);
        // Clip effect keyframes use source frame numbers.
        QString animation = QStringLiteral("%1=%2").arg(range.first).arg(place(fx, fy));
        if (command.contains(QStringLiteral("endFocusX")) || command.contains(QStringLiteral("endFocusY"))) {
            animation += QStringLiteral(";%1=%2")
                             .arg(range.second)
                             .arg(place(command.value(QStringLiteral("endFocusX")).toDouble(fx), command.value(QStringLiteral("endFocusY")).toDouble(fy)));
        }
        auto stack = m_timeline->getClipEffectStackModel(id);
        for (int row = 0; row < stack->rowCount(); ++row) {
            auto effect = effectAt(stack, row);
            if (effect && !effect->isBuiltIn() && effect->getAssetId() == QLatin1String("qtblend")) {
                pCore->pushUndo(new AssetUpdateCommand(effect, {{QStringLiteral("rect"), animation}, {QStringLiteral("distort"), QStringLiteral("0")}}));
                return {{"ok", true}, {"clipId", id}, {"effectIndex", row}, {"rect", animation}};
            }
        }
        if (!stack->appendEffect(QStringLiteral("qtblend"), false, {{QStringLiteral("rect"), animation}, {QStringLiteral("distort"), QStringLiteral("0")}}))
            return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native Transform effect was rejected."));
        return {{"ok", true}, {"clipId", id}, {"effectIndex", stack->rowCount() - 1}, {"rect", animation}};
    }
    if (type == QLatin1String("effect_add") || type == QLatin1String("effect_set") || type == QLatin1String("effect_remove")) {
        QStringList allowed{QStringLiteral("type"), QStringLiteral("clipId"), QStringLiteral("binId")};
        allowed << (type == QLatin1String("effect_add")   ? QStringList{QStringLiteral("effectId"), QStringLiteral("params")}
                    : type == QLatin1String("effect_set") ? QStringList{QStringLiteral("index"), QStringLiteral("params")}
                                                          : QStringList{QStringLiteral("index")});
        if (!keys(command, allowed) || (type != QLatin1String("effect_add") && !integer(command, QStringLiteral("index")))) return invalid();
        std::shared_ptr<EffectStackModel> stack;
        int origin = 0;
        if (auto rejected = effectTarget(command, m_timeline, stack, origin); !rejected.isEmpty()) return rejected;
        const QJsonValue id = command.contains(QStringLiteral("clipId")) ? command.value(QStringLiteral("clipId")) : command.value(QStringLiteral("binId"));
        stringMap params;
        if (type != QLatin1String("effect_remove") && (type == QLatin1String("effect_set") || command.contains(QStringLiteral("params")))) {
            if (auto rejected = stringParameters(command.value(QStringLiteral("params")), params); !rejected.isEmpty()) return rejected;
        }
        if (type == QLatin1String("effect_add")) {
            const QString effectId = command.value(QStringLiteral("effectId")).toString();
            if (!EffectsRepository::get()->exists(effectId) || EffectsRepository::get()->getType(effectId) == AssetListType::AssetType::Hidden)
                return error(QStringLiteral("UNKNOWN_EFFECT"), QStringLiteral("Unknown effect id. Use desktop_effect_list to search the catalog."));
            const int rows = stack->rowCount();
            if (!stack->appendEffect(effectId, false, params) || stack->rowCount() == rows)
                return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native effect insertion was rejected (track type, uniqueness or master-only)."));
            return {{"ok", true}, {"target", id}, {"effectIndex", stack->rowCount() - 1}};
        }
        auto effect = effectAt(stack, command.value(QStringLiteral("index")).toInt());
        if (!effect) return error(QStringLiteral("UNKNOWN_EFFECT"), QStringLiteral("No effect at this index; read desktop_effect_list."));
        if (effect->isBuiltIn()) return error(QStringLiteral("EFFECT_PROTECTED"), QStringLiteral("Built-in effects cannot be edited through MCP."));
        if (type == QLatin1String("effect_remove")) {
            stack->removeEffect(effect);
            return {{"ok", true}, {"target", id}};
        }
        QStringList known;
        for (const auto &param : effect->getAllParameters())
            known << param.first;
        QVector<QPair<QString, QVariant>> update;
        for (auto it = params.cbegin(); it != params.cend(); ++it) {
            if (!known.contains(it.key()))
                return error(QStringLiteral("UNKNOWN_PARAMETER"), QStringLiteral("Effect has no parameter %1; read desktop_effect_list.").arg(it.key()));
            update.append({it.key(), it.value()});
        }
        if (update.isEmpty()) return invalid();
        pCore->pushUndo(new AssetUpdateCommand(effect, update));
        return {{"ok", true}, {"target", id}, {"effectIndex", command.value(QStringLiteral("index")).toInt()}};
    }
    if (type == QLatin1String("title_edit")) {
        if (!keys(command, {QStringLiteral("type"), QStringLiteral("binId"), QStringLiteral("width"), QStringLiteral("height"), QStringLiteral("items")}) ||
            (command.contains(QStringLiteral("width")) && !integer(command, QStringLiteral("width"), 16)) ||
            (command.contains(QStringLiteral("height")) && !integer(command, QStringLiteral("height"), 16)) ||
            (command.contains(QStringLiteral("items")) && !command.value(QStringLiteral("items")).isArray()))
            return invalid();
        const QString binId = command.value(QStringLiteral("binId")).toString();
        auto clip = pCore->projectItemModel()->getClipByBinID(binId);
        if (!clip || !clip->statusReady() || clip->clipType() != ClipType::Text)
            return error(QStringLiteral("NOT_A_TITLE"), QStringLiteral("binId must be a loaded title clip."));
        QDomDocument xml;
        if (!xml.setContent(clip->getProducerProperty(QStringLiteral("xmldata"))))
            return error(QStringLiteral("INVALID_TITLE"), QStringLiteral("Title XML cannot be parsed."));
        auto root = xml.documentElement();
        if (command.contains(QStringLiteral("width"))) root.setAttribute(QStringLiteral("width"), command.value(QStringLiteral("width")).toInt());
        if (command.contains(QStringLiteral("height"))) root.setAttribute(QStringLiteral("height"), command.value(QStringLiteral("height")).toInt());
        const auto items = root.elementsByTagName(QStringLiteral("item"));
        for (const auto &value : command.value(QStringLiteral("items")).toArray()) {
            const auto edit = value.toObject();
            if (!value.isObject() ||
                !keys(edit, {QStringLiteral("index"), QStringLiteral("text"), QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("fontPixelSize"),
                             QStringLiteral("alignment")}) ||
                !integer(edit, QStringLiteral("index")) || edit.value(QStringLiteral("index")).toInt() >= items.count() ||
                (edit.contains(QStringLiteral("text")) && !edit.value(QStringLiteral("text")).isString()) ||
                (edit.contains(QStringLiteral("x")) && !edit.value(QStringLiteral("x")).isDouble()) ||
                (edit.contains(QStringLiteral("y")) && !edit.value(QStringLiteral("y")).isDouble()) ||
                (edit.contains(QStringLiteral("fontPixelSize")) && !integer(edit, QStringLiteral("fontPixelSize"), 1)) ||
                (edit.contains(QStringLiteral("alignment")) && !QStringList{QStringLiteral("left"), QStringLiteral("center"), QStringLiteral("right")}.contains(
                                                                   edit.value(QStringLiteral("alignment")).toString())))
                return invalid();
            auto item = items.at(edit.value(QStringLiteral("index")).toInt()).toElement();
            auto position = item.firstChildElement(QStringLiteral("position"));
            if (edit.contains(QStringLiteral("x"))) position.setAttribute(QStringLiteral("x"), edit.value(QStringLiteral("x")).toDouble());
            if (edit.contains(QStringLiteral("y"))) position.setAttribute(QStringLiteral("y"), edit.value(QStringLiteral("y")).toDouble());
            auto content = item.firstChildElement(QStringLiteral("content"));
            const bool text = item.attribute(QStringLiteral("type")) == QLatin1String("QGraphicsTextItem");
            if ((edit.contains(QStringLiteral("text")) || edit.contains(QStringLiteral("fontPixelSize")) || edit.contains(QStringLiteral("alignment"))) &&
                !text)
                return error(QStringLiteral("INVALID_TITLE"), QStringLiteral("Text fields apply only to text items."));
            if (edit.contains(QStringLiteral("text"))) {
                while (content.hasChildNodes())
                    content.removeChild(content.firstChild());
                content.appendChild(xml.createTextNode(edit.value(QStringLiteral("text")).toString()));
            }
            if (edit.contains(QStringLiteral("fontPixelSize"))) {
                const double before = content.attribute(QStringLiteral("font-pixel-size")).toDouble();
                const int size = edit.value(QStringLiteral("fontPixelSize")).toInt();
                content.setAttribute(QStringLiteral("font-pixel-size"), size);
                // Keep the stored text box proportional so alignment stays meaningful.
                if (before > 0) {
                    for (const auto &key : {QStringLiteral("box-width"), QStringLiteral("box-height")})
                        if (content.hasAttribute(key)) content.setAttribute(key, content.attribute(key).toDouble() * size / before);
                }
            }
            if (edit.contains(QStringLiteral("alignment"))) {
                const QString alignment = edit.value(QStringLiteral("alignment")).toString();
                content.setAttribute(QStringLiteral("alignment"), int(alignment == QLatin1String("left")    ? Qt::AlignLeft
                                                                      : alignment == QLatin1String("right") ? Qt::AlignRight
                                                                                                            : Qt::AlignHCenter));
            }
        }
        QMap<QString, QString> properties{{QStringLiteral("xmldata"), xml.toString(-1)}};
        auto previous = clip->currentProperties(properties);
        properties.insert(QStringLiteral("force_reload"), QStringLiteral("1"));
        previous.insert(QStringLiteral("force_reload"), QStringLiteral("1"));
        pCore->bin()->slotEditClipCommand(binId, previous, properties);
        return {{"ok", true}, {"binId", binId}};
    }
    if (type == QLatin1String("render")) return startRender(command);
    handled = false;
    return {};
}

// Timeline primitives. Each command validates first, then runs Kdenlive's own model or TimelineFunctions calls into one undo/redo pair and
// pushes it once, so every command (and a batch of them) is one Undo step tagged as an MCP entry.
namespace {
QJsonObject invalidTimeline(const QString &message)
{
    return error(QStringLiteral("INVALID_COMMAND"), message);
}

/** Reads an optional boolean; false when the key holds anything else. */
bool optionalBool(const QJsonObject &object, const QString &key, bool &value)
{
    if (!object.contains(key)) return true;
    if (!object.value(key).isBool()) return false;
    value = object.value(key).toBool();
    return true;
}

QJsonArray idArray(const QList<int> &ids)
{
    QJsonArray result;
    for (int id : ids)
        result.append(id);
    return result;
}

QList<int> sortedIds(const QSet<int> &ids)
{
    QList<int> result(ids.cbegin(), ids.cend());
    std::sort(result.begin(), result.end());
    return result;
}

/** Runs one native step on its own accumulator and appends it to undo/redo on success. Kdenlive's steps revert their own partial work when they fail. */
template <typename Step> bool runStep(Fun &undo, Fun &redo, Step &&step)
{
    Fun localUndo = [] { return true; };
    Fun localRedo = [] { return true; };
    if (!step(localUndo, localRedo)) return false;
    UPDATE_UNDO_REDO_NOLOCK(localRedo, localUndo, undo, redo);
    return true;
}
} // namespace

std::shared_ptr<TimelineItemModel> LiveBridge::sharedTimeline() const
{
    return std::static_pointer_cast<TimelineItemModel>(m_timeline->shared_from_this());
}

QJsonObject LiveBridge::editableTrack(int trackId) const
{
    if (!m_timeline->isTrack(trackId)) return error(QStringLiteral("UNKNOWN_TRACK"), QStringLiteral("Track does not exist."));
    if (m_timeline->trackIsLocked(trackId)) return error(QStringLiteral("TRACK_LOCKED"), QStringLiteral("Track is locked."));
    return {};
}

QList<int> LiveBridge::unlockedTracks() const
{
    QList<int> result;
    for (int row = 0; row < m_timeline->rowCount(); ++row) {
        const int trackId = int(m_timeline->index(row, 0).internalId());
        if (!m_timeline->trackIsLocked(trackId)) result.append(trackId);
    }
    return result;
}

QList<LiveBridge::ClipSpan> LiveBridge::trackClips(int trackId) const
{
    QList<ClipSpan> result;
    const auto track = m_timeline->makeTrackIndexFromID(trackId);
    for (int i = 0; i < m_timeline->rowCount(track); ++i) {
        const int id = int(m_timeline->index(i, 0, track).internalId());
        if (!m_timeline->isClip(id)) continue;
        const int start = m_timeline->getClipPosition(id);
        result.append({id, start, start + m_timeline->getClipPlaytime(id)});
    }
    std::sort(result.begin(), result.end(), [](const ClipSpan &a, const ClipSpan &b) { return a.start < b.start; });
    return result;
}

QSet<int> LiveBridge::allClipIds() const
{
    QSet<int> result;
    for (int row = 0; row < m_timeline->rowCount(); ++row)
        for (const auto &span : trackClips(int(m_timeline->index(row, 0).internalId())))
            result.insert(span.id);
    return result;
}

QJsonObject LiveBridge::executeTimeline(const QJsonObject &command, bool &handled)
{
    handled = true;
    const QString type = command.value(QStringLiteral("type")).toString();
    using Handler = QJsonObject (LiveBridge::*)(const QJsonObject &);
    static const QHash<QString, Handler> handlers{
        {QStringLiteral("insert"), &LiveBridge::insertClip},    {QStringLiteral("remove_clip"), &LiveBridge::removeClip},
        {QStringLiteral("split"), &LiveBridge::splitClips},     {QStringLiteral("remove_range"), &LiveBridge::removeRange},
        {QStringLiteral("remove_gap"), &LiveBridge::removeGap}, {QStringLiteral("insert_space"), &LiveBridge::insertSpace},
        {QStringLiteral("group"), &LiveBridge::groupClips},     {QStringLiteral("ungroup"), &LiveBridge::ungroupClips},
        {QStringLiteral("speed"), &LiveBridge::clipSpeed},      {QStringLiteral("enable"), &LiveBridge::clipEnable}};
    const auto handler = handlers.value(type);
    if (!handler) {
        handled = false;
        return {};
    }
    // A GUI selection is a group of its own: grouped deletion, cutting and moving would otherwise extend to every selected clip.
    m_timeline->requestClearSelection();
    return (this->*handler)(command);
}

QJsonObject LiveBridge::insertClip(const QJsonObject &command)
{
    const auto invalid = [] { return invalidTimeline(QStringLiteral("Invalid command fields or frame range.")); };
    if (!keys(command, {QStringLiteral("type"), QStringLiteral("binId"), QStringLiteral("trackId"), QStringLiteral("position"), QStringLiteral("sourceIn"),
                        QStringLiteral("sourceOut"), QStringLiteral("media"), QStringLiteral("mode"), QStringLiteral("linked"), QStringLiteral("audioTrackId"),
                        QStringLiteral("allTracks")}) ||
        !integer(command, QStringLiteral("trackId")) || !integer(command, QStringLiteral("position")) || !integer(command, QStringLiteral("sourceIn")) ||
        !integer(command, QStringLiteral("sourceOut"), 1) ||
        (command.contains(QStringLiteral("audioTrackId")) && !integer(command, QStringLiteral("audioTrackId"))))
        return invalid();
    const int trackId = command.value(QStringLiteral("trackId")).toInt();
    if (auto rejected = editableTrack(trackId); !rejected.isEmpty()) return rejected;
    const auto binId = command.value(QStringLiteral("binId")).toString();
    const auto media = command.value(QStringLiteral("media")).toString();
    const QString mode = command.value(QStringLiteral("mode")).toString(QStringLiteral("normal"));
    bool allTracks = false;
    if (!QRegularExpression(QStringLiteral("^[0-9]+$")).match(binId).hasMatch() || (media != QLatin1String("video") && media != QLatin1String("audio")) ||
        !QStringList{QStringLiteral("normal"), QStringLiteral("overwrite"), QStringLiteral("insert")}.contains(mode) ||
        !optionalBool(command, QStringLiteral("allTracks"), allTracks) || (allTracks && mode != QLatin1String("insert")))
        return invalid();
    if (command.contains(QStringLiteral("linked")) && !command.value(QStringLiteral("linked")).isBool()) return invalid();
    auto clip = pCore->projectItemModel()->getClipByBinID(binId);
    if (!clip || !clip->statusReady()) return error(QStringLiteral("MEDIA_NOT_READY"), QStringLiteral("Bin clip is missing or still loading."));
    const int start = command.value(QStringLiteral("sourceIn")).toInt();
    const int end = command.value(QStringLiteral("sourceOut")).toInt();
    const int position = command.value(QStringLiteral("position")).toInt();
    if (end <= start || size_t(end) > clip->frameDuration() || qint64(position) + end - start > std::numeric_limits<int>::max()) return invalid();
    if (m_timeline->isAudioTrack(trackId) != (media == QLatin1String("audio")))
        return error(QStringLiteral("INCOMPATIBLE_MEDIA"), QStringLiteral("media video needs a video track and media audio an audio track."));
    const bool linkRequested = command.value(QStringLiteral("linked")).toBool(false);
    if ((linkRequested || command.contains(QStringLiteral("audioTrackId"))) && media != QLatin1String("video"))
        return invalidTimeline(QStringLiteral("linked and audioTrackId apply to media video; the audio goes to an audio track."));
    if (command.contains(QStringLiteral("audioTrackId")) && command.contains(QStringLiteral("linked")) && !linkRequested)
        return invalidTimeline(QStringLiteral("audioTrackId needs linked insertion."));
    // Linked insertion places the audio of an A/V clip on an audio track and links both halves, as dropping the clip in the GUI does.
    int audioTrack = -1;
    const bool canLink = media == QLatin1String("video") && clip->hasAudioAndVideo();
    if (command.contains(QStringLiteral("audioTrackId"))) {
        audioTrack = command.value(QStringLiteral("audioTrackId")).toInt();
        if (auto rejected = editableTrack(audioTrack); !rejected.isEmpty()) return rejected;
        if (!m_timeline->isAudioTrack(audioTrack)) return error(QStringLiteral("INCOMPATIBLE_MEDIA"), QStringLiteral("audioTrackId must be an audio track."));
    } else if (canLink && command.value(QStringLiteral("linked")).toBool(true)) {
        // The GUI's mirror track first, else the nearest unlocked audio track.
        const int mirror = m_timeline->getMirrorTrackId(trackId);
        if (mirror >= 0 && m_timeline->isAudioTrack(mirror) && !m_timeline->trackIsLocked(mirror)) {
            audioTrack = mirror;
        } else {
            const int from = m_timeline->getTrackPosition(trackId);
            int best = std::numeric_limits<int>::max();
            for (int candidate : unlockedTracks()) {
                const int distance = qAbs(m_timeline->getTrackPosition(candidate) - from);
                if (m_timeline->isAudioTrack(candidate) && distance < best) {
                    best = distance;
                    audioTrack = candidate;
                }
            }
        }
    }
    if (linkRequested && !canLink)
        return error(QStringLiteral("INCOMPATIBLE_MEDIA"), QStringLiteral("Linked insertion needs a bin clip with audio and video."));
    if (linkRequested && audioTrack < 0) return error(QStringLiteral("NO_AUDIO_TRACK"), QStringLiteral("No unlocked audio track for the linked audio."));
    const bool linked = canLink && audioTrack >= 0 && command.value(QStringLiteral("linked")).toBool(true);
    const int duration = end - start;
    QList<int> targets{trackId};
    if (linked) targets.append(audioTrack);
    if (mode == QLatin1String("normal")) {
        for (int target : std::as_const(targets))
            for (const auto &span : trackClips(target))
                if (span.start < position + duration && span.end > position)
                    return error(
                        QStringLiteral("OVERLAP"),
                        QStringLiteral("Clip %1 occupies the range on track %2. Use mode overwrite or insert, or another position.").arg(span.id).arg(target));
    }
    auto timeline = sharedTimeline();
    Fun undo = [] { return true; };
    Fun redo = [] { return true; };
    const QPoint zone(position, position + duration);
    bool ok = true;
    if (mode == QLatin1String("overwrite")) {
        // Kdenlive's overwrite: lift the range from the receiving tracks.
        ok = runStep(undo, redo, [&](Fun &u, Fun &r) {
            bool result = TimelineFunctions::breakAffectedGroups(timeline, QVector<int>(targets.cbegin(), targets.cend()), zone, u, r);
            for (int target : std::as_const(targets))
                result = result && TimelineFunctions::liftZone(timeline, target, zone, u, r);
            return result;
        });
    } else if (mode == QLatin1String("insert")) {
        // Kdenlive's insert (TimelineFunctions::insertZone): cut at the position and push later items right on the rippled tracks.
        const QList<int> rippled = allTracks ? unlockedTracks() : targets;
        const QVector<int> tracks(rippled.cbegin(), rippled.cend());
        ok = runStep(undo, redo, [&](Fun &u, Fun &r) { return TimelineFunctions::breakAffectedGroups(timeline, tracks, zone, u, r); });
        for (int target : rippled) {
            const int cut = m_timeline->getClipByPosition(target, position);
            if (ok && cut >= 0 && m_timeline->getClipPosition(cut) < position)
                ok = runStep(undo, redo, [&](Fun &u, Fun &r) { return TimelineFunctions::requestClipCut(timeline, cut, position, u, r); });
        }
        ok = ok && runStep(undo, redo, [&](Fun &u, Fun &r) { return TimelineFunctions::requestInsertSpace(timeline, zone, u, r, tracks); });
    }
    int id = -1;
    int audioId = -1;
    int groupId = -1;
    // finalMove=0 keeps native validation without prompting to create audio tracks.
    ok = ok && runStep(undo, redo, [&](Fun &u, Fun &r) {
             return m_timeline->requestClipInsertion(QStringLiteral("%1%2/%3/%4")
                                                         .arg(media == QLatin1String("video") ? QStringLiteral("V") : QStringLiteral("A"), binId)
                                                         .arg(start)
                                                         .arg(end - 1),
                                                     trackId, position, id, true, true, false, u, r, {}, 0) &&
                    id >= 0;
         });
    if (ok && linked) {
        ok = runStep(undo, redo, [&](Fun &u, Fun &r) {
            return m_timeline->requestClipInsertion(QStringLiteral("A%1/%2/%3").arg(binId).arg(start).arg(end - 1), audioTrack, position, audioId, true, true,
                                                    false, u, r, {}, 0) &&
                   audioId >= 0;
        });
        ok = ok && runStep(undo, redo, [&](Fun &u, Fun &r) {
                 groupId = m_timeline->requestClipsGroup({id, audioId}, u, r, GroupType::AVSplit);
                 return groupId >= 0;
             });
    }
    if (!ok) {
        undo();
        return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native insertion rejected the range, track type or collision."));
    }
    pCore->pushUndo(undo, redo,
                    mode == QLatin1String("insert")      ? QStringLiteral("Insert clip (ripple)")
                    : mode == QLatin1String("overwrite") ? QStringLiteral("Overwrite clip")
                                                         : QStringLiteral("Insert Clip"));
    QJsonObject result{{"ok", true}, {"clipId", id}, {"mode", mode}, {"linked", linked}};
    if (linked) {
        result.insert(QStringLiteral("audioClipId"), audioId);
        result.insert(QStringLiteral("groupId"), m_timeline->getItemGroupId(id));
    }
    return result;
}

QJsonObject LiveBridge::removeClip(const QJsonObject &command)
{
    if (!keys(command, {QStringLiteral("type"), QStringLiteral("clipId"), QStringLiteral("mode"), QStringLiteral("group"), QStringLiteral("allTracks")}) ||
        !integer(command, QStringLiteral("clipId")))
        return invalidTimeline(QStringLiteral("Invalid command fields."));
    const QString mode = command.value(QStringLiteral("mode")).toString(QStringLiteral("lift"));
    const QString group = command.value(QStringLiteral("group")).toString(QStringLiteral("whole"));
    bool allTracks = false;
    if ((mode != QLatin1String("lift") && mode != QLatin1String("extract")) || (group != QLatin1String("whole") && group != QLatin1String("single")) ||
        !optionalBool(command, QStringLiteral("allTracks"), allTracks) || (allTracks && mode != QLatin1String("extract")))
        return invalidTimeline(QStringLiteral("mode is lift or extract, group whole or single; allTracks needs mode extract."));
    const int id = command.value(QStringLiteral("clipId")).toInt();
    if (auto rejected = editableClip(id); !rejected.isEmpty()) return rejected;
    const bool grouped = m_timeline->getItemGroupId(id) >= 0;
    const bool whole = grouped && group == QLatin1String("whole");
    QSet<int> members{id};
    if (whole)
        for (int item : m_timeline->getGroupElements(id))
            members.insert(item);
    for (int item : std::as_const(members))
        if (m_timeline->trackIsLocked(m_timeline->getItemTrackId(item)))
            return error(QStringLiteral("TRACK_LOCKED"), QStringLiteral("Item %1 of the group is on a locked track. Use group single.").arg(item));
    const auto before = allClipIds();
    Fun undo = [] { return true; };
    Fun redo = [] { return true; };
    bool ok = true;
    if (grouped && !whole) ok = runStep(undo, redo, [&](Fun &u, Fun &r) { return m_timeline->requestRemoveFromGroup(id, u, r); });
    QJsonArray ranges;
    if (mode == QLatin1String("lift")) {
        ok = ok && runStep(undo, redo, [&](Fun &u, Fun &r) { return m_timeline->requestItemDeletion(id, u, r, true); });
    } else {
        // Extract as the GUI's Extract does: each clip's own range on its own track; allTracks extracts the whole range from every unlocked
        // track, as Extract zone does.
        QList<std::pair<int, QPoint>> zones;
        int first = std::numeric_limits<int>::max();
        int last = 0;
        for (int item : std::as_const(members)) {
            if (!m_timeline->isClip(item)) continue;
            const int start = m_timeline->getClipPosition(item);
            const QPoint zone(start, start + m_timeline->getClipPlaytime(item));
            zones.append({m_timeline->getClipTrackId(item), zone});
            first = qMin(first, zone.x());
            last = qMax(last, zone.y());
        }
        if (allTracks) {
            const auto tracks = unlockedTracks();
            ok = ok && runStep(undo, redo,
                               [&](Fun &u, Fun &r) { return rippleRemove(QVector<int>(tracks.cbegin(), tracks.cend()), QPoint(first, last), false, u, r); });
            ok = ok && runStep(undo, redo, [&](Fun &u, Fun &r) { return rippleGuides(QPoint(first, last), u, r); });
            ranges.append(QJsonObject{{"trackId", QJsonValue::Null}, {"start", first}, {"end", last}});
        } else {
            for (const auto &item : std::as_const(zones)) {
                ok = ok && runStep(undo, redo, [&](Fun &u, Fun &r) { return rippleRemove({item.first}, item.second, false, u, r); });
                ranges.append(QJsonObject{{"trackId", item.first}, {"start", item.second.x()}, {"end", item.second.y()}});
            }
        }
    }
    if (!ok) {
        undo();
        return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native clip removal was rejected."));
    }
    const bool extract = mode == QLatin1String("extract");
    pCore->pushUndo(undo, redo,
                    extract ? (allTracks ? QStringLiteral("Extract zone") : QStringLiteral("Extract clip"))
                    : whole ? QStringLiteral("Remove group")
                            : QStringLiteral("Delete Clip"));
    const auto removed = idArray(sortedIds(before - allClipIds()));
    QJsonObject result{{"ok", true}, {"mode", mode}, {"removedClipIds", removed}, {"historySummary", QJsonObject{{"removedClipIds", removed}}}};
    if (extract) {
        result.insert(QStringLiteral("ranges"), ranges);
        result.insert(QStringLiteral("guidesMoved"), allTracks && guidesFollowRipple());
    }
    return result;
}

bool LiveBridge::rippleRemove(const QVector<int> &tracks, QPoint zone, bool liftOnly, Fun &undo, Fun &redo)
{
    // TimelineFunctions::extractZoneWithUndo, except that the rippled tracks are the given ones, not the GUI's active tracks.
    auto timeline = sharedTimeline();
    bool result = TimelineFunctions::breakAffectedGroups(timeline, tracks, zone, undo, redo);
    for (int track : tracks)
        result = result && TimelineFunctions::liftZone(timeline, track, zone, undo, redo);
    return result && (liftOnly || TimelineFunctions::removeSpace(timeline, zone, undo, redo, tracks, false));
}

bool LiveBridge::guidesFollowRipple() const
{
    // As the GUI's Extract zone: only when every track ripples and Kdenlive's "Lock guides" setting is off (it is on by default).
    return !KdenliveSettings::lockedGuides() && unlockedTracks().size() == m_timeline->getTracksCount();
}

bool LiveBridge::rippleGuides(QPoint zone, Fun &undo, Fun &redo)
{
    if (!guidesFollowRipple()) return true;
    auto guides = m_timeline->getGuideModel();
    const double fps = pCore->getCurrentFps();
    bool result = true;
    for (const auto &guide : guides->getMarkersInRange(zone.x(), zone.y()))
        result = result && guides->removeMarker(guide.time(), undo, redo);
    const auto later = guides->getMarkersInRange(zone.y(), -1);
    if (result && !later.isEmpty()) result = guides->moveMarkers(later, GenTime(zone.y(), fps), GenTime(zone.x(), fps), undo, redo);
    return result;
}

QJsonObject LiveBridge::splitClips(const QJsonObject &command)
{
    if (!keys(command,
              {QStringLiteral("type"), QStringLiteral("clipId"), QStringLiteral("trackId"), QStringLiteral("position"), QStringLiteral("allTracks")}) ||
        !integer(command, QStringLiteral("position")) || (command.contains(QStringLiteral("clipId")) && !integer(command, QStringLiteral("clipId"))) ||
        (command.contains(QStringLiteral("trackId")) && !integer(command, QStringLiteral("trackId"))))
        return invalidTimeline(QStringLiteral("Invalid command fields."));
    bool allTracks = false;
    if (!optionalBool(command, QStringLiteral("allTracks"), allTracks) ||
        int(command.contains(QStringLiteral("clipId"))) + int(command.contains(QStringLiteral("trackId"))) + int(allTracks) != 1)
        return invalidTimeline(QStringLiteral("Pass exactly one of clipId, trackId or allTracks: true."));
    const int position = command.value(QStringLiteral("position")).toInt();
    const auto inside = [this, position](int id) {
        const int start = m_timeline->getClipPosition(id);
        return start < position && position < start + m_timeline->getClipPlaytime(id);
    };
    const auto notSplittable = [] {
        return error(QStringLiteral("NOT_SPLITTABLE"), QStringLiteral("No clip spans the position: a cut needs a frame strictly inside a clip."));
    };
    QList<int> targets;
    if (command.contains(QStringLiteral("clipId"))) {
        const int id = command.value(QStringLiteral("clipId")).toInt();
        if (auto rejected = editableClip(id); !rejected.isEmpty()) return rejected;
        if (!inside(id)) return notSplittable();
        targets.append(id);
    } else {
        QList<int> tracks;
        if (allTracks) {
            tracks = unlockedTracks();
        } else {
            tracks.append(command.value(QStringLiteral("trackId")).toInt());
            if (auto rejected = editableTrack(tracks.first()); !rejected.isEmpty()) return rejected;
        }
        for (int track : std::as_const(tracks))
            for (const auto &span : trackClips(track))
                if (span.start < position && position < span.end) targets.append(span.id);
        if (targets.isEmpty()) return notSplittable();
    }
    // Every clip that will be cut: the targets and, as in the GUI, their group members under the position on unlocked tracks.
    QList<int> cut;
    for (int id : std::as_const(targets)) {
        const auto members = m_timeline->getItemGroupId(id) >= 0 ? m_timeline->getGroupElements(id) : std::unordered_set<int>{id};
        for (int item : members)
            if (m_timeline->isClip(item) && !m_timeline->trackIsLocked(m_timeline->getClipTrackId(item)) && inside(item) && !cut.contains(item))
                cut.append(item);
    }
    const auto before = allClipIds();
    auto timeline = sharedTimeline();
    Fun undo = [] { return true; };
    Fun redo = [] { return true; };
    for (int id : std::as_const(targets)) {
        // Cutting a grouped target already cut its group members.
        if (!inside(id)) continue;
        if (!TimelineFunctions::requestClipCut(timeline, id, position, undo, redo))
            // requestClipCut reverts everything accumulated so far when it fails.
            return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native cut was rejected."));
    }
    pCore->pushUndo(undo, redo, allTracks ? QStringLiteral("Cut all clips") : QStringLiteral("Cut clip"));
    QJsonArray pieces;
    QJsonArray summary;
    int right = -1;
    for (int id : std::as_const(cut)) {
        const int track = m_timeline->getClipTrackId(id);
        const int piece = m_timeline->getClipByStartPosition(track, position);
        pieces.append(QJsonObject{{"trackId", track}, {"leftClipId", id}, {"rightClipId", piece}});
        summary.append(QJsonArray{id, piece});
        if (id == targets.first()) right = piece;
    }
    QJsonObject result{{"ok", true},
                       {"position", position},
                       {"pieces", pieces},
                       {"newClipIds", idArray(sortedIds(allClipIds() - before))},
                       {"historySummary", QJsonObject{{"pieces", summary}}}};
    if (!allTracks) {
        result.insert(QStringLiteral("leftClipId"), targets.first());
        result.insert(QStringLiteral("rightClipId"), right);
    }
    return result;
}

namespace {
/** trackIds (a non-empty array of distinct ids) or allTracks: true; trackId is accepted where one track is meant. */
QJsonObject rippleTracks(const QJsonObject &command, const QString &listKey, QList<int> &tracks, bool &allTracks)
{
    allTracks = false;
    if (!optionalBool(command, QStringLiteral("allTracks"), allTracks)) return invalidTimeline(QStringLiteral("allTracks must be a boolean."));
    if (allTracks == command.contains(listKey)) return invalidTimeline(QStringLiteral("Pass exactly one of %1 or allTracks: true.").arg(listKey));
    if (allTracks) return {};
    const auto value = command.value(listKey);
    if (listKey == QLatin1String("trackId")) {
        if (!integer(command, listKey)) return invalidTimeline(QStringLiteral("Invalid trackId."));
        tracks.append(value.toInt());
        return {};
    }
    if (!value.isArray() || value.toArray().isEmpty() || value.toArray().size() > 64)
        return invalidTimeline(QStringLiteral("trackIds must list 1 to 64 tracks."));
    for (const auto &item : value.toArray()) {
        if (!item.isDouble() || item.toDouble() < 0 || std::floor(item.toDouble()) != item.toDouble() || tracks.contains(item.toInt()))
            return invalidTimeline(QStringLiteral("trackIds must be distinct track ids."));
        tracks.append(item.toInt());
    }
    return {};
}
} // namespace

QJsonObject LiveBridge::removeRange(const QJsonObject &command)
{
    if (!keys(command, {QStringLiteral("type"), QStringLiteral("start"), QStringLiteral("end"), QStringLiteral("trackIds"), QStringLiteral("allTracks"),
                        QStringLiteral("mode")}) ||
        !integer(command, QStringLiteral("start")) || !integer(command, QStringLiteral("end"), 1) ||
        command.value(QStringLiteral("start")).toInt() >= command.value(QStringLiteral("end")).toInt())
        return invalidTimeline(QStringLiteral("Expected start < end (end exclusive)."));
    const QString mode = command.value(QStringLiteral("mode")).toString(QStringLiteral("lift"));
    if (mode != QLatin1String("lift") && mode != QLatin1String("extract")) return invalidTimeline(QStringLiteral("mode is lift or extract."));
    QList<int> tracks;
    bool allTracks = false;
    if (auto rejected = rippleTracks(command, QStringLiteral("trackIds"), tracks, allTracks); !rejected.isEmpty()) return rejected;
    for (int track : std::as_const(tracks))
        if (auto rejected = editableTrack(track); !rejected.isEmpty()) return rejected;
    if (allTracks) tracks = unlockedTracks();
    const QPoint zone(command.value(QStringLiteral("start")).toInt(), command.value(QStringLiteral("end")).toInt());
    const bool extract = mode == QLatin1String("extract");
    const auto before = allClipIds();
    Fun undo = [] { return true; };
    Fun redo = [] { return true; };
    bool ok = runStep(undo, redo, [&](Fun &u, Fun &r) { return rippleRemove(QVector<int>(tracks.cbegin(), tracks.cend()), zone, !extract, u, r); });
    if (ok && extract && allTracks) ok = runStep(undo, redo, [&](Fun &u, Fun &r) { return rippleGuides(zone, u, r); });
    if (!ok) {
        undo();
        return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native lift/extract was rejected."));
    }
    pCore->pushUndo(undo, redo, extract ? QStringLiteral("Extract zone") : QStringLiteral("Lift zone"));
    const auto after = allClipIds();
    const auto removed = idArray(sortedIds(before - after));
    const auto created = idArray(sortedIds(after - before));
    return {{"ok", true},
            {"mode", mode},
            {"trackIds", idArray(tracks)},
            {"removedClipIds", removed},
            {"newClipIds", created},
            {"guidesMoved", extract && allTracks && guidesFollowRipple()},
            {"historySummary", QJsonObject{{"removedClipIds", removed}, {"newClipIds", created}}}};
}

QJsonObject LiveBridge::removeGap(const QJsonObject &command)
{
    if (!keys(command, {QStringLiteral("type"), QStringLiteral("position"), QStringLiteral("trackId"), QStringLiteral("allTracks")}) ||
        !integer(command, QStringLiteral("position")))
        return invalidTimeline(QStringLiteral("Invalid command fields."));
    QList<int> tracks;
    bool allTracks = false;
    if (auto rejected = rippleTracks(command, QStringLiteral("trackId"), tracks, allTracks); !rejected.isEmpty()) return rejected;
    const int position = command.value(QStringLiteral("position")).toInt();
    const int trackId = allTracks ? -1 : tracks.first();
    if (!allTracks) {
        if (auto rejected = editableTrack(trackId); !rejected.isEmpty()) return rejected;
    }
    // The blank around position on each checked track, and the first clip after it, so the result can be verified and reported.
    int next = -1;
    int nextStart = std::numeric_limits<int>::max();
    for (int track : allTracks ? unlockedTracks() : tracks) {
        int after = -1;
        for (const auto &span : trackClips(track)) {
            if (span.start <= position && position < span.end)
                return error(QStringLiteral("NO_GAP"), QStringLiteral("Track %1 has a clip at the position, not a gap.").arg(track));
            if (span.start > position && after < 0) after = span.id;
        }
        if (after >= 0 && m_timeline->getClipPosition(after) < nextStart) {
            next = after;
            nextStart = m_timeline->getClipPosition(after);
        }
    }
    if (next < 0) return error(QStringLiteral("NO_GAP"), QStringLiteral("No clip follows the position, so there is no gap to close."));
    // The GUI's Remove space: it refuses a gap between two clips of one group and moves guides only when "Lock guides" is off.
    if (!TimelineFunctions::requestDeleteBlankAt(sharedTimeline(), trackId, position, allTracks) || m_timeline->getClipPosition(next) >= nextStart)
        return error(QStringLiteral("EDIT_REJECTED"),
                     QStringLiteral("Native space removal was rejected: the gap may lie inside a group or not be blank on every track."));
    return {{"ok", true},
            {"position", position},
            {"removed", nextStart - m_timeline->getClipPosition(next)},
            {"guidesMoved", !KdenliveSettings::lockedGuides()},
            {"historySummary", QJsonObject{{"removed", nextStart - m_timeline->getClipPosition(next)}, {"firstMovedClipId", next}}}};
}

QJsonObject LiveBridge::insertSpace(const QJsonObject &command)
{
    if (!keys(command,
              {QStringLiteral("type"), QStringLiteral("position"), QStringLiteral("duration"), QStringLiteral("trackId"), QStringLiteral("allTracks")}) ||
        !integer(command, QStringLiteral("position")) || !integer(command, QStringLiteral("duration"), 1))
        return invalidTimeline(QStringLiteral("Invalid command fields."));
    QList<int> tracks;
    bool allTracks = false;
    if (auto rejected = rippleTracks(command, QStringLiteral("trackId"), tracks, allTracks); !rejected.isEmpty()) return rejected;
    const int trackId = allTracks ? -1 : tracks.first();
    if (!allTracks) {
        if (auto rejected = editableTrack(trackId); !rejected.isEmpty()) return rejected;
    }
    const int position = command.value(QStringLiteral("position")).toInt();
    const int duration = command.value(QStringLiteral("duration")).toInt();
    if (qint64(m_timeline->duration()) + duration > std::numeric_limits<int>::max()) return invalidTimeline(QStringLiteral("duration is too large."));
    auto timeline = sharedTimeline();
    // The GUI's Insert space: items at or after position (a clip spanning it moves whole), with their groups, move right.
    const int first = TimelineFunctions::requestSpacerStartOperation(timeline, trackId, position).first;
    if (first < 0) return error(QStringLiteral("NOTHING_TO_MOVE"), QStringLiteral("No item at or after the position on the chosen tracks."));
    const int start = m_timeline->getItemPosition(first);
    Fun undo = [] { return true; };
    Fun redo = [] { return true; };
    const bool guides = allTracks && !KdenliveSettings::lockedGuides();
    if (!TimelineFunctions::requestSpacerEndOperation(timeline, first, start, start + duration, trackId, guides ? start : -1, undo, redo))
        return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native space insertion was rejected."));
    return {{"ok", true},
            {"position", position},
            {"duration", duration},
            {"firstMovedClipId", first},
            {"guidesMoved", guides},
            {"historySummary", QJsonObject{{"firstMovedClipId", first}}}};
}

QJsonObject LiveBridge::groupClips(const QJsonObject &command)
{
    const auto list = command.value(QStringLiteral("clipIds"));
    if (!keys(command, {QStringLiteral("type"), QStringLiteral("clipIds")}) || !list.isArray() || list.toArray().size() < 2 || list.toArray().size() > 200)
        return invalidTimeline(QStringLiteral("clipIds must list 2 to 200 timeline clips."));
    std::unordered_set<int> ids;
    for (const auto &value : list.toArray()) {
        if (!value.isDouble() || value.toDouble() < 0 || std::floor(value.toDouble()) != value.toDouble() || ids.count(value.toInt()))
            return invalidTimeline(QStringLiteral("clipIds must be distinct clip ids."));
        if (auto rejected = editableClip(value.toInt()); !rejected.isEmpty()) return rejected;
        ids.insert(value.toInt());
    }
    const int first = list.toArray().first().toInt();
    QSet<int> roots;
    for (int id : ids)
        roots.insert(m_timeline->getItemGroupId(id));
    if (roots.size() == 1 && !roots.contains(-1)) return {{"ok", true}, {"groupId", *roots.cbegin()}, {"changed", false}};
    // Grouping a video and an audio clip of the same bin clip links them (an A/V group), as in the GUI.
    if (m_timeline->requestClipsGroup(ids, true) < 0) return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native grouping was rejected."));
    const int groupId = m_timeline->getItemGroupId(first);
    const auto members = m_timeline->getGroupElements(first);
    QList<int> clipIds(members.cbegin(), members.cend());
    std::sort(clipIds.begin(), clipIds.end());
    return {{"ok", true}, {"groupId", groupId}, {"changed", true}, {"clipIds", idArray(clipIds)}, {"linked", m_timeline->getClipSplitPartner(first) >= 0}};
}

QJsonObject LiveBridge::ungroupClips(const QJsonObject &command)
{
    if (!keys(command, {QStringLiteral("type"), QStringLiteral("clipId"), QStringLiteral("groupId")}) ||
        command.contains(QStringLiteral("clipId")) == command.contains(QStringLiteral("groupId")) ||
        !integer(command, command.contains(QStringLiteral("clipId")) ? QStringLiteral("clipId") : QStringLiteral("groupId")))
        return invalidTimeline(QStringLiteral("Pass exactly one of clipId or groupId."));
    int item = -1;
    int groupId = -1;
    if (command.contains(QStringLiteral("clipId"))) {
        item = command.value(QStringLiteral("clipId")).toInt();
        if (!m_timeline->isClip(item)) return error(QStringLiteral("UNKNOWN_CLIP"), QStringLiteral("Timeline clip does not exist."));
        groupId = m_timeline->getItemGroupId(item);
        if (groupId < 0) return error(QStringLiteral("UNKNOWN_GROUP"), QStringLiteral("The clip is not grouped."));
    } else {
        groupId = command.value(QStringLiteral("groupId")).toInt();
        // Only a topmost group, as desktop_state reports them.
        if (m_timeline->isGroup(groupId))
            for (int member : m_timeline->getGroupElements(groupId))
                if (m_timeline->getItemGroupId(member) == groupId) item = member;
        if (item < 0) return error(QStringLiteral("UNKNOWN_GROUP"), QStringLiteral("No such group; use a groupId from desktop_state."));
    }
    const auto members = m_timeline->getGroupElements(item);
    QList<int> clipIds;
    for (int member : members)
        if (m_timeline->isClip(member)) clipIds.append(member);
    std::sort(clipIds.begin(), clipIds.end());
    // Ungrouping an A/V group unlinks its audio and video, as in the GUI.
    if (!m_timeline->requestClipUngroup(item, true)) return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native ungrouping was rejected."));
    return {{"ok", true}, {"groupId", groupId}, {"clipIds", idArray(clipIds)}, {"historySummary", QJsonObject{{"clipIds", idArray(clipIds)}}}};
}

QJsonObject LiveBridge::clipSpeed(const QJsonObject &command)
{
    const auto speedValue = command.value(QStringLiteral("speed"));
    if (!keys(command, {QStringLiteral("type"), QStringLiteral("clipId"), QStringLiteral("speed"), QStringLiteral("pitchCompensation")}) ||
        !integer(command, QStringLiteral("clipId")) || !speedValue.isDouble() || !std::isfinite(speedValue.toDouble()) || qAbs(speedValue.toDouble()) < 0.01 ||
        qAbs(speedValue.toDouble()) > 100 ||
        (command.contains(QStringLiteral("pitchCompensation")) && !command.value(QStringLiteral("pitchCompensation")).isBool()))
        return invalidTimeline(QStringLiteral("speed is a factor with 0.01 <= |speed| <= 100 (1 normal, 0.5 half, 2 double, negative reverse)."));
    const int id = command.value(QStringLiteral("clipId")).toInt();
    if (auto rejected = editableClip(id); !rejected.isEmpty()) return rejected;
    const int partner = m_timeline->getClipSplitPartner(id);
    if (partner >= 0 && m_timeline->trackIsLocked(m_timeline->getClipTrackId(partner)))
        return error(QStringLiteral("TRACK_LOCKED"), QStringLiteral("The linked clip is on a locked track."));
    auto binClip = pCore->projectItemModel()->getClipByBinID(m_timeline->getClipBinId(id));
    if (!binClip || !binClip->hasLimitedDuration())
        return error(QStringLiteral("INCOMPATIBLE_MEDIA"), QStringLiteral("Speed changes need video or audio media with a fixed duration."));
    const double speed = speedValue.toDouble();
    const double previous = m_timeline->getClipSpeed(id);
    const int previousDuration = m_timeline->getClipPlaytime(id);
    const bool pitch = command.value(QStringLiteral("pitchCompensation")).toBool(m_timeline->getClipProducer(id)->get_int("warp_pitch") != 0);
    if (qFuzzyCompare(speed, previous) && pitch == (m_timeline->getClipProducer(id)->get_int("warp_pitch") != 0))
        return {{"ok", true}, {"clipId", id}, {"speed", previous}, {"duration", previousDuration}, {"changed", false}};
    // The clip keeps its source range, so its duration scales; a longer clip must fit before the next clip on its track.
    for (int item : {id, partner}) {
        if (item < 0) continue;
        const int duration = m_timeline->getClipPlaytime(item);
        const int wanted = qMax(1, int(qRound64(double(duration) * std::fabs(m_timeline->getClipSpeed(item) / speed))));
        if (wanted > duration && wanted - duration > m_timeline->getBlankSizeNearClip(item, true))
            return error(QStringLiteral("OVERLAP"), QStringLiteral("At this speed clip %1 would run into the next clip on its track.").arg(item));
    }
    // As the GUI's Change speed: the linked clip changes too, in one Undo step.
    Fun undo = [] { return true; };
    Fun redo = [] { return true; };
    bool ok = true;
    for (int item : {partner, id}) {
        if (item < 0) continue;
        ok = ok && runStep(undo, redo, [&](Fun &u, Fun &r) { return m_timeline->requestClipTimeWarp(item, speed, pitch, true, u, r); });
    }
    if (!ok) {
        undo();
        return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native speed change was rejected."));
    }
    pCore->pushUndo(undo, redo, QStringLiteral("Change clip speed"));
    return {
        {"ok", true},
        {"clipId", id},
        {"speed", m_timeline->getClipSpeed(id)},
        {"duration", m_timeline->getClipPlaytime(id)},
        {"linkedClipId", optionalId(partner)},
        {"changed", true},
        {"historySummary", QJsonObject{{"previousSpeed", previous}, {"previousDuration", previousDuration}, {"newDuration", m_timeline->getClipPlaytime(id)}}}};
}

QJsonObject LiveBridge::clipEnable(const QJsonObject &command)
{
    bool linked = true;
    if (!keys(command, {QStringLiteral("type"), QStringLiteral("clipId"), QStringLiteral("enabled"), QStringLiteral("linked")}) ||
        !integer(command, QStringLiteral("clipId")) || !command.value(QStringLiteral("enabled")).isBool() ||
        !optionalBool(command, QStringLiteral("linked"), linked))
        return invalidTimeline(QStringLiteral("Expected clipId, enabled (boolean) and optional linked (boolean)."));
    const int id = command.value(QStringLiteral("clipId")).toInt();
    if (auto rejected = editableClip(id); !rejected.isEmpty()) return rejected;
    const bool enabled = command.value(QStringLiteral("enabled")).toBool();
    std::unordered_set<int> ids{id};
    const int partner = m_timeline->getClipSplitPartner(id);
    if (linked && partner >= 0) {
        if (m_timeline->trackIsLocked(m_timeline->getClipTrackId(partner)))
            return error(QStringLiteral("TRACK_LOCKED"), QStringLiteral("The linked clip is on a locked track; pass linked: false."));
        ids.insert(partner);
    }
    QList<int> clipIds(ids.cbegin(), ids.cend());
    std::sort(clipIds.begin(), clipIds.end());
    bool changed = false;
    for (int item : ids)
        changed = changed || (m_timeline->getClipState(item).first != PlaylistState::Disabled) != enabled;
    if (changed && !TimelineFunctions::setClipsEnabled(sharedTimeline(), ids, enabled))
        return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native clip state change was rejected."));
    return {{"ok", true}, {"clipIds", idArray(clipIds)}, {"enabled", enabled}, {"changed", changed}};
}

namespace {
QJsonObject invalidMarker(const QString &message)
{
    return error(QStringLiteral("INVALID_ARGUMENTS"), message);
}

const QStringList &markerFormats()
{
    static const QStringList result{QStringLiteral("json"), QStringLiteral("csv"), QStringLiteral("kdenlive")};
    return result;
}

/** Resolves a category index or name (exact, then case-insensitive) against the project's marker categories. */
QJsonObject markerCategory(const QJsonValue &value, int &category)
{
    const auto unknown = [] {
        return error(QStringLiteral("UNKNOWN_CATEGORY"), QStringLiteral("Unknown marker category. desktop_capabilities lists markerCategories."));
    };
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (!std::isfinite(number) || std::floor(number) != number || number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max() ||
            !pCore->markerTypes.contains(int(number)))
            return unknown();
        category = int(number);
        return {};
    }
    if (!value.isString() || value.toString().trimmed().isEmpty())
        return error(QStringLiteral("INVALID_COMMAND"), QStringLiteral("category must be a category index or name."));
    const QString name = value.toString().trimmed();
    for (const auto sensitivity : {Qt::CaseSensitive, Qt::CaseInsensitive})
        for (auto it = pCore->markerTypes.cbegin(); it != pCore->markerTypes.cend(); ++it)
            if (it.value().displayName.compare(name, sensitivity) == 0) {
                category = it.key();
                return {};
            }
    return unknown();
}

int defaultMarkerCategory()
{
    const int preferred = KdenliveSettings::default_marker_type();
    if (pCore->markerTypes.contains(preferred)) return preferred;
    return pCore->markerTypes.isEmpty() ? -1 : pCore->markerTypes.firstKey();
}

/** Guides of the active sequence, or the markers of one bin clip (binId). length bounds new marker positions. */
struct MarkerTarget
{
    std::shared_ptr<MarkerListModel> model;
    int length{0};
    bool guides{true};
};

QJsonObject markerTarget(const QJsonObject &object, TimelineItemModel *timeline, MarkerTarget &target)
{
    if (!object.contains(QStringLiteral("binId"))) {
        target.model = timeline->getGuideModel();
        target.length = timeline->duration();
        return target.model ? QJsonObject{} : error(QStringLiteral("NOT_READY"), QStringLiteral("The active sequence has no guides model."));
    }
    const QString id = object.value(QStringLiteral("binId")).toString();
    if (!QRegularExpression(QStringLiteral("^[0-9]+$")).match(id).hasMatch()) return error(QStringLiteral("INVALID_COMMAND"), QStringLiteral("Invalid binId."));
    auto clip = pCore->projectItemModel()->getClipByBinID(id);
    if (!clip || !clip->statusReady()) return error(QStringLiteral("MEDIA_NOT_READY"), QStringLiteral("Bin clip is missing or still loading."));
    if (clip->clipType() == ClipType::Timeline)
        return error(QStringLiteral("SEQUENCE_PROTECTED"), QStringLiteral("Mark a sequence with guides: omit binId to target the active sequence."));
    target.model = clip->getMarkerModel();
    target.length = int(qMin<qint64>(qint64(clip->frameDuration()), std::numeric_limits<int>::max()));
    target.guides = false;
    return target.model ? QJsonObject{} : error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Bin clip has no marker model."));
}

struct MarkerEntry
{
    int position{0};
    int duration{0};
    QString comment;
    int category{0};
};

QJsonObject markerSpan(const MarkerEntry &entry, const MarkerTarget &target)
{
    if (entry.position >= target.length || qint64(entry.position) + entry.duration > target.length)
        return invalidMarker(QStringLiteral("Marker at %1 (duration %2) must lie within the %3 (%4 frames).")
                                 .arg(entry.position)
                                 .arg(entry.duration)
                                 .arg(target.guides ? QStringLiteral("sequence") : QStringLiteral("clip"))
                                 .arg(target.length));
    if (entry.comment.size() > 4096) return invalidMarker(QStringLiteral("Marker comments are limited to 4096 characters."));
    return {};
}

bool addMarkerTo(const std::shared_ptr<MarkerListModel> &model, const MarkerEntry &entry, double fps, Fun &undo, Fun &redo)
{
    const GenTime position(entry.position, fps);
    // Both native calls replace a marker already at that frame (comment, category and duration).
    return entry.duration > 0 ? model->addRangeMarker(position, GenTime(entry.duration, fps), entry.comment, entry.category, undo, redo)
                              : model->addMarker(position, entry.comment, entry.category, undo, redo);
}

QString markerNoun(const MarkerTarget &target, qsizetype count)
{
    const QString noun = target.guides ? QStringLiteral("guide") : QStringLiteral("clip marker");
    return count == 1 ? noun : QStringLiteral("%1 %2s").arg(count).arg(noun);
}

QJsonObject markerFrame(const QJsonObject &object, const QString &key, int &value, bool optional)
{
    if (!object.contains(key)) return optional ? QJsonObject{} : invalidMarker(QStringLiteral("%1 is required.").arg(key));
    if (!integer(object, key)) return invalidMarker(QStringLiteral("%1 must be a whole frame number of at least 0.").arg(key));
    value = object.value(key).toInt();
    return {};
}

QString csvField(const QString &value)
{
    if (!value.contains(QLatin1Char(',')) && !value.contains(QLatin1Char('"')) && !value.contains(QLatin1Char('\n')) && !value.contains(QLatin1Char('\r')))
        return value;
    return QLatin1Char('"') + QString(value).replace(QLatin1Char('"'), QStringLiteral("\"\"")) + QLatin1Char('"');
}

/** RFC 4180 records: quoted fields may hold commas, doubled quotes and line breaks. Returns false on an unterminated quote. */
bool parseCsv(const QString &text, QList<QStringList> &rows)
{
    QStringList row;
    QString field;
    bool quoted = false;
    bool started = false;
    const auto endRow = [&] {
        row << field;
        if (row.size() > 1 || !row.first().trimmed().isEmpty()) rows << row;
        row.clear();
        field.clear();
        started = false;
    };
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (quoted) {
            if (c != QLatin1Char('"'))
                field += c;
            else if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('"'))
                field += text.at(++i);
            else
                quoted = false;
        } else if (c == QLatin1Char('"') && !started) {
            quoted = started = true;
        } else if (c == QLatin1Char(',')) {
            row << field;
            field.clear();
            started = false;
        } else if (c == QLatin1Char('\n')) {
            endRow();
        } else if (c != QLatin1Char('\r')) {
            field += c;
            started = true;
        }
    }
    if (quoted) return false;
    if (started || !field.isEmpty() || !row.isEmpty()) endRow();
    return true;
}

/** Parses marker text in one of markerFormats() into entries with resolved categories. */
QJsonObject parseMarkers(const QString &format, const QString &text, QList<MarkerEntry> &entries)
{
    const int fallback = defaultMarkerCategory();
    const auto entryError = [](qsizetype index, const QString &message) { return invalidMarker(QStringLiteral("Marker %1: %2").arg(index + 1).arg(message)); };
    if (format == QLatin1String("csv")) {
        QList<QStringList> rows;
        if (!parseCsv(text, rows)) return invalidMarker(QStringLiteral("CSV has an unterminated quoted field."));
        if (rows.isEmpty()) return invalidMarker(QStringLiteral("CSV needs a header row."));
        QHash<QString, int> columns;
        for (int i = 0; i < rows.first().size(); ++i)
            columns.insert(rows.first().at(i).trimmed().toLower(), i);
        if (!columns.contains(QStringLiteral("position")) && !columns.contains(QStringLiteral("timecode")))
            return invalidMarker(QStringLiteral("CSV header needs a position (frames) or timecode column."));
        const QRegularExpression timecode(QStringLiteral("^\\d{2}:\\d{2}:\\d{2}[:;]\\d{2,}$"));
        for (qsizetype r = 1; r < rows.size(); ++r) {
            const auto &row = rows.at(r);
            const auto cell = [&](const QString &name) { return columns.contains(name) ? row.value(columns.value(name)) : QString(); };
            MarkerEntry entry;
            bool ok = true;
            if (!cell(QStringLiteral("position")).trimmed().isEmpty()) {
                entry.position = cell(QStringLiteral("position")).trimmed().toInt(&ok);
            } else if (timecode.match(cell(QStringLiteral("timecode")).trimmed()).hasMatch()) {
                entry.position = pCore->timecode().getFrameCount(cell(QStringLiteral("timecode")).trimmed());
            } else {
                ok = false;
            }
            if (!ok || entry.position < 0) return entryError(r - 1, QStringLiteral("needs a frame position or an HH:MM:SS:FF timecode."));
            if (!cell(QStringLiteral("duration")).trimmed().isEmpty()) {
                entry.duration = cell(QStringLiteral("duration")).trimmed().toInt(&ok);
                if (!ok || entry.duration < 0) return entryError(r - 1, QStringLiteral("duration must be a frame count of at least 0."));
            }
            entry.comment = cell(QStringLiteral("comment"));
            entry.category = fallback;
            const QString index = cell(QStringLiteral("category")).trimmed();
            const QString name = cell(QStringLiteral("categoryname")).trimmed();
            if (!index.isEmpty() || !name.isEmpty()) {
                const int number = index.toInt(&ok);
                if (auto rejected = markerCategory(ok ? QJsonValue(number) : QJsonValue(index.isEmpty() ? name : index), entry.category); !rejected.isEmpty())
                    return rejected;
            }
            entries << entry;
        }
    } else {
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(text.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isArray()) return invalidMarker(QStringLiteral("Expected a JSON array of markers."));
        const bool native = format == QLatin1String("kdenlive");
        const QString positionKey = native ? QStringLiteral("pos") : QStringLiteral("position");
        const QString categoryKey = native ? QStringLiteral("type") : QStringLiteral("category");
        const auto array = document.array();
        for (qsizetype i = 0; i < array.size(); ++i) {
            const auto object = array.at(i).toObject();
            if (!array.at(i).isObject() || !integer(object, positionKey))
                return entryError(i, QStringLiteral("needs a whole frame %1 of at least 0.").arg(positionKey));
            MarkerEntry entry{object.value(positionKey).toInt(), 0, QString(), fallback};
            if (object.contains(QStringLiteral("duration"))) {
                if (!integer(object, QStringLiteral("duration"))) return entryError(i, QStringLiteral("duration must be a frame count of at least 0."));
                entry.duration = object.value(QStringLiteral("duration")).toInt();
            }
            if (object.contains(QStringLiteral("comment"))) {
                if (!object.value(QStringLiteral("comment")).isString()) return entryError(i, QStringLiteral("comment must be a string."));
                entry.comment = object.value(QStringLiteral("comment")).toString();
            }
            if (object.contains(categoryKey)) {
                if (auto rejected = markerCategory(object.value(categoryKey), entry.category); !rejected.isEmpty()) return rejected;
            }
            entries << entry;
        }
    }
    if (entries.isEmpty()) return invalidMarker(QStringLiteral("No markers to import."));
    if (entries.size() > 10000) return invalidMarker(QStringLiteral("At most 10000 markers per import."));
    if (fallback < 0) return error(QStringLiteral("UNKNOWN_CATEGORY"), QStringLiteral("The project defines no marker categories."));
    return {};
}
} // namespace

QJsonObject LiveBridge::executeMarker(const QJsonObject &command)
{
    const QString type = command.value(QStringLiteral("type")).toString();
    const double fps = pCore->getCurrentFps();
    const auto invalid = [](const QString &message) { return error(QStringLiteral("INVALID_COMMAND"), message); };
    if (command.contains(QStringLiteral("comment")) && !command.value(QStringLiteral("comment")).isString())
        return invalid(QStringLiteral("comment must be a string."));
    MarkerTarget target;
    const auto finish = [&](QJsonObject result) {
        result.insert(QStringLiteral("ok"), true);
        result.insert(QStringLiteral("target"), target.guides ? QStringLiteral("guides") : QStringLiteral("clip"));
        if (!target.guides) result.insert(QStringLiteral("binId"), command.value(QStringLiteral("binId")));
        return result;
    };
    const auto lookup = [&](int position, CommentedTime &marker) {
        bool found = false;
        marker = target.model->getMarker(GenTime(position, fps), &found);
        return found ? QJsonObject{} : error(QStringLiteral("UNKNOWN_MARKER"), QStringLiteral("No %1 at frame %2.").arg(markerNoun(target, 1)).arg(position));
    };

    if (type == QLatin1String("marker_add")) {
        if (!keys(command, {QStringLiteral("type"), QStringLiteral("position"), QStringLiteral("duration"), QStringLiteral("comment"),
                            QStringLiteral("category"), QStringLiteral("binId")}))
            return invalid(QStringLiteral("marker_add takes position, optional duration, comment, category and binId."));
        if (auto rejected = markerTarget(command, m_timeline, target); !rejected.isEmpty()) return rejected;
        MarkerEntry entry{0, 0, command.value(QStringLiteral("comment")).toString(), defaultMarkerCategory()};
        if (auto rejected = markerFrame(command, QStringLiteral("position"), entry.position, false); !rejected.isEmpty()) return rejected;
        if (auto rejected = markerFrame(command, QStringLiteral("duration"), entry.duration, true); !rejected.isEmpty()) return rejected;
        if (command.contains(QStringLiteral("category"))) {
            if (auto rejected = markerCategory(command.value(QStringLiteral("category")), entry.category); !rejected.isEmpty()) return rejected;
        } else if (entry.category < 0) {
            return error(QStringLiteral("UNKNOWN_CATEGORY"), QStringLiteral("The project defines no marker categories."));
        }
        if (auto rejected = markerSpan(entry, target); !rejected.isEmpty()) return rejected;
        const bool replaced = target.model->hasMarker(GenTime(entry.position, fps));
        Fun undo = [] { return true; };
        Fun redo = [] { return true; };
        if (!addMarkerTo(target.model, entry, fps, undo, redo))
            return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native marker edit was rejected."));
        pCore->pushUndo(undo, redo, QStringLiteral("%1 %2").arg(replaced ? QStringLiteral("Replace") : QStringLiteral("Add"), markerNoun(target, 1)));
        CommentedTime marker;
        lookup(entry.position, marker);
        return finish({{"marker", markerJson(marker, fps)}, {"replaced", replaced}});
    }

    if (type == QLatin1String("marker_edit")) {
        if (!keys(command, {QStringLiteral("type"), QStringLiteral("position"), QStringLiteral("binId"), QStringLiteral("newPosition"),
                            QStringLiteral("duration"), QStringLiteral("comment"), QStringLiteral("category")}))
            return invalid(QStringLiteral("marker_edit takes position, optional binId, and newPosition, duration, comment or category."));
        if (!command.contains(QStringLiteral("newPosition")) && !command.contains(QStringLiteral("duration")) && !command.contains(QStringLiteral("comment")) &&
            !command.contains(QStringLiteral("category")))
            return invalid(QStringLiteral("Pass at least one of newPosition, duration, comment or category."));
        if (auto rejected = markerTarget(command, m_timeline, target); !rejected.isEmpty()) return rejected;
        int position = 0;
        if (auto rejected = markerFrame(command, QStringLiteral("position"), position, false); !rejected.isEmpty()) return rejected;
        CommentedTime current;
        if (auto rejected = lookup(position, current); !rejected.isEmpty()) return rejected;
        const MarkerEntry before{position, current.duration().frames(fps), current.comment(), current.markerType()};
        MarkerEntry entry = before;
        if (auto rejected = markerFrame(command, QStringLiteral("newPosition"), entry.position, true); !rejected.isEmpty()) return rejected;
        if (auto rejected = markerFrame(command, QStringLiteral("duration"), entry.duration, true); !rejected.isEmpty()) return rejected;
        if (command.contains(QStringLiteral("comment"))) entry.comment = command.value(QStringLiteral("comment")).toString();
        if (command.contains(QStringLiteral("category"))) {
            if (auto rejected = markerCategory(command.value(QStringLiteral("category")), entry.category); !rejected.isEmpty()) return rejected;
        }
        const bool moved = entry.position != before.position;
        // A marker may already sit past the end of a shortened sequence; only a changed span has to fit.
        if (moved || entry.duration != before.duration) {
            if (auto rejected = markerSpan(entry, target); !rejected.isEmpty()) return rejected;
        } else if (entry.comment.size() > 4096) {
            return invalidMarker(QStringLiteral("Marker comments are limited to 4096 characters."));
        }
        if (moved && target.model->hasMarker(GenTime(entry.position, fps)))
            return error(QStringLiteral("MARKER_EXISTS"), QStringLiteral("Another %1 is already at frame %2.").arg(markerNoun(target, 1)).arg(entry.position));
        const bool changed = moved || entry.duration != before.duration || entry.comment != before.comment || entry.category != before.category;
        if (changed) {
            Fun undo = [] { return true; };
            Fun redo = [] { return true; };
            bool done = !moved || target.model->removeMarker(GenTime(before.position, fps), undo, redo);
            done = done && addMarkerTo(target.model, entry, fps, undo, redo);
            if (!done) {
                undo();
                return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native marker edit was rejected."));
            }
            pCore->pushUndo(undo, redo, QStringLiteral("%1 %2").arg(moved ? QStringLiteral("Move") : QStringLiteral("Edit"), markerNoun(target, 1)));
        }
        CommentedTime marker;
        lookup(entry.position, marker);
        return finish({{"marker", markerJson(marker, fps)}, {"previous", markerJson(current, fps)}, {"changed", changed}});
    }

    if (type == QLatin1String("marker_remove")) {
        if (!keys(command, {QStringLiteral("type"), QStringLiteral("binId"), QStringLiteral("position"), QStringLiteral("all"), QStringLiteral("category"),
                            QStringLiteral("range")}))
            return invalid(QStringLiteral("marker_remove takes optional binId and one of position, all, or category and/or range."));
        const bool single = command.contains(QStringLiteral("position"));
        const bool filtered = command.contains(QStringLiteral("category")) || command.contains(QStringLiteral("range"));
        if (command.contains(QStringLiteral("all")) && !command.value(QStringLiteral("all")).isBool()) return invalid(QStringLiteral("all must be a boolean."));
        const bool all = command.value(QStringLiteral("all")).toBool();
        if (int(single) + int(all) + int(filtered) != 1)
            return invalid(QStringLiteral("Pass exactly one selection: position, all:true, or category and/or range."));
        if (auto rejected = markerTarget(command, m_timeline, target); !rejected.isEmpty()) return rejected;
        QList<CommentedTime> matches;
        if (single) {
            int position = 0;
            if (auto rejected = markerFrame(command, QStringLiteral("position"), position, false); !rejected.isEmpty()) return rejected;
            CommentedTime marker;
            if (auto rejected = lookup(position, marker); !rejected.isEmpty()) return rejected;
            matches << marker;
        } else {
            int category = -1;
            if (command.contains(QStringLiteral("category"))) {
                if (auto rejected = markerCategory(command.value(QStringLiteral("category")), category); !rejected.isEmpty()) return rejected;
            }
            int start = 0;
            int end = std::numeric_limits<int>::max();
            if (command.contains(QStringLiteral("range"))) {
                const auto range = command.value(QStringLiteral("range")).toObject();
                if (!command.value(QStringLiteral("range")).isObject() || !keys(range, {QStringLiteral("start"), QStringLiteral("end")}) ||
                    !integer(range, QStringLiteral("start")) || !integer(range, QStringLiteral("end"), 1) ||
                    range.value(QStringLiteral("start")).toInt() >= range.value(QStringLiteral("end")).toInt())
                    return invalidMarker(QStringLiteral("range needs whole frames start < end (end exclusive)."));
                start = range.value(QStringLiteral("start")).toInt();
                end = range.value(QStringLiteral("end")).toInt();
            }
            for (const auto &marker : target.model->getAllMarkers(category)) {
                const int position = marker.time().frames(fps);
                if (position >= start && position < end) matches << marker;
            }
            if (matches.isEmpty()) return error(QStringLiteral("UNKNOWN_MARKER"), QStringLiteral("No %1 matches.").arg(markerNoun(target, 1)));
        }
        Fun undo = [] { return true; };
        Fun redo = [] { return true; };
        QJsonArray removed;
        for (const auto &marker : std::as_const(matches)) {
            if (!target.model->removeMarker(marker.time(), undo, redo)) {
                undo();
                return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native marker removal was rejected."));
            }
            removed.append(markerJson(marker, fps));
        }
        pCore->pushUndo(undo, redo, QStringLiteral("Remove %1").arg(markerNoun(target, matches.size())));
        return finish({{"removed", removed}, {"count", removed.size()}});
    }

    if (type == QLatin1String("marker_import")) {
        if (!keys(command, {QStringLiteral("type"), QStringLiteral("binId"), QStringLiteral("format"), QStringLiteral("text")}) ||
            !markerFormats().contains(command.value(QStringLiteral("format")).toString()) || !command.value(QStringLiteral("text")).isString())
            return invalid(QStringLiteral("marker_import takes format (json, csv or kdenlive), text and optional binId."));
        if (auto rejected = markerTarget(command, m_timeline, target); !rejected.isEmpty()) return rejected;
        QList<MarkerEntry> entries;
        if (auto rejected = parseMarkers(command.value(QStringLiteral("format")).toString(), command.value(QStringLiteral("text")).toString(), entries);
            !rejected.isEmpty())
            return rejected;
        QSet<int> positions;
        int replaced = 0;
        for (const auto &entry : std::as_const(entries)) {
            if (auto rejected = markerSpan(entry, target); !rejected.isEmpty()) return rejected;
            if (!positions.contains(entry.position) && target.model->hasMarker(GenTime(entry.position, fps))) ++replaced;
            positions.insert(entry.position);
        }
        Fun undo = [] { return true; };
        Fun redo = [] { return true; };
        for (const auto &entry : std::as_const(entries))
            if (!addMarkerTo(target.model, entry, fps, undo, redo)) {
                undo();
                return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native marker import was rejected."));
            }
        pCore->pushUndo(undo, redo, QStringLiteral("Import %1").arg(markerNoun(target, positions.size())));
        return finish({{"imported", int(positions.size())}, {"replaced", replaced}});
    }
    return error(QStringLiteral("UNSUPPORTED_COMMAND"), QStringLiteral("Read capabilities() for supported commands."));
}

QJsonObject LiveBridge::markerExport(const QJsonObject &arguments)
{
    if (!bind()) return error(QStringLiteral("NOT_READY"), QStringLiteral("No fully loaded active timeline."));
    const QString format = arguments.value(QStringLiteral("format")).toString();
    if (!keys(arguments, {QStringLiteral("format"), QStringLiteral("binId")}) || !markerFormats().contains(format) ||
        (arguments.contains(QStringLiteral("binId")) &&
         !QRegularExpression(QStringLiteral("^[0-9]+$")).match(arguments.value(QStringLiteral("binId")).toString()).hasMatch()))
        return error(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("Expected format (json, csv or kdenlive) and optional binId."));
    MarkerTarget target;
    if (auto rejected = markerTarget(arguments, m_timeline, target); !rejected.isEmpty()) return rejected;
    const double fps = pCore->getCurrentFps();
    const auto markers = target.model->getAllMarkers();
    QString text;
    if (format == QLatin1String("kdenlive")) {
        text = target.model->toJson();
    } else if (format == QLatin1String("json")) {
        QJsonArray list;
        for (const auto &marker : markers)
            list.append(markerJson(marker, fps));
        text = QString::fromUtf8(QJsonDocument(list).toJson(QJsonDocument::Indented));
    } else {
        QStringList lines{QStringLiteral("position,timecode,duration,category,categoryName,color,comment")};
        for (const auto &marker : markers) {
            const auto item = markerJson(marker, fps);
            const int position = item.value(QStringLiteral("position")).toInt();
            lines << QStringList{QString::number(position),
                                 pCore->timecode().getTimecodeFromFrames(position),
                                 QString::number(item.value(QStringLiteral("duration")).toInt()),
                                 QString::number(marker.markerType()),
                                 csvField(item.value(QStringLiteral("categoryName")).toString()),
                                 item.value(QStringLiteral("color")).toString(),
                                 csvField(marker.comment())}
                         .join(QLatin1Char(','));
        }
        text = lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
    }
    QJsonObject result{{"ok", true},
                       {"format", format},
                       {"target", target.guides ? QStringLiteral("guides") : QStringLiteral("clip")},
                       {"count", int(markers.size())},
                       {"text", text}};
    if (!target.guides) result.insert(QStringLiteral("binId"), arguments.value(QStringLiteral("binId")));
    return result;
}

QJsonObject LiveBridge::startRender(const QJsonObject &command)
{
    if (!keys(command, {QStringLiteral("type"), QStringLiteral("path"), QStringLiteral("preset")}) ||
        (command.contains(QStringLiteral("preset")) && !command.value(QStringLiteral("preset")).isString()))
        return error(QStringLiteral("INVALID_COMMAND"), QStringLiteral("render takes path and an optional preset."));
    if (pCore->projectDuration() < 2) return error(QStringLiteral("EMPTY_TIMELINE"), QStringLiteral("Nothing to render."));
    Wizard::fixKdenliveRenderPath();
    if (!QFile::exists(KdenliveSettings::meltpath()) || KdenliveSettings::kdenliverendererpath().isEmpty())
        return error(QStringLiteral("RENDERER_MISSING"), QStringLiteral("melt or kdenlive_render is not configured."));
    const QString presetName = command.value(QStringLiteral("preset")).toString(KdenliveSettings::renderProfile());
    if (!RenderPresetRepository::get()->presetExists(presetName))
        return error(QStringLiteral("UNKNOWN_PRESET"), QStringLiteral("Unknown render preset %1.").arg(presetName));
    auto &preset = RenderPresetRepository::get()->getPreset(presetName);
    if (!preset->error().isEmpty()) return error(QStringLiteral("UNKNOWN_PRESET"), preset->error());
    const QFileInfo output(command.value(QStringLiteral("path")).toString());
    if (!output.isAbsolute() || output.exists() || !QFileInfo(output.absolutePath()).isDir() || !QFileInfo(output.absolutePath()).isWritable())
        return error(QStringLiteral("INVALID_PATH"), QStringLiteral("Render needs a new absolute file path in a writable folder."));
    if (!preset->extension().isEmpty() && output.suffix().compare(preset->extension(), Qt::CaseInsensitive) != 0)
        return error(QStringLiteral("INVALID_PATH"), QStringLiteral("Preset %1 writes .%2 files.").arg(presetName, preset->extension()));
    for (auto it = m_renders.cbegin(); it != m_renders.cend(); ++it)
        if (it.value().status == QLatin1String("running") || it.value().status == QLatin1String("starting"))
            return error(QStringLiteral("RENDER_BUSY"), QStringLiteral("Wait for %1 to finish rendering.").arg(it.key()));
    if (!m_renderTracking) {
        auto *server = pCore->window()->findChild<RenderServer *>();
        if (!server) return error(QStringLiteral("RENDERER_MISSING"), QStringLiteral("Render progress server is unavailable."));
        connect(server, &RenderServer::setRenderingProgress, this, [this](const QString &url, int progress, int frame) {
            auto it = m_renders.find(url);
            if (it == m_renders.end()) return;
            it->status = QStringLiteral("running");
            it->progress = progress;
            it->frame = frame;
        });
        connect(server, &RenderServer::setRenderingFinished, this, [this](const QString &url, int status, const QString &message) {
            auto it = m_renders.find(url);
            if (it == m_renders.end()) return;
            it->status = status == -1 ? QStringLiteral("finished") : status == -3 ? QStringLiteral("aborted") : QStringLiteral("failed");
            if (status == -1) it->progress = 100;
            it->error = message;
        });
        m_renderTracking = true;
    }
    RenderRequest request;
    request.setOutputFile(output.absoluteFilePath());
    request.loadPresetParams(presetName);
    request.setDelayedRendering(false);
    request.setProxyRendering(false);
    request.setEmbedSubtitles(false);
    request.setTwoPass(false);
    request.setAudioFilePerTrack(false);
    request.setOverlayData(QString());
    const auto jobs = request.process();
    if (jobs.empty() || !request.errorMessages().isEmpty())
        return error(QStringLiteral("RENDER_FAILED"), request.errorMessages().join(QLatin1Char('\n')).isEmpty()
                                                          ? QStringLiteral("No render job was created.")
                                                          : request.errorMessages().join(QLatin1Char('\n')));
    QJsonArray outputs;
    for (const auto &job : jobs) {
        QProcess process;
        process.setProgram(KdenliveSettings::kdenliverendererpath());
        process.setArguments(RenderRequest::argsByJob(job, true));
        RenderState state{presetName, QStringLiteral("starting"), 0, 0, {}, QDateTime::currentSecsSinceEpoch()};
        if (!process.startDetached()) {
            state.status = QStringLiteral("failed");
            state.error = QStringLiteral("Cannot start kdenlive_render.");
        }
        m_renders.insert(job.outputPath, state);
        outputs.append(job.outputPath);
    }
    return {{"ok", true}, {"outputs", outputs}, {"preset", presetName}};
}

QJsonObject LiveBridge::renderStatus()
{
    QJsonArray jobs;
    for (auto it = m_renders.cbegin(); it != m_renders.cend(); ++it) {
        QJsonObject job{{"path", it.key()},         {"preset", it->preset}, {"status", it->status},
                        {"progress", it->progress}, {"frame", it->frame},   {"elapsedSeconds", QDateTime::currentSecsSinceEpoch() - it->started}};
        if (!it->error.isEmpty()) job.insert(QStringLiteral("error"), it->error);
        if (it->status == QLatin1String("finished")) job.insert(QStringLiteral("bytes"), QFileInfo(it.key()).size());
        jobs.append(job);
    }
    return {{"ok", true}, {"jobs", jobs}};
}

QJsonObject LiveBridge::frameCapture(const QJsonObject &arguments)
{
    if (!bind()) return error(QStringLiteral("NOT_READY"), QStringLiteral("No fully loaded active timeline."));
    if (!keys(arguments, {QStringLiteral("position"), QStringLiteral("width")}) || !integer(arguments, QStringLiteral("position")) ||
        (arguments.contains(QStringLiteral("width")) && !integer(arguments, QStringLiteral("width"), 64)))
        return error(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("Expected position and an optional width of at least 64."));
    const int position = arguments.value(QStringLiteral("position")).toInt();
    if (position >= pCore->projectDuration()) return error(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("Position is past the end of the timeline."));
    auto &profile = pCore->getProjectProfile();
    int width = std::min(arguments.value(QStringLiteral("width")).toInt(540), profile.width());
    width -= width % 2;
    int height = qRound(width * double(profile.height()) / profile.width());
    height += height % 2;
    // Render a private copy of the sequence so the user's playhead and monitor are untouched.
    const QString scene = pCore->projectManager()->projectSceneList(QString(), true).first;
    QImage image;
    {
        QReadLocker lock(&pCore->xmlMutex);
        Mlt::Producer producer(profile, "xml-string", scene.toUtf8().constData());
        if (!producer.is_valid()) return error(QStringLiteral("CAPTURE_FAILED"), QStringLiteral("Cannot build the sequence for capture."));
        image = KThumb::getFrame(&producer, position, width, height);
    }
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (image.isNull() || !image.save(&buffer, "PNG")) return error(QStringLiteral("CAPTURE_FAILED"), QStringLiteral("Cannot encode the frame."));
    return {{"ok", true},
            {"position", position},
            {"width", image.width()},
            {"height", image.height()},
            {"image", QJsonObject{{"data", QString::fromLatin1(png.toBase64())}, {"mimeType", "image/png"}}}};
}

QJsonObject LiveBridge::effectList(const QJsonObject &arguments)
{
    if (!bind()) return error(QStringLiteral("NOT_READY"), QStringLiteral("No fully loaded active timeline."));
    if (!keys(arguments, {QStringLiteral("clipId"), QStringLiteral("binId"), QStringLiteral("query")}) ||
        (arguments.contains(QStringLiteral("query")) && !arguments.value(QStringLiteral("query")).isString()))
        return error(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("Expected clipId or binId and/or query."));
    QJsonObject result{{"ok", true}};
    const bool target = arguments.contains(QStringLiteral("clipId")) || arguments.contains(QStringLiteral("binId"));
    if (target) {
        std::shared_ptr<EffectStackModel> stack;
        int origin = 0;
        // Reading needs no lock check, but shares target resolution with edits.
        auto lookup = arguments;
        lookup.remove(QStringLiteral("query"));
        if (auto rejected = effectTarget(lookup, m_timeline, stack, origin);
            !rejected.isEmpty() && rejected.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")) != QLatin1String("TRACK_LOCKED"))
            return rejected;
        if (!stack) stack = m_timeline->getClipEffectStackModel(arguments.value(QStringLiteral("clipId")).toInt());
        if (arguments.contains(QStringLiteral("clipId"))) origin = m_timeline->getClipInOut(arguments.value(QStringLiteral("clipId")).toInt()).first;
        QJsonArray effects;
        for (int row = 0; row < stack->rowCount(); ++row)
            if (auto effect = effectAt(stack, row)) effects.append(effectJson(row, effect));
        result.insert(QStringLiteral("keyframeOrigin"), origin);
        result.insert(QStringLiteral("effects"), effects);
    }
    if (arguments.contains(QStringLiteral("query")) || !target) {
        const QString query = arguments.value(QStringLiteral("query")).toString();
        QJsonArray catalog;
        for (const auto &entry : EffectsRepository::get()->getNames()) {
            const auto type = EffectsRepository::get()->getType(entry.first);
            if (type == AssetListType::AssetType::Hidden) continue;
            if (!query.isEmpty() && !entry.first.contains(query, Qt::CaseInsensitive) && !entry.second.contains(query, Qt::CaseInsensitive)) continue;
            catalog.append(QJsonObject{{"effectId", entry.first}, {"name", entry.second}, {"audio", EffectsRepository::get()->isAudioEffect(entry.first)}});
            if (catalog.size() >= 100) break;
        }
        result.insert(QStringLiteral("catalog"), catalog);
    }
    return result;
}

QJsonObject LiveBridge::titleRead(const QJsonObject &arguments)
{
    if (!bind()) return error(QStringLiteral("NOT_READY"), QStringLiteral("No fully loaded active timeline."));
    if (!keys(arguments, {QStringLiteral("binId")}) || !arguments.value(QStringLiteral("binId")).isString())
        return error(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("Expected binId."));
    auto clip = pCore->projectItemModel()->getClipByBinID(arguments.value(QStringLiteral("binId")).toString());
    if (!clip || !clip->statusReady() || clip->clipType() != ClipType::Text)
        return error(QStringLiteral("NOT_A_TITLE"), QStringLiteral("binId must be a loaded title clip."));
    QDomDocument xml;
    if (!xml.setContent(clip->getProducerProperty(QStringLiteral("xmldata"))))
        return error(QStringLiteral("INVALID_TITLE"), QStringLiteral("Title XML cannot be parsed."));
    const auto root = xml.documentElement();
    const auto items = root.elementsByTagName(QStringLiteral("item"));
    QJsonArray list;
    for (int i = 0; i < items.count(); ++i) {
        const auto item = items.at(i).toElement();
        const auto position = item.firstChildElement(QStringLiteral("position"));
        QJsonObject entry{{"index", i},
                          {"type", item.attribute(QStringLiteral("type"))},
                          {"x", position.attribute(QStringLiteral("x")).toDouble()},
                          {"y", position.attribute(QStringLiteral("y")).toDouble()}};
        if (item.attribute(QStringLiteral("type")) == QLatin1String("QGraphicsTextItem")) {
            const auto content = item.firstChildElement(QStringLiteral("content"));
            const int alignment = content.attribute(QStringLiteral("alignment")).toInt();
            entry.insert(QStringLiteral("text"), content.text());
            entry.insert(QStringLiteral("font"), content.attribute(QStringLiteral("font")));
            entry.insert(QStringLiteral("fontPixelSize"), content.attribute(QStringLiteral("font-pixel-size")).toInt());
            entry.insert(QStringLiteral("boxWidth"), content.attribute(QStringLiteral("box-width")).toDouble());
            entry.insert(QStringLiteral("boxHeight"), content.attribute(QStringLiteral("box-height")).toDouble());
            entry.insert(QStringLiteral("alignment"), (alignment & Qt::AlignRight)     ? QStringLiteral("right")
                                                      : (alignment & Qt::AlignHCenter) ? QStringLiteral("center")
                                                                                       : QStringLiteral("left"));
        }
        list.append(entry);
    }
    return {{"ok", true},
            {"binId", arguments.value(QStringLiteral("binId")).toString()},
            {"width", root.attribute(QStringLiteral("width")).toInt()},
            {"height", root.attribute(QStringLiteral("height")).toInt()},
            {"projectWidth", pCore->getCurrentFrameSize().width()},
            {"projectHeight", pCore->getCurrentFrameSize().height()},
            {"items", list}};
}

// Change log. m_history mirrors the shared QUndoStack row by row, with the origin of each command: "mcp" when it was pushed while an edit
// request ran, "user" when it appeared at any other time, "unknown" when the bridge did not see it being pushed.
void LiveBridge::attachHistory(KdenliveDoc *document)
{
    // Keep the table across sequence switches and transient states without a document; only a different stack resets it.
    if (!document) return;
    QUndoStack *stack = document->commandStack().get();
    if (stack == m_historyStack.data()) return;
    for (const auto &connection : std::as_const(m_historyConnections))
        disconnect(connection);
    m_historyConnections.clear();
    m_history.clear();
    m_historySeeded = false;
    m_historyStack = stack;
    m_historyConnections << connect(stack, &QUndoStack::indexChanged, this, [this] { syncHistory(); });
    m_historyConnections << connect(stack, &QUndoStack::cleanChanged, this, [this] { syncHistory(); });
    syncHistory();
}

void LiveBridge::syncHistory()
{
    if (m_historyStack.isNull()) {
        m_history.clear();
        return;
    }
    QUndoStack *stack = m_historyStack.data();
    const int count = stack->count();
    // Commands are compared by pointer only and never dereferenced from the table: a pushed command is allocated before the redo
    // commands it replaces are deleted, so a replaced row always shows a different pointer.
    if (!m_history.isEmpty() && count > 0 && m_history.constFirst().command != stack->command(0)) {
        // The undo limit drops the oldest commands; any other mismatch means the whole stack was replaced.
        int shift = -1;
        for (int row = 1; row < m_history.size() && shift < 0; ++row)
            if (m_history.at(row).command == stack->command(0)) shift = row;
        if (shift > 0)
            m_history.remove(0, shift);
        else
            m_history.clear();
    }
    int matched = 0;
    while (matched < m_history.size() && matched < count && m_history.at(matched).command == stack->command(matched)) {
        m_history[matched].text = stack->command(matched)->text();
        ++matched;
    }
    // A push after Undo discards the redo rows; clear() and deleted obsolete commands shrink the stack.
    m_history.resize(matched);
    // Each push emits indexChanged, so more than one new row at once means the bridge missed them.
    const bool missed = count - matched > 1;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int row = matched; row < count; ++row) {
        HistoryEntry entry;
        entry.command = stack->command(row);
        entry.text = entry.command->text();
        entry.time = now;
        if (m_request) {
            entry.origin = QStringLiteral("mcp");
            entry.sessionId = m_session;
            entry.requestId = m_request->requestId;
            entry.type = m_request->type;
            entry.caller = m_request->caller;
            entry.revisionBefore = m_request->revisionBefore;
            entry.revisionAfter = m_request->revisionBefore;
            m_request->created.append(entry.command);
        } else {
            entry.origin = m_historySeeded && !missed ? QStringLiteral("user") : QStringLiteral("unknown");
        }
        m_history.append(entry);
    }
    m_historySeeded = true;
}

namespace {
QString isoTime(qint64 msecs)
{
    return QDateTime::fromMSecsSinceEpoch(msecs, QTimeZone::UTC).toString(Qt::ISODateWithMs);
}

/** Ids and positions a command touched, from its own fields and its result. A command can add more with a historySummary object in its result. */
QJsonObject historySummary(const QJsonObject &command, const QJsonObject &result)
{
    static const QStringList commandKeys = QStringLiteral("clipId binId trackId position newPosition duration edge media sourceIn sourceOut "
                                                          "replacementBinId effectId index name path width height mode format category all range "
                                                          "count toIndex revertSession start end trackIds allTracks clipIds groupId group linked "
                                                          "audioTrackId speed pitchCompensation enabled")
                                               .split(QLatin1Char(' '));
    static const QStringList resultKeys =
        QStringLiteral("clipId binId actualDuration effectIndex count imported replaced existing steps undoIndex documentUrl mode linked audioClipId groupId "
                       "leftClipId rightClipId")
            .split(QLatin1Char(' '));
    QJsonObject summary;
    for (const auto &key : commandKeys)
        if (command.contains(key)) summary.insert(key, command.value(key));
    for (const auto &key : resultKeys)
        if (result.contains(key)) summary.insert(key, result.value(key));
    if (result.value(QStringLiteral("marker")).isObject()) {
        const auto marker = result.value(QStringLiteral("marker")).toObject();
        summary.insert(QStringLiteral("marker"),
                       QJsonObject{{"position", marker.value(QStringLiteral("position"))}, {"duration", marker.value(QStringLiteral("duration"))}});
    }
    const auto extra = result.value(QStringLiteral("historySummary")).toObject();
    for (auto it = extra.begin(); it != extra.end(); ++it)
        summary.insert(it.key(), it.value());
    return summary;
}
} // namespace

void LiveBridge::finishRequest(const QJsonObject &command, const QJsonObject &result)
{
    if (!m_request) return;
    syncHistory();
    const QString type = m_request->type;
    QJsonObject summary = historySummary(command, result);
    QJsonArray children;
    if (type == QLatin1String("batch")) {
        // The batch plan and its receipts, not the macro's QUndoStack children.
        const auto commands = command.value(QStringLiteral("commands")).toArray();
        const auto results = result.value(QStringLiteral("results")).toArray();
        for (int i = 0; i < commands.size() && i < results.size(); ++i) {
            const auto inner = commands.at(i).toObject();
            children.append(QJsonObject{{"type", inner.value(QStringLiteral("type"))}, {"summary", historySummary(inner, results.at(i).toObject())}});
        }
        summary = QJsonObject{{"commands", int(commands.size())}};
    }
    const auto base = [this](const HistoryEntry &entry) {
        QJsonObject line{{"documentUrl", m_document ? m_document->url().toString() : QString()},
                         {"sessionId", entry.sessionId},
                         {"requestId", entry.requestId},
                         {"command", entry.type},
                         {"client", entry.caller.client},
                         {"revisionBefore", entry.revisionBefore},
                         {"revisionAfter", entry.revisionAfter},
                         {"timestamp", isoTime(entry.time)}};
        if (!entry.caller.tool.isEmpty()) line.insert(QStringLiteral("tool"), entry.caller.tool);
        if (!entry.caller.clientName.isEmpty()) line.insert(QStringLiteral("clientName"), entry.caller.clientName);
        return line;
    };
    bool logged = false;
    for (int row = 0; row < m_history.size(); ++row) {
        auto &entry = m_history[row];
        if (!m_request->created.contains(entry.command)) continue;
        entry.revisionAfter = m_revision;
        entry.summary = summary;
        entry.children = children;
        auto line = base(entry);
        line.insert(QStringLiteral("event"), QStringLiteral("edit"));
        line.insert(QStringLiteral("undoIndex"), row + 1);
        line.insert(QStringLiteral("text"), historyLabel(entry.text));
        line.insert(QStringLiteral("summary"), summary);
        if (!children.isEmpty()) line.insert(QStringLiteral("children"), children);
        writeLog(line);
        logged = true;
    }
    // History moves and changes outside the Undo stack are logged as events.
    static const QStringList events{QStringLiteral("undo"),    QStringLiteral("redo"),   QStringLiteral("save"),
                                    QStringLiteral("save_as"), QStringLiteral("render"), QStringLiteral("set_profile")};
    if (logged || !result.value(QStringLiteral("ok")).toBool() || !events.contains(type)) return;
    HistoryEntry event;
    event.sessionId = m_session;
    event.requestId = m_request->requestId;
    event.type = type;
    event.caller = m_request->caller;
    event.revisionBefore = m_request->revisionBefore;
    event.revisionAfter = m_revision;
    event.time = QDateTime::currentMSecsSinceEpoch();
    auto line = base(event);
    line.insert(QStringLiteral("event"), type);
    line.insert(QStringLiteral("summary"), summary);
    const auto moved = result.value(type == QLatin1String("undo") ? QStringLiteral("undone") : QStringLiteral("redone")).toArray();
    if (!moved.isEmpty()) {
        QJsonArray entries;
        for (const auto &item : moved)
            entries.append(QJsonObject{{"undoIndex", item.toObject().value(QStringLiteral("undoIndex"))},
                                       {"text", item.toObject().value(QStringLiteral("text"))},
                                       {"origin", item.toObject().value(QStringLiteral("origin"))}});
        line.insert(QStringLiteral("entries"), entries);
    }
    writeLog(line);
}

QJsonObject LiveBridge::historyJson(int row) const
{
    const auto &entry = m_history.at(row);
    const int index = m_historyStack.isNull() ? 0 : m_historyStack->index();
    QJsonObject result{{"undoIndex", row + 1},   {"text", historyLabel(entry.text)},
                       {"rawText", entry.text},  {"state", row < index ? QStringLiteral("applied") : QStringLiteral("undone")},
                       {"origin", entry.origin}, {"timestamp", isoTime(entry.time)}};
    if (entry.origin != QLatin1String("mcp")) return result;
    result.insert(QStringLiteral("sessionId"), entry.sessionId);
    result.insert(QStringLiteral("requestId"), entry.requestId);
    result.insert(QStringLiteral("command"), entry.type);
    if (!entry.caller.tool.isEmpty()) result.insert(QStringLiteral("tool"), entry.caller.tool);
    result.insert(QStringLiteral("client"), entry.caller.client);
    if (!entry.caller.clientName.isEmpty()) result.insert(QStringLiteral("clientName"), entry.caller.clientName);
    result.insert(QStringLiteral("revisionBefore"), entry.revisionBefore);
    result.insert(QStringLiteral("revisionAfter"), entry.revisionAfter);
    result.insert(QStringLiteral("summary"), entry.summary);
    if (!entry.children.isEmpty()) result.insert(QStringLiteral("children"), entry.children);
    return result;
}

QJsonObject LiveBridge::history(const QJsonObject &arguments)
{
    if (!bind()) return QJsonDocument::fromJson(failure(QStringLiteral("NOT_READY"), QStringLiteral("No fully loaded active timeline.")).toUtf8()).object();
    syncHistory();
    const auto invalid = [](const QString &message) { return error(QStringLiteral("INVALID_ARGUMENTS"), message); };
    if (!keys(arguments,
              {QStringLiteral("limit"), QStringLiteral("origin"), QStringLiteral("sessionId"), QStringLiteral("since"), QStringLiteral("includeUndone")}))
        return invalid(QStringLiteral("Expected optional limit, origin, sessionId, since and includeUndone."));
    int limit = 50;
    if (arguments.contains(QStringLiteral("limit"))) {
        if (!integer(arguments, QStringLiteral("limit"), 1) || arguments.value(QStringLiteral("limit")).toInt() > 1000)
            return invalid(QStringLiteral("limit must be 1 to 1000."));
        limit = arguments.value(QStringLiteral("limit")).toInt();
    }
    const QString origin = arguments.value(QStringLiteral("origin")).toString(QStringLiteral("all"));
    if (!QStringList{QStringLiteral("all"), QStringLiteral("mcp"), QStringLiteral("user"), QStringLiteral("unknown")}.contains(origin) ||
        (arguments.contains(QStringLiteral("origin")) && !arguments.value(QStringLiteral("origin")).isString()))
        return invalid(QStringLiteral("origin must be all, mcp, user or unknown."));
    const auto sessionValue = arguments.value(QStringLiteral("sessionId"));
    if (arguments.contains(QStringLiteral("sessionId")) && (!sessionValue.isString() || sessionValue.toString().isEmpty()))
        return invalid(QStringLiteral("sessionId must be a session id string."));
    int sinceIndex = -1;
    qint64 sinceTime = -1;
    if (arguments.contains(QStringLiteral("since"))) {
        const auto since = arguments.value(QStringLiteral("since"));
        if (since.isString()) {
            const auto time = QDateTime::fromString(since.toString(), Qt::ISODateWithMs);
            if (!time.isValid()) return invalid(QStringLiteral("since must be an undoIndex or an ISO 8601 time."));
            sinceTime = time.toMSecsSinceEpoch();
        } else if (integer(arguments, QStringLiteral("since"))) {
            sinceIndex = since.toInt();
        } else {
            return invalid(QStringLiteral("since must be an undoIndex or an ISO 8601 time."));
        }
    }
    const auto undone = arguments.value(QStringLiteral("includeUndone"));
    if (arguments.contains(QStringLiteral("includeUndone")) && !undone.isBool()) return invalid(QStringLiteral("includeUndone must be a boolean."));
    const bool includeUndone = undone.toBool(true);
    const int index = m_historyStack.isNull() ? 0 : m_historyStack->index();
    QList<int> rows;
    QJsonObject counts{{"applied", 0}, {"undone", 0}, {"mcp", 0}, {"user", 0}, {"unknown", 0}};
    const auto bump = [&counts](const QString &key) { counts.insert(key, counts.value(key).toInt() + 1); };
    for (int row = 0; row < m_history.size(); ++row) {
        const auto &entry = m_history.at(row);
        bump(row < index ? QStringLiteral("applied") : QStringLiteral("undone"));
        bump(entry.origin);
        if (origin != QLatin1String("all") && entry.origin != origin) continue;
        if (sessionValue.isString() && entry.sessionId != sessionValue.toString()) continue;
        if (row + 1 <= sinceIndex || (sinceTime >= 0 && entry.time < sinceTime)) continue;
        if (!includeUndone && row >= index) continue;
        rows.append(row);
    }
    QJsonArray entries;
    for (int i = qMax(0, int(rows.size()) - limit); i < rows.size(); ++i)
        entries.append(historyJson(rows.at(i)));
    const QString log = logPath();
    return {{"ok", true},
            {"sessionId", m_session},
            {"revision", m_revision},
            {"undo", QJsonObject{{"index", index}, {"count", int(m_history.size())}}},
            {"entries", entries},
            {"matched", int(rows.size())},
            {"counts", counts},
            {"logFile", log.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(log)}};
}

QJsonObject LiveBridge::executeHistoryStep(const QJsonObject &command)
{
    const bool undo = command.value(QStringLiteral("type")).toString() == QLatin1String("undo");
    QStringList allowed{QStringLiteral("type"), QStringLiteral("count"), QStringLiteral("toIndex")};
    if (undo) allowed << QStringLiteral("revertSession");
    const auto invalid = [] { return error(QStringLiteral("INVALID_COMMAND"), QStringLiteral("Pass at most one of count, toIndex or revertSession: true.")); };
    const int selectors = int(command.contains(QStringLiteral("count"))) + int(command.contains(QStringLiteral("toIndex"))) +
                          int(command.contains(QStringLiteral("revertSession")));
    if (!keys(command, allowed) || selectors > 1) return invalid();
    syncHistory();
    auto stack = m_document->commandStack();
    const int index = stack->index();
    int target;
    if (command.contains(QStringLiteral("revertSession"))) {
        if (command.value(QStringLiteral("revertSession")) != QJsonValue(true)) return invalid();
        // This connection's edits in the current editing session; anything else above the oldest of them blocks the revert.
        const auto own = [this](const HistoryEntry &entry) {
            return entry.origin == QLatin1String("mcp") && entry.sessionId == m_session && m_request && entry.caller.client == m_request->caller.client;
        };
        int lowest = -1;
        for (int row = 0; row < index && row < m_history.size() && lowest < 0; ++row)
            if (own(m_history.at(row))) lowest = row;
        if (lowest < 0)
            return error(QStringLiteral("EMPTY_HISTORY"), QStringLiteral("This MCP connection has no applied edits in the current editing session."));
        QJsonArray blocking;
        for (int row = lowest; row < index && row < m_history.size(); ++row)
            if (!own(m_history.at(row))) blocking.append(historyJson(row));
        if (!blocking.isEmpty()) {
            auto refused = error(QStringLiteral("HISTORY_INTERLEAVED"),
                                 QStringLiteral("Other edits are interleaved with this connection's edits; undo with toIndex after confirming with the user."));
            refused.insert(QStringLiteral("blocking"), blocking);
            refused.insert(QStringLiteral("toIndex"), lowest);
            return refused;
        }
        target = lowest;
    } else if (command.contains(QStringLiteral("toIndex"))) {
        if (!integer(command, QStringLiteral("toIndex"))) return invalid();
        target = command.value(QStringLiteral("toIndex")).toInt();
        if (undo ? target > index : (target < index || target > stack->count()))
            return error(QStringLiteral("INVALID_ARGUMENTS"),
                         undo ? QStringLiteral("toIndex must be between 0 and the current undo index %1.").arg(index)
                              : QStringLiteral("toIndex must be between the current undo index %1 and %2.").arg(index).arg(stack->count()));
    } else {
        if (command.contains(QStringLiteral("count")) && !integer(command, QStringLiteral("count"), 1)) return invalid();
        const int steps = command.value(QStringLiteral("count")).toInt(1);
        const int available = undo ? index : stack->count() - index;
        if (available == 0) return error(QStringLiteral("EMPTY_HISTORY"), QStringLiteral("No operation to undo or redo."));
        if (steps > available)
            return error(QStringLiteral("EMPTY_HISTORY"),
                         QStringLiteral("Only %1 step(s) can be %2.").arg(available).arg(undo ? QStringLiteral("undone") : QStringLiteral("redone")));
        target = undo ? index - steps : index + steps;
    }
    // Describe the entries before moving: an obsolete command is deleted when undone.
    QJsonArray moved;
    for (int row = undo ? index - 1 : index; undo ? row >= target : row < target; undo ? --row : ++row) {
        if (row < 0 || row >= m_history.size()) continue;
        auto entry = historyJson(row);
        entry.insert(QStringLiteral("state"), undo ? QStringLiteral("undone") : QStringLiteral("applied"));
        moved.append(entry);
    }
    while (stack->index() > target) {
        const int before = stack->index();
        stack->undo();
        if (stack->index() == before) break;
    }
    while (stack->index() < target && stack->canRedo()) {
        const int before = stack->index();
        stack->redo();
        if (stack->index() == before) break;
    }
    return {{"ok", true},
            {"steps", qAbs(index - stack->index())},
            {"undoIndex", stack->index()},
            {undo ? QStringLiteral("undone") : QStringLiteral("redone"), moved}};
}

QString LiveBridge::logPath() const
{
    if (!KdenliveSettings::mcpWriteLog() || !m_document) return {};
    const QUrl url = m_document->url();
    // Untitled projects are never logged.
    if (url.isEmpty() || !url.isLocalFile()) return {};
    const QFileInfo folder(QFileInfo(url.toLocalFile()).absolutePath());
    if (folder.isDir() && folder.isWritable()) return folder.absoluteFilePath() + QStringLiteral("/.kdenlive-mcp-log.jsonl");
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/mcp-log.jsonl");
}

void LiveBridge::writeLog(const QJsonObject &line)
{
    const QString path = logPath();
    if (path.isEmpty()) return;
    const QByteArray data = QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n';
    const auto append = [&data](const QString &target) {
        QDir().mkpath(QFileInfo(target).absolutePath());
        QFile file(target);
        return file.open(QIODevice::Append | QIODevice::Text) && file.write(data) == data.size();
    };
    // A project folder that refuses the write falls back to the data directory.
    const QString fallback = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/mcp-log.jsonl");
    if (!append(path) && path != fallback) append(fallback);
}
