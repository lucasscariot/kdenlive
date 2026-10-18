/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
// Composition helpers for agents assembling a film: replacing a timeline clip in place, fades, music ducking, project creation and
// frame-rate conversion, bin readiness waits, the profile catalog and V1/A2 track tags.
#include "livebridge.h"

#include "assets/model/assetcommand.hpp"
#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "doc/docundostack.hpp"
#include "doc/kdenlivedoc.h"
#include "effects/effectstack/model/effectitemmodel.hpp"
#include "effects/effectstack/model/effectstackmodel.hpp"
#include "kdenlivesettings.h"
#include "macros.hpp"
#include "mainwindow.h"
#include "profiles/profilemodel.hpp"
#include "profiles/profilerepository.hpp"
#include "project/projectmanager.h"
#include "timeline2/model/timelineitemmodel.hpp"
#include "utils/qstringutils.h"
#include "xml/xml.hpp"

#include <KAutoSaveFile>
#include <QDomDocument>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <mlt++/MltProfile.h>
#include <numeric>

namespace {
QJsonObject error(const QString &code, const QString &message)
{
    return {{"ok", false}, {"error", QJsonObject{{"code", code}, {"message", message}}}};
}

QJsonObject invalid(const QString &message)
{
    return error(QStringLiteral("INVALID_COMMAND"), message);
}

bool integer(const QJsonObject &object, const QString &key, int minimum = 0)
{
    const auto value = object.value(key);
    const double number = value.toDouble(-1);
    return value.isDouble() && std::isfinite(number) && number >= minimum && number <= std::numeric_limits<int>::max() && std::floor(number) == number;
}

bool keys(const QJsonObject &object, const QStringList &allowed)
{
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!allowed.contains(it.key())) return false;
    return true;
}

bool optionalBool(const QJsonObject &object, const QString &key, bool &value)
{
    if (!object.contains(key)) return true;
    if (!object.value(key).isBool()) return false;
    value = object.value(key).toBool();
    return true;
}

bool optionalNumber(const QJsonObject &object, const QString &key, double minimum, double maximum, double &value)
{
    if (!object.contains(key)) return true;
    const auto number = object.value(key);
    if (!number.isDouble() || !std::isfinite(number.toDouble()) || number.toDouble() < minimum || number.toDouble() > maximum) return false;
    value = number.toDouble();
    return true;
}

template <typename Step> bool runStep(Fun &undo, Fun &redo, Step &&step)
{
    Fun localUndo = [] { return true; };
    Fun localRedo = [] { return true; };
    if (!step(localUndo, localRedo)) return false;
    UPDATE_UNDO_REDO_NOLOCK(localRedo, localUndo, undo, redo);
    return true;
}

/** The user-added effects of a stack as the XML fromXml reads, keyframes relative to parentIn. Built-in effects belong to each clip already. */
QDomElement userEffectsXml(QDomDocument &document, const std::shared_ptr<EffectStackModel> &stack, int parentIn, int &count)
{
    auto container = document.createElement(QStringLiteral("effects"));
    container.setAttribute(QStringLiteral("parentIn"), parentIn);
    count = 0;
    for (int row = 0; row < stack->rowCount(); ++row) {
        auto effect = std::static_pointer_cast<EffectItemModel>(stack->getEffectStackRow(row));
        if (!effect || effect->isBuiltIn()) continue;
        auto node = document.createElement(QStringLiteral("effect"));
        node.setAttribute(QStringLiteral("id"), effect->getAssetId());
        const int in = effect->filter().get_int("in");
        const int out = effect->filter().get_int("out");
        if (out > in) {
            node.setAttribute(QStringLiteral("in"), in);
            node.setAttribute(QStringLiteral("out"), out);
        }
        if (!effect->isAssetEnabled()) Xml::setXmlProperty(node, QStringLiteral("disable"), QStringLiteral("1"));
        for (const auto &param : effect->getAllParameters())
            Xml::setXmlProperty(node, param.first, param.second.toString());
        container.appendChild(node);
        ++count;
    }
    return container;
}

QJsonArray idArray(const QList<int> &ids)
{
    QJsonArray result;
    for (int id : ids)
        result.append(id);
    return result;
}

/** NTSC rates keep their 1001 denominators; anything else becomes an exact thousandth. */
void frameRateFraction(double fps, int &numerator, int &denominator)
{
    const std::pair<double, std::pair<int, int>> ntsc[]{
        {23.976, {24000, 1001}}, {29.97, {30000, 1001}}, {47.952, {48000, 1001}}, {59.94, {60000, 1001}}, {119.88, {120000, 1001}}};
    for (const auto &rate : ntsc)
        if (std::abs(fps - rate.first) < 0.005) {
            numerator = rate.second.first;
            denominator = rate.second.second;
            return;
        }
    numerator = qRound(fps * 1000);
    denominator = 1000;
    const int divisor = std::gcd(numerator, denominator);
    numerator /= divisor;
    denominator /= divisor;
}

