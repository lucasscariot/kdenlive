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
#include "mainwindow.h"
#include "profiles/profilemodel.hpp"
#include "profiles/profilerepository.hpp"
#include "project/projectmanager.h"
#include "render/renderrequest.h"
#include "render/renderserver.h"
#include "renderpresets/renderpresetmodel.hpp"
#include "renderpresets/renderpresetrepository.hpp"
#include "timeline2/model/compositionmodel.hpp"
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
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QSet>
#include <QTimer>
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
    static const QStringList result{QStringLiteral("tracks"),    QStringLiteral("clips"),     QStringLiteral("compositions"),
                                    QStringLiteral("markers"),   QStringLiteral("subtitles"), QStringLiteral("bin"),
                                    QStringLiteral("sequences"), QStringLiteral("effects"),   QStringLiteral("media")};
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
                 {"operations", QJsonArray{"import",     "remove_asset",  "remove_clip", "replace_media", "audio_envelope", "rename_track",  "save",
                                           "save_as",    "set_profile",   "insert",      "move",          "trim",           "reframe",       "effect_add",
                                           "effect_set", "effect_remove", "title_edit",  "marker_add",    "marker_edit",    "marker_remove", "marker_import",
                                           "render",     "batch",         "undo",        "redo"}},
                 {"insertModes", QJsonArray{"video", "audio"}},
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
    return json(snapshot(defaultScope()));
}

QString LiveBridge::apply(const QString &request)
{
    return applyAuthorized(request, {});
}

QString LiveBridge::applyAuthorized(const QString &request, const std::function<QJsonObject()> &authorize)
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
    auto result = execute(object.value(QStringLiteral("command")).toObject());
    if (result.value(QStringLiteral("ok")).toBool()) {
        // Even a successful no-op gets a fresh revision, preventing replay after receipt eviction.
        contentChanged();
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
    if (type == QLatin1String("remove_clip")) {
        if (!keys(command, {QStringLiteral("type"), QStringLiteral("clipId")}) || !integer(command, QStringLiteral("clipId"))) return invalid();
        const int id = command.value(QStringLiteral("clipId")).toInt();
        if (!m_timeline->isClip(id)) return error(QStringLiteral("UNKNOWN_CLIP"), QStringLiteral("Timeline clip does not exist."));
        if (m_timeline->isInGroup(id)) return error(QStringLiteral("GROUPED_CLIP"), QStringLiteral("Ungroup the clip before removing it individually."));
        const int track = m_timeline->getClipTrackId(id);
        if (m_timeline->data(m_timeline->makeTrackIndexFromID(track), TimelineModel::IsLockedRole).toBool())
            return error(QStringLiteral("TRACK_LOCKED"), QStringLiteral("Track is locked."));
        if (!m_timeline->requestItemDeletion(id, true)) return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native clip removal was rejected."));
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
const QStringList batchable{QStringLiteral("import"),       QStringLiteral("remove_asset"), QStringLiteral("remove_clip"), QStringLiteral("audio_envelope"),
                            QStringLiteral("rename_track"), QStringLiteral("insert"),       QStringLiteral("move"),        QStringLiteral("trim"),
                            QStringLiteral("reframe"),      QStringLiteral("effect_add"),   QStringLiteral("effect_set"),  QStringLiteral("effect_remove"),
                            QStringLiteral("title_edit"),   QStringLiteral("marker_add"),   QStringLiteral("marker_edit"), QStringLiteral("marker_remove"),
                            QStringLiteral("marker_import")};

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