QJsonObject profileJson(const QString &path, const std::unique_ptr<ProfileModel> &profile)
{
    return {{"path", path},
            {"description", profile->description()},
            {"width", profile->width()},
            {"height", profile->height()},
            {"fps", std::round(profile->fps() * 1000) / 1000},
            {"fpsNumerator", profile->frame_rate_num()},
            {"fpsDenominator", profile->frame_rate_den()},
            {"progressive", profile->progressive()},
            {"displayAspect", QStringLiteral("%1:%2").arg(profile->display_aspect_num()).arg(profile->display_aspect_den())}};
}

/** A profile by repository path, or by its description (case-insensitive). Empty when none matches. */
QString resolveProfile(const QString &wanted)
{
    if (ProfileRepository::get()->profileExists(wanted)) return wanted;
    for (const auto &entry : ProfileRepository::get()->getAllProfiles())
        if (entry.first.compare(wanted.trimmed(), Qt::CaseInsensitive) == 0) return entry.second;
    return {};
}
} // namespace

int LiveBridge::trackIdForTag(const QString &tag) const
{
    if (!m_timeline) return -1;
    const QString wanted = tag.trimmed().toUpper();
    for (int row = 0; row < m_timeline->rowCount(); ++row) {
        const int id = int(m_timeline->index(row, 0).internalId());
        if (m_timeline->getTrackTagById(id).toUpper() == wanted) return id;
    }
    return -1;
}

QJsonObject LiveBridge::resolveTrackReferences(QJsonObject &command) const
{
    const auto unknown = [](const QString &tag) {
        return error(QStringLiteral("UNKNOWN_TRACK"), QStringLiteral("No track has the tag %1; desktop_state lists tracks[].tag.").arg(tag));
    };
    for (const auto &key : {QStringLiteral("trackId"), QStringLiteral("audioTrackId")}) {
        if (!command.value(key).isString()) continue;
        const int id = trackIdForTag(command.value(key).toString());
        if (id < 0) return unknown(command.value(key).toString());
        command.insert(key, id);
    }
    if (command.value(QStringLiteral("trackIds")).isArray()) {
        QJsonArray resolved;
        bool changed = false;
        for (const auto &item : command.value(QStringLiteral("trackIds")).toArray()) {
            if (!item.isString()) {
                resolved.append(item);
                continue;
            }
            const int id = trackIdForTag(item.toString());
            if (id < 0) return unknown(item.toString());
            resolved.append(id);
            changed = true;
        }
        if (changed) command.insert(QStringLiteral("trackIds"), resolved);
    }
    return {};
}

bool LiveBridge::waitForBinClips(const QStringList &binIds, int timeoutMs)
{
    const auto ready = [&binIds] {
        for (const auto &id : binIds) {
            auto clip = pCore->projectItemModel()->getClipByBinID(id);
            if (clip && !clip->statusReady()) return false;
        }
        return true;
    };
    if (ready()) return true;
    // Loading completes on the GUI thread, so wait by running the event loop; edits arriving meanwhile are refused as EDITOR_BUSY.
    QScopedValueRollback<bool> executing(m_executing, true);
    QScopedValueRollback<bool> waiting(m_waiting, true);
    QEventLoop loop;
    QTimer timer;
    QElapsedTimer elapsed;
    elapsed.start();
    timer.setInterval(50);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        if (ready() || elapsed.elapsed() >= timeoutMs || m_timeline.isNull()) loop.quit();
    });
    timer.start();
    loop.exec();
    return ready();
}

QJsonObject LiveBridge::mediaWait(const QJsonObject &arguments)
{
    if (!bind()) return error(QStringLiteral("NOT_READY"), QStringLiteral("No fully loaded active timeline."));
    if (m_executing || m_waiting) return error(QStringLiteral("EDITOR_BUSY"), QStringLiteral("An edit or another wait is in progress."));
    double timeout = 30;
    if (!keys(arguments, {QStringLiteral("binIds"), QStringLiteral("timeoutSeconds")}) ||
        !optionalNumber(arguments, QStringLiteral("timeoutSeconds"), 0, 120, timeout) ||
        (arguments.contains(QStringLiteral("binIds")) && !arguments.value(QStringLiteral("binIds")).isArray()))
        return error(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("Expected optional binIds (strings) and timeoutSeconds (0 to 120, default 30)."));
    QStringList ids;
    if (arguments.contains(QStringLiteral("binIds"))) {
        for (const auto &value : arguments.value(QStringLiteral("binIds")).toArray()) {
            if (!value.isString() || !QRegularExpression(QStringLiteral("^[0-9]+$")).match(value.toString()).hasMatch())
                return error(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("binIds must be numeric strings."));
            if (!pCore->projectItemModel()->getClipByBinID(value.toString()))
                return error(QStringLiteral("UNKNOWN_BIN"), QStringLiteral("Bin clip %1 does not exist.").arg(value.toString()));
            ids.append(value.toString());
        }
    } else {
        for (const auto &id : pCore->projectItemModel()->getAllClipIds())
            ids.append(id);
    }
    QElapsedTimer waited;
    waited.start();
    const bool ready = waitForBinClips(ids, qRound(timeout * 1000));
    QJsonArray items;
    for (const auto &id : ids) {
        auto clip = pCore->projectItemModel()->getClipByBinID(id);
        if (!clip) continue;
        QJsonObject item{{"binId", id}, {"name", clip->clipName()}, {"ready", clip->statusReady()}};
        if (clip->statusReady()) item.insert(QStringLiteral("duration"), qint64(clip->frameDuration()));
        items.append(item);
    }
    return {{"ok", true}, {"ready", ready}, {"timedOut", !ready}, {"waitedMs", waited.elapsed()}, {"items", items}};
}

QJsonObject LiveBridge::profileList(const QJsonObject &arguments)
{
    if (!keys(arguments, {QStringLiteral("query")}) || (arguments.contains(QStringLiteral("query")) && !arguments.value(QStringLiteral("query")).isString()))
        return error(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("Expected an optional query string."));
    const QString query = arguments.value(QStringLiteral("query")).toString().trimmed();
    const QString current = pCore->getCurrentProfilePath();
    QJsonArray profiles;
    for (const auto &entry : ProfileRepository::get()->getAllProfiles()) {
        auto &profile = ProfileRepository::get()->getProfile(entry.second);
        if (!profile || !profile->is_valid()) continue;
        if (!query.isEmpty() && !entry.first.contains(query, Qt::CaseInsensitive) && !entry.second.contains(query, Qt::CaseInsensitive)) continue;
        auto item = profileJson(entry.second, profile);
        item.insert(QStringLiteral("current"), entry.second == current);
        profiles.append(item);
    }
    return {{"ok", true}, {"current", current}, {"count", profiles.size()}, {"profiles", profiles}};
}

QJsonObject LiveBridge::executeComposition(const QJsonObject &command, bool &handled)
{
    handled = true;
    const QString type = command.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("project_new")) return newProject(command);
    using Handler = QJsonObject (LiveBridge::*)(const QJsonObject &);
    static const QHash<QString, Handler> handlers{{QStringLiteral("replace_clip"), &LiveBridge::replaceClip},
                                                  {QStringLiteral("clip_fade"), &LiveBridge::clipFade},
                                                  {QStringLiteral("audio_duck"), &LiveBridge::audioDuck}};
    const auto handler = handlers.value(type);
    if (!handler) {
        handled = false;
        return {};
    }
    // A GUI selection is a group of its own and must not widen the edit.
    m_timeline->requestClearSelection();
    return (this->*handler)(command);
}

QJsonObject LiveBridge::replaceClip(const QJsonObject &command)
{
    bool linked = true;
    bool keepEffects = true;
    if (!keys(command, {QStringLiteral("type"), QStringLiteral("clipId"), QStringLiteral("binId"), QStringLiteral("sourceIn"), QStringLiteral("linked"),
                        QStringLiteral("keepEffects")}) ||
        !integer(command, QStringLiteral("clipId")) || !command.value(QStringLiteral("binId")).isString() ||
        (command.contains(QStringLiteral("sourceIn")) && !integer(command, QStringLiteral("sourceIn"))) ||
        !optionalBool(command, QStringLiteral("linked"), linked) || !optionalBool(command, QStringLiteral("keepEffects"), keepEffects))
        return invalid(QStringLiteral("replace_clip takes clipId, binId and optional sourceIn, linked and keepEffects."));
    const int id = command.value(QStringLiteral("clipId")).toInt();
    if (auto rejected = editableClip(id); !rejected.isEmpty()) return rejected;
    const QString binId = command.value(QStringLiteral("binId")).toString();
    auto replacement = pCore->projectItemModel()->getClipByBinID(binId);
    if (!replacement) return error(QStringLiteral("UNKNOWN_BIN"), QStringLiteral("binId is not in the bin."));
    if (!replacement->statusReady())
        return error(QStringLiteral("MEDIA_NOT_READY"), QStringLiteral("The replacement is still loading; wait with desktop_media_wait."));
    if (replacement->clipType() == ClipType::Timeline)
        return error(QStringLiteral("SEQUENCE_PROTECTED"), QStringLiteral("Sequences cannot be used as replacements here."));
    // Targets: the clip, and its linked partner when the replacement has that stream too.
    struct Target
    {
        int id;
        int trackId;
        bool audio;
        int position;
        int duration;
        int in;
        int groupId;
        QDomElement effects;
        int effectCount{0};
    };
    QDomDocument document;
    QList<Target> targets;
    const auto describe = [&](int clipId, int in) {
        const int trackId = m_timeline->getClipTrackId(clipId);
        Target target{clipId,
                      trackId,
                      m_timeline->isAudioTrack(trackId),
                      m_timeline->getClipPosition(clipId),
                      m_timeline->getClipPlaytime(clipId),
                      in,
                      m_timeline->getItemGroupId(clipId),
                      {}};
        target.effects = userEffectsXml(document, m_timeline->getClipEffectStackModel(clipId), m_timeline->getClipIn(clipId), target.effectCount);
        return target;
    };
    const int mainIn = command.value(QStringLiteral("sourceIn")).toInt(m_timeline->getClipIn(id));
    targets.append(describe(id, mainIn));
    const bool mainAudio = targets.first().audio;
    if ((mainAudio && !replacement->hasAudio()) || (!mainAudio && !replacement->hasVideo()))
        return error(QStringLiteral("INCOMPATIBLE_MEDIA"),
                     QStringLiteral("The replacement has no %1 stream.").arg(mainAudio ? QStringLiteral("audio") : QStringLiteral("video")));
    const int partner = m_timeline->getClipSplitPartner(id);
    bool partnerReplaced = false;
    if (linked && partner >= 0) {
        const bool partnerAudio = m_timeline->isAudioTrack(m_timeline->getClipTrackId(partner));
        if (m_timeline->trackIsLocked(m_timeline->getClipTrackId(partner)))
            return error(QStringLiteral("TRACK_LOCKED"), QStringLiteral("The linked clip is on a locked track; pass linked: false."));
        if ((partnerAudio && replacement->hasAudio()) || (!partnerAudio && replacement->hasVideo())) {
            // The partner keeps its own source offset relative to the main clip, so a pair stays in sync.
            targets.append(describe(partner, mainIn + (m_timeline->getClipIn(partner) - m_timeline->getClipIn(id))));
            partnerReplaced = true;
        }
    }
    if (replacement->hasLimitedDuration()) {
        for (const auto &target : std::as_const(targets)) {
            if (target.in < 0 || qint64(target.in) + target.duration > qint64(replacement->frameDuration())) {
                auto result = error(QStringLiteral("MEDIA_TOO_SHORT"), QStringLiteral("Clip %1 needs source frames [%2, %3) but the replacement has %4 frames.")
                                                                           .arg(target.id)
                                                                           .arg(target.in)
                                                                           .arg(target.in + target.duration)
                                                                           .arg(replacement->frameDuration()));
                result.insert(QStringLiteral("details"), QJsonObject{{"clipId", target.id},
                                                                     {"sourceIn", target.in},
                                                                     {"sourceOut", target.in + target.duration},
                                                                     {"replacementDuration", qint64(replacement->frameDuration())}});
                return result;
            }
        }
    }
    // Former group members that stay, regrouped with the new clips afterwards.
    QSet<int> leaves;
    QSet<int> rootGroups;
    for (const auto &target : std::as_const(targets)) {
        if (target.groupId < 0) continue;
        for (int member : m_timeline->getGroupElements(target.id))
            leaves.insert(member);
    }
    QList<int> targetIds;
    for (const auto &target : std::as_const(targets))
        targetIds.append(target.id);
    for (int member : std::as_const(targetIds))
        leaves.remove(member);
    Fun undo = [] { return true; };
    Fun redo = [] { return true; };
    bool ok = true;
    QSet<int> ungrouped;
    for (const auto &target : std::as_const(targets)) {
        if (target.groupId < 0 || ungrouped.contains(target.groupId)) continue;
        ungrouped.insert(target.groupId);
        ok = ok && runStep(undo, redo, [&](Fun &u, Fun &r) { return m_timeline->requestClipUngroup(target.id, u, r); });
    }
    for (const auto &target : std::as_const(targets))
        ok = ok && runStep(undo, redo, [&](Fun &u, Fun &r) { return m_timeline->requestItemDeletion(target.id, u, r, true); });
    QList<int> newIds;
    QJsonArray replaced;
    bool effectsRestored = true;
    for (auto &target : targets) {
        int newId = -1;
        const QString producer = QStringLiteral("%1%2/%3/%4")
                                     .arg(target.audio ? QStringLiteral("A") : QStringLiteral("V"), binId)
                                     .arg(target.in)
                                     .arg(target.in + target.duration - 1);
        ok = ok && runStep(undo, redo, [&](Fun &u, Fun &r) {
                 return m_timeline->requestClipInsertion(producer, target.trackId, target.position, newId, true, true, false, u, r, {}, 0) && newId >= 0;
             });
        if (!ok) break;
        if (keepEffects && target.effectCount > 0) {
            // fromXml accumulates its own steps; a refused effect (unique on the new clip) is reported, not fatal.
            if (!m_timeline->getClipEffectStackModel(newId)->fromXml(target.effects, undo, redo)) effectsRestored = false;
        }
        newIds.append(newId);
        replaced.append(QJsonObject{{"clipId", target.id},
                                    {"newClipId", newId},
                                    {"trackId", target.trackId},
                                    {"position", target.position},
                                    {"duration", target.duration},
                                    {"sourceIn", target.in},
                                    {"effects", keepEffects ? target.effectCount : 0}});
    }
    int groupId = -1;
    if (ok) {
        std::unordered_set<int> members(leaves.cbegin(), leaves.cend());
        for (int newId : std::as_const(newIds))
            members.insert(newId);
        if (members.size() >= 2) {
            // Exactly the replaced pair, both halves from the replacement: a linked A/V pair again.
            const GroupType kind = members.size() == 2 && newIds.size() == 2 ? GroupType::AVSplit : GroupType::Normal;
            ok = runStep(undo, redo, [&](Fun &u, Fun &r) {
                groupId = m_timeline->requestClipsGroup(members, u, r, kind);
                return groupId >= 0;
            });
        }
    }
    if (!ok) {
        undo();
        return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native replacement was rejected; the timeline is unchanged."));
    }
    pCore->pushUndo(undo, redo, QStringLiteral("Replace clip"));
    QJsonObject result{{"ok", true},
                       {"clipId", newIds.first()},
                       {"binId", binId},
                       {"replaced", replaced},
                       {"groupId", groupId >= 0 ? QJsonValue(groupId) : QJsonValue(QJsonValue::Null)},
                       {"effectsRestored", effectsRestored},
                       {"historySummary", QJsonObject{{"newClipIds", idArray(newIds)}, {"replacedClipIds", idArray(targetIds)}}}};
    if (partnerReplaced)
        result.insert(QStringLiteral("linkedClipId"), newIds.last());
    else if (partner >= 0 && m_timeline->isClip(partner))
        result.insert(QStringLiteral("linkedClipId"), partner);
    return result;
}

QJsonObject LiveBridge::clipFade(const QJsonObject &command)
{
    bool linked = true;
    if (!keys(command, {QStringLiteral("type"), QStringLiteral("clipId"), QStringLiteral("fadeIn"), QStringLiteral("fadeOut"), QStringLiteral("linked")}) ||
        !integer(command, QStringLiteral("clipId")) || (command.contains(QStringLiteral("fadeIn")) && !integer(command, QStringLiteral("fadeIn"))) ||
        (command.contains(QStringLiteral("fadeOut")) && !integer(command, QStringLiteral("fadeOut"))) ||
        !optionalBool(command, QStringLiteral("linked"), linked) ||
        (!command.contains(QStringLiteral("fadeIn")) && !command.contains(QStringLiteral("fadeOut"))))
        return invalid(QStringLiteral("clip_fade takes clipId and fadeIn and/or fadeOut in frames (0 removes a fade), optional linked."));
    const int id = command.value(QStringLiteral("clipId")).toInt();
    if (auto rejected = editableClip(id); !rejected.isEmpty()) return rejected;
    QList<int> targets{id};
    const int partner = m_timeline->getClipSplitPartner(id);
    if (linked && partner >= 0) {
        if (m_timeline->trackIsLocked(m_timeline->getClipTrackId(partner)))
            return error(QStringLiteral("TRACK_LOCKED"), QStringLiteral("The linked clip is on a locked track; pass linked: false."));
        targets.append(partner);
    }
    const int fadeIn = command.value(QStringLiteral("fadeIn")).toInt(-1);
    const int fadeOut = command.value(QStringLiteral("fadeOut")).toInt(-1);
    for (int target : std::as_const(targets)) {
        const int duration = m_timeline->getClipPlaytime(target);
        if (qMax(0, fadeIn) + qMax(0, fadeOut) >= duration)
            return invalid(QStringLiteral("Clip %1 is %2 frames long; the fades must leave at least one frame.").arg(target).arg(duration));
    }
    // Kdenlive's fade effects: fadein/fadeout on audio, fade_from_black/fade_to_black on video. Adding one is an Undo step; one macro covers the call.
    auto stack = m_document->commandStack();
    stack->beginMacro(QStringLiteral("Fade clip"));
    QJsonArray applied;
    for (int target : std::as_const(targets)) {
        const bool audio = m_timeline->isAudioTrack(m_timeline->getClipTrackId(target));
        auto effects = m_timeline->getClipEffectStackModel(target);
        QJsonObject entry{{"clipId", target}, {"type", audio ? QStringLiteral("audio") : QStringLiteral("video")}};
        if (fadeIn >= 0) {
            if (fadeIn == 0)
                effects->removeFade(true);
            else
                effects->adjustFadeLength(fadeIn, true, audio, !audio, true);
            entry.insert(QStringLiteral("fadeIn"), fadeIn);
        }
        if (fadeOut >= 0) {
            if (fadeOut == 0)
                effects->removeFade(false);
            else
                effects->adjustFadeLength(fadeOut, false, audio, !audio, true);
            entry.insert(QStringLiteral("fadeOut"), fadeOut);
        }
        applied.append(entry);
    }
    stack->endMacro();
    pCore->refreshProjectMonitorOnce();
    return {{"ok", true}, {"clipIds", idArray(targets)}, {"fades", applied}, {"historySummary", QJsonObject{{"clipIds", idArray(targets)}}}};
}

QJsonObject LiveBridge::audioDuck(const QJsonObject &command)
{
    bool automatic = false;
    bool mute = false;
    double level = 0;
    double duck = -18;
    const double fps = pCore->getCurrentFps();
    double ramp = std::round(fps / 2);
    if (!keys(command, {QStringLiteral("type"), QStringLiteral("clipId"), QStringLiteral("ranges"), QStringLiteral("auto"), QStringLiteral("trackIds"),
                        QStringLiteral("levelDb"), QStringLiteral("duckDb"), QStringLiteral("rampFrames"), QStringLiteral("mute")}) ||
        !integer(command, QStringLiteral("clipId")) || !optionalBool(command, QStringLiteral("auto"), automatic) ||
        !optionalBool(command, QStringLiteral("mute"), mute) || !optionalNumber(command, QStringLiteral("levelDb"), -60, 24, level) ||
        !optionalNumber(command, QStringLiteral("duckDb"), -100, 24, duck) || !optionalNumber(command, QStringLiteral("rampFrames"), 0, 100000, ramp) ||
        std::floor(ramp) != ramp || automatic == command.contains(QStringLiteral("ranges")) ||
        (command.contains(QStringLiteral("trackIds")) && (!automatic || !command.value(QStringLiteral("trackIds")).isArray())))
        return invalid(QStringLiteral("audio_duck takes clipId and either ranges [{start, end}] or auto: true (optional trackIds), plus optional levelDb, "
                                      "duckDb, rampFrames and mute."));
    const int id = command.value(QStringLiteral("clipId")).toInt();
    if (auto rejected = editableClip(id); !rejected.isEmpty()) return rejected;
    const int ownTrack = m_timeline->getClipTrackId(id);
    if (!m_timeline->isAudioTrack(ownTrack)) return error(QStringLiteral("INCOMPATIBLE_MEDIA"), QStringLiteral("Ducking applies to a clip on an audio track."));
    if (mute) duck = -100;
    const int position = m_timeline->getClipPosition(id);
    const int duration = m_timeline->getClipPlaytime(id);
    const int end = position + duration;
    const double speed = std::abs(m_timeline->getClipSpeed(id));
    const int in = m_timeline->getClipInOut(id).first;
    QList<QPair<int, int>> spans;
    if (automatic) {
        // Every clip on the other audio tracks (or the listed ones) counts as speech or sound to duck under.
        QList<int> tracks;
        if (command.contains(QStringLiteral("trackIds"))) {
            for (const auto &value : command.value(QStringLiteral("trackIds")).toArray()) {
                if (!value.isDouble() || !m_timeline->isTrack(value.toInt()))
                    return error(QStringLiteral("UNKNOWN_TRACK"), QStringLiteral("trackIds names an unknown track."));
                if (!m_timeline->isAudioTrack(value.toInt()))
                    return error(QStringLiteral("INCOMPATIBLE_MEDIA"), QStringLiteral("trackIds must be audio tracks."));
                tracks.append(value.toInt());
            }
        } else {
            for (int row = 0; row < m_timeline->rowCount(); ++row) {
                const int track = int(m_timeline->index(row, 0).internalId());
                if (track != ownTrack && m_timeline->isAudioTrack(track)) tracks.append(track);
            }
        }
        for (int track : std::as_const(tracks))
            for (const auto &span : trackClips(track))
                if (span.id != id) spans.append({span.start, span.end});
    } else {
        const auto list = command.value(QStringLiteral("ranges"));
        if (!list.isArray() || list.toArray().isEmpty() || list.toArray().size() > 1000) return invalid(QStringLiteral("ranges must list 1 to 1000 spans."));
        for (const auto &value : list.toArray()) {
            const auto range = value.toObject();
            if (!value.isObject() || !keys(range, {QStringLiteral("start"), QStringLiteral("end")}) || !integer(range, QStringLiteral("start")) ||
                !integer(range, QStringLiteral("end"), 1) || range.value(QStringLiteral("start")).toInt() >= range.value(QStringLiteral("end")).toInt())
                return invalid(QStringLiteral("Each range needs whole frames start < end (end exclusive)."));
            spans.append({range.value(QStringLiteral("start")).toInt(), range.value(QStringLiteral("end")).toInt()});
        }
    }
    // Clip to the music clip, sort and merge spans whose gap could not hold both ramps.
    QList<QPair<int, int>> merged;
    std::sort(spans.begin(), spans.end());
    const int rampFrames = int(ramp);
    for (auto span : std::as_const(spans)) {
        span.first = qMax(span.first, position);
        span.second = qMin(span.second, end);
        if (span.second <= span.first) continue;
        if (!merged.isEmpty() && span.first - merged.last().second < 2 * rampFrames)
            merged.last().second = qMax(merged.last().second, span.second);
        else
            merged.append(span);
    }
    if (merged.isEmpty()) return error(QStringLiteral("NO_RANGES"), QStringLiteral("No range overlaps the clip, so there is nothing to duck under."));
    // Keyframes are source frames of the clip, as desktop_effect_list's keyframeOrigin reports them.
    const auto source = [&](int frame) { return in + qRound((frame - position) * speed); };
    std::map<int, double> levels;
    levels[source(position)] = level;
    for (const auto &span : std::as_const(merged)) {
        const int rampInStart = span.first - rampFrames;
        if (rampInStart > position)
            levels[source(rampInStart)] = level;
        else
            levels[source(position)] = duck;
        levels[source(span.first)] = duck;
        levels[source(span.second - 1)] = duck;
        const int rampOutEnd = span.second - 1 + rampFrames;
        if (rampOutEnd < end) levels[source(rampOutEnd)] = level;
    }
    if (merged.last().second < end) levels[source(end - 1)] = level;
    QStringList keyframes;
    for (const auto &[frame, value] : levels)
        keyframes << QStringLiteral("%1=%2").arg(frame).arg(QString::number(value, 'g', 15));
    const QString animation = keyframes.join(QLatin1Char(';'));
    auto stack = m_timeline->getClipEffectStackModel(id);
    int effectIndex = -1;
    bool replaced = false;
    for (int row = 0; row < stack->rowCount() && effectIndex < 0; ++row) {
        auto effect = std::static_pointer_cast<EffectItemModel>(stack->getEffectStackRow(row));
        if (effect && !effect->isBuiltIn() && effect->getAssetId() == QLatin1String("volume")) {
            pCore->pushUndo(new AssetUpdateCommand(effect, {{QStringLiteral("level"), animation}}));
            effectIndex = row;
            replaced = true;
        }
    }
    if (effectIndex < 0) {
        if (!stack->appendEffect(QStringLiteral("volume"), false, {{QStringLiteral("level"), animation}}))
            return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native volume effect was rejected."));
        effectIndex = stack->rowCount() - 1;
    }
    QJsonArray ranges;
    for (const auto &span : std::as_const(merged))
        ranges.append(QJsonObject{{"start", span.first}, {"end", span.second}});
    return {{"ok", true},
            {"clipId", id},
            {"effectIndex", effectIndex},
            {"replacedVolume", replaced},
            {"ranges", ranges},
            {"keyframes", int(levels.size())},
            {"levelDb", level},
            {"duckDb", duck},
            {"rampFrames", rampFrames},
            {"level", animation},
            {"historySummary", QJsonObject{{"ranges", ranges.size()}, {"duckDb", duck}}}};
}

QJsonObject LiveBridge::newProject(const QJsonObject &command)
{
    if (!keys(command,
              {QStringLiteral("type"), QStringLiteral("profile"), QStringLiteral("path"), QStringLiteral("videoTracks"), QStringLiteral("audioTracks")}) ||
        !command.value(QStringLiteral("profile")).isString() || !command.value(QStringLiteral("path")).isString() ||
        (command.contains(QStringLiteral("videoTracks")) && !integer(command, QStringLiteral("videoTracks"), 1)) ||
        (command.contains(QStringLiteral("audioTracks")) && !integer(command, QStringLiteral("audioTracks"), 1)) ||
        command.value(QStringLiteral("videoTracks")).toInt(1) > 32 || command.value(QStringLiteral("audioTracks")).toInt(1) > 32)
        return invalid(QStringLiteral("project_new takes profile (path or description from desktop_profile_list), a new .kdenlive path and optional "
                                      "videoTracks/audioTracks (1 to 32)."));
    const QFileInfo file(command.value(QStringLiteral("path")).toString());
    if (!file.isAbsolute() || file.suffix() != QLatin1String("kdenlive") || file.exists() || !QFileInfo(file.absolutePath()).isDir() ||
        !QFileInfo(file.absolutePath()).isWritable())
        return error(QStringLiteral("INVALID_PATH"), QStringLiteral("project_new needs a new absolute .kdenlive path in a writable folder."));
    const QString profilePath = resolveProfile(command.value(QStringLiteral("profile")).toString());
    if (profilePath.isEmpty())
        return error(QStringLiteral("INVALID_PROFILE"), QStringLiteral("Unknown profile; use a path or description from desktop_profile_list."));
    if (m_document && m_document->isModified())
        return error(QStringLiteral("PROJECT_MODIFIED"),
                     QStringLiteral("The open project has unsaved changes; save it with desktop_project_save (or undo them) before creating a new one."));
    if (pCore->window()->hasRunningTask()) return error(QStringLiteral("EDITOR_BUSY"), QStringLiteral("A background task is running; try again later."));
    // File > New offers to recover a stale autosave of an untitled project in a dialog, which would block the call. Report it instead.
    QJsonArray backups;
    const QUrl untitled = QUrl::fromLocalFile(KdenliveSettings::defaultprojectfolder() + QStringLiteral("/_untitled.kdenlive"));
    for (KAutoSaveFile *stale : KAutoSaveFile::staleFiles(untitled)) {
        if (stale->open(QIODevice::ReadWrite)) backups.append(stale->fileName());
        delete stale;
    }
    if (!backups.isEmpty()) {
        auto rejected = error(QStringLiteral("BACKUP_PENDING"),
                              QStringLiteral("Kdenlive holds an autosave backup of an untitled project; recover or discard it with File > New in the "
                                             "editor (or delete the listed files) and retry."));
        rejected.insert(QStringLiteral("backups"), backups);
        return rejected;
    }
    const QString previousId = m_document ? m_document->getDocumentProperty(QStringLiteral("documentid")) : QString();
    const int previousVideo = KdenliveSettings::videotracks();
    const int previousAudio = KdenliveSettings::audiotracks();
    if (command.contains(QStringLiteral("videoTracks"))) KdenliveSettings::setVideotracks(command.value(QStringLiteral("videoTracks")).toInt());
    if (command.contains(QStringLiteral("audioTracks"))) KdenliveSettings::setAudiotracks(command.value(QStringLiteral("audioTracks")).toInt());
    // Kdenlive's own File > New without its dialog: closes the current document and builds the new one synchronously.
    pCore->projectManager()->newFile(profilePath, false);
    KdenliveSettings::setVideotracks(previousVideo);
    KdenliveSettings::setAudiotracks(previousAudio);
    KdenliveDoc *created = pCore->currentDoc();
    if (!created || (!previousId.isEmpty() && created->getDocumentProperty(QStringLiteral("documentid")) == previousId))
        return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Kdenlive did not replace the current project."));
    QJsonObject result{{"ok", true}, {"sessionEnded", true}, {"profile", profileJson(profilePath, ProfileRepository::get()->getProfile(profilePath))}};
    if (!pCore->projectManager()->saveFileAs(file.absoluteFilePath(), true, false)) {
        result.insert(QStringLiteral("saved"), false);
        result.insert(QStringLiteral("warning"), QStringLiteral("The project was created but could not be saved to the requested path."));
        return result;
    }
    result.insert(QStringLiteral("saved"), true);
    result.insert(QStringLiteral("documentUrl"), created->url().toString());
    return result;
}

QJsonObject LiveBridge::changeFrameRate(const QJsonObject &command, int width, int height, double fps)
{
    Q_UNUSED(command)
    const QFileInfo file(m_document->url().toLocalFile());
    if (!m_document->url().isLocalFile() || !file.isFile())
        return error(QStringLiteral("SAVE_REQUIRED"), QStringLiteral("Changing the frame rate converts the saved project file; save it first."));
    if (m_document->isModified())
        return error(QStringLiteral("PROJECT_MODIFIED"), QStringLiteral("Save the project (desktop_project_save) before changing its frame rate."));
    int numerator = 0;
    int denominator = 1;
    frameRateFraction(fps, numerator, denominator);
    auto &current = pCore->getProjectProfile();
    const int divisor = std::gcd(width, height);
    ProfileParam wanted(width, height, numerator, denominator, width / divisor, height / divisor, 1, 1, current.colorspace(), !current.progressive());
    QString path = ProfileRepository::get()->findMatchingProfile(&wanted);
    if (path.isEmpty()) {
        wanted.m_description = QStringLiteral("%1x%2 %3 fps").arg(width).arg(height).arg(QString::number(double(numerator) / denominator, 'f', 2));
        path = ProfileRepository::get()->saveProfile(&wanted);
    }
    if (path.isEmpty()) return error(QStringLiteral("INVALID_PROFILE"), QStringLiteral("Cannot create a matching project profile."));
    auto &profile = ProfileRepository::get()->getProfile(path);
    // Kdenlive writes the converted project next to the original with the frame rate appended and opens it.
    const QString converted = QStringUtils::appendToFilename(file.absoluteFilePath(), QStringLiteral("-%1").arg(int(profile->fps() * 100)));
    if (QFile::exists(converted))
        return error(QStringLiteral("FILE_EXISTS"),
                     QStringLiteral("Kdenlive would write the converted project to %1, which already exists; move it first.").arg(converted));
    pCore->projectManager()->saveWithUpdatedProfile(path);
    return {{"ok", true},
            {"changed", true},
            {"sessionEnded", true},
            {"profile", profileJson(path, profile)},
            {"convertedFile", converted},
            {"note", QStringLiteral("Kdenlive converted the saved project into convertedFile and is opening it; read desktop_state until documentUrl "
                                    "names that file, then continue with the new sessionId.")}};
}
