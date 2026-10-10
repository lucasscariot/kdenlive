/*
    SPDX-FileCopyrightText: 2026 Lucas Scariot <lucas@scariot.fr>
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "magnetictimeline.hpp"
#include "clipmodel.hpp"
#include "compositionmodel.hpp"
#include "core.h"
#include "groupsmodel.hpp"
#include "macros.hpp"
#include "timelinefunctions.hpp"
#include "timelineitemmodel.hpp"
#include "trackmodel.hpp"

#include <KLocalizedString>
#include <algorithm>

namespace {
const QString primaryProperty = QStringLiteral("kdenlive:magnetic_primary");
/** @brief Anchor id standing for the end of the storyline */
constexpr int endAnchor = -1;

} // namespace

/** @brief The magnetic edits are built from normal mode operations: they make room themselves, so the overlap checks of
    normal mode must apply while they run */
class MagneticTimeline::NormalEditScope
{
public:
    explicit NormalEditScope(const std::shared_ptr<TimelineItemModel> &timeline)
        : m_timeline(timeline)
        , m_mode(timeline->m_editMode)
    {
        // Silently, the interface keeps showing the magnetic mode
        m_timeline->m_editMode = TimelineMode::NormalEdit;
    }
    ~NormalEditScope() { m_timeline->m_editMode = m_mode; }

private:
    std::shared_ptr<TimelineItemModel> m_timeline;
    TimelineMode::EditMode m_mode;
};

bool MagneticTimeline::isEnabled(const std::shared_ptr<TimelineItemModel> &timeline)
{
    return timeline && timeline->editMode() == TimelineMode::MagneticEdit;
}

int MagneticTimeline::primaryTrack(const std::shared_ptr<TimelineItemModel> &timeline)
{
    return primaryTrack(timeline.get());
}

int MagneticTimeline::primaryTrack(const TimelineModel *timeline)
{
    int lowest = -1;
    for (const auto &track : timeline->m_allTracks) {
        if (track->isAudioTrack()) {
            continue;
        }
        if (track->getProperty(primaryProperty).toInt() == 1) {
            return track->getId();
        }
        if (lowest == -1) {
            lowest = track->getId();
        }
    }
    return lowest;
}

bool MagneticTimeline::setPrimaryTrack(const std::shared_ptr<TimelineItemModel> &timeline, int trackId, Fun &undo, Fun &redo)
{
    if (!timeline->isTrack(trackId) || timeline->isAudioTrack(trackId)) {
        return false;
    }
    const int previous = primaryTrack(timeline);
    if (previous == trackId) {
        return true;
    }
    std::weak_ptr<TimelineItemModel> weak = timeline;
    auto mark = [weak](int tid) {
        return [weak, tid]() {
            if (auto ptr = weak.lock()) {
                for (const auto &track : ptr->m_allTracks) {
                    if (!track->isAudioTrack()) {
                        track->setProperty(primaryProperty, track->getId() == tid ? QStringLiteral("1") : QString());
                        const QModelIndex ix = ptr->makeTrackIndexFromID(track->getId());
                        Q_EMIT ptr->dataChanged(ix, ix, {TimelineModel::StorylineRole});
                    }
                }
                return true;
            }
            return false;
        };
    };
    Fun local_redo = mark(trackId);
    Fun local_undo = mark(previous);
    local_redo();
    UPDATE_UNDO_REDO_NOLOCK(local_redo, local_undo, undo, redo);
    return true;
}

std::vector<int> MagneticTimeline::storylineClips(const std::shared_ptr<TimelineItemModel> &timeline)
{
    std::vector<int> clips;
    const int tid = primaryTrack(timeline);
    if (tid == -1) {
        return clips;
    }
    const auto inRange = timeline->getTrackById_const(tid)->getClipsInRange(0, -1);
    clips.assign(inRange.begin(), inRange.end());
    std::sort(clips.begin(), clips.end(), [&timeline](int a, int b) { return timeline->getClipPosition(a) < timeline->getClipPosition(b); });
    return clips;
}

int MagneticTimeline::storylineEnd(const std::shared_ptr<TimelineItemModel> &timeline)
{
    int end = 0;
    for (int cid : storylineClips(timeline)) {
        end = std::max(end, timeline->getClipPosition(cid) + timeline->getClipPlaytime(cid));
    }
    return end;
}

std::unordered_set<int> MagneticTimeline::unitOf(const std::shared_ptr<TimelineItemModel> &timeline, int itemId)
{
    if (timeline->m_groups->isInGroup(itemId)) {
        return timeline->m_groups->getLeaves(timeline->m_groups->getRootId(itemId));
    }
    return {itemId};
}

bool MagneticTimeline::isStorylineItem(const std::shared_ptr<TimelineItemModel> &timeline, int itemId)
{
    const int tid = primaryTrack(timeline);
    if (tid == -1 || !timeline->isItem(itemId)) {
        return false;
    }
    for (int id : unitOf(timeline, itemId)) {
        if (timeline->isClip(id) && timeline->getClipTrackId(id) == tid) {
            return true;
        }
    }
    return false;
}

int MagneticTimeline::parentOf(const std::shared_ptr<TimelineItemModel> &timeline, int itemId)
{
    if (!timeline->isItem(itemId) || isStorylineItem(timeline, itemId)) {
        return -1;
    }
    const int tid = primaryTrack(timeline);
    if (tid == -1) {
        return -1;
    }
    return timeline->getClipByPosition(tid, unitStart(timeline, unitOf(timeline, itemId)));
}

int MagneticTimeline::unitStart(const std::shared_ptr<TimelineItemModel> &timeline, const std::unordered_set<int> &leaves)
{
    int start = -1;
    for (int id : leaves) {
        if (!timeline->isClip(id) && !timeline->isComposition(id)) {
            continue;
        }
        const int pos = timeline->getItemPosition(id);
        if (start == -1 || pos < start) {
            start = pos;
        }
    }
    return start;
}

std::unordered_set<int> MagneticTimeline::storylineTracks(const std::shared_ptr<TimelineItemModel> &timeline)
{
    std::unordered_set<int> tracks;
    const int tid = primaryTrack(timeline);
    if (tid == -1) {
        return tracks;
    }
    tracks.insert(tid);
    const int mirror = timeline->getMirrorAudioTrackId(tid);
    if (mirror > -1) {
        tracks.insert(mirror);
    }
    for (int cid : storylineClips(timeline)) {
        for (int id : unitOf(timeline, cid)) {
            if (timeline->isClip(id) || timeline->isComposition(id)) {
                tracks.insert(timeline->getItemTrackId(id));
            }
        }
    }
    return tracks;
}

MagneticTimeline::Connections MagneticTimeline::connections(const std::shared_ptr<TimelineItemModel> &timeline, const std::unordered_set<int> &exclude)
{
    Connections result;
    const int tid = primaryTrack(timeline);
    if (tid == -1) {
        return result;
    }
    const std::vector<int> story = storylineClips(timeline);
    std::unordered_set<int> seenRoots;
    auto consider = [&](int itemId, int trackId) {
        if (trackId == -1 || exclude.count(itemId) > 0 || timeline->getTrackById_const(trackId)->isLocked()) {
            return;
        }
        const int root = timeline->m_groups->getRootId(itemId);
        if (!seenRoots.insert(root).second) {
            return;
        }
        Connection c;
        c.item = itemId;
        c.leaves = unitOf(timeline, itemId);
        for (int id : c.leaves) {
            if (exclude.count(id) > 0) {
                return;
            }
            if (timeline->isClip(id) && timeline->getClipTrackId(id) == tid) {
                // Part of the storyline
                return;
            }
        }
        c.start = unitStart(timeline, c.leaves);
        c.parent = timeline->getClipByPosition(tid, c.start);
        if (c.parent > -1) {
            c.anchors.emplace_back(c.parent, c.start - timeline->getClipPosition(c.parent));
        }
        // Over a gap, hang from the next storyline clip; past the end, or if that clip goes, keep the distance to the end
        auto next = std::find_if(story.begin(), story.end(), [&](int cid) { return cid != c.parent && timeline->getClipPosition(cid) >= c.start; });
        if (next != story.end()) {
            c.anchors.emplace_back(*next, c.start - timeline->getClipPosition(*next));
        }
        c.anchors.emplace_back(endAnchor, c.start - storylineEnd(timeline));
        result.push_back(c);
    };
    for (const auto &clip : timeline->m_allClips) {
        consider(clip.first, clip.second->getCurrentTrackId());
    }
    for (const auto &compo : timeline->m_allCompositions) {
        consider(compo.first, compo.second->getCurrentTrackId());
    }
    return result;
}

bool MagneticTimeline::moveUnitBy(const std::shared_ptr<TimelineItemModel> &timeline, int itemId, int deltaTrack, int deltaPos, Fun &undo, Fun &redo)
{
    if (deltaTrack == 0 && deltaPos == 0) {
        return true;
    }
    if (timeline->m_groups->isInGroup(itemId)) {
        return groupMove(timeline, itemId, deltaTrack, deltaPos, true, undo, redo);
    }
    const int trackId = timeline->getItemTrackId(itemId);
    int targetTrack = trackId;
    if (deltaTrack != 0) {
        const int pos = timeline->getTrackPosition(trackId) + deltaTrack;
        if (pos < 0 || pos >= timeline->getTracksCount()) {
            return false;
        }
        targetTrack = timeline->getTrackIndexFromPosition(pos);
    }
    const int position = timeline->getItemPosition(itemId) + deltaPos;
    if (position < 0) {
        return false;
    }
    if (timeline->isClip(itemId)) {
        if (!timeline->getTrackById_const(targetTrack)->isAvailableWithExceptions(position, timeline->getClipPlaytime(itemId), {itemId})) {
            return false;
        }
        return timeline->requestClipMove(itemId, targetTrack, position, true, true, true, true, undo, redo) == TimelineModel::MoveSuccess;
    }
    if (timeline->isComposition(itemId)) {
        return timeline->requestCompositionMove(itemId, targetTrack, timeline->m_allCompositions[itemId]->getForcedTrack(), position, true, true, undo, redo);
    }
    return false;
}

bool MagneticTimeline::groupMove(const std::shared_ptr<TimelineItemModel> &timeline, int itemId, int deltaTrack, int deltaPos, bool moveMirrorTracks, Fun &undo,
                                 Fun &redo)
{
    const int groupId = timeline->m_groups->getRootId(itemId);
    // A final group move trusts its target, so first run it as a drag preview, which refuses to overlap other clips
    Fun test_undo = []() { return true; };
    Fun test_redo = []() { return true; };
    const bool fits = timeline->requestGroupMove(itemId, groupId, deltaTrack, deltaPos, false, false, test_undo, test_redo, false, moveMirrorTracks, false);
    test_undo();
    if (!fits) {
        return false;
    }
    return timeline->requestGroupMove(itemId, groupId, deltaTrack, deltaPos, true, true, undo, redo, false, moveMirrorTracks);
}

bool MagneticTimeline::moveItemsBy(const std::shared_ptr<TimelineItemModel> &timeline, const std::unordered_set<int> &items, int delta, Fun &undo, Fun &redo)
{
    if (delta == 0 || items.empty()) {
        return true;
    }
    std::unordered_set<int> roots;
    for (int id : items) {
        roots.insert(timeline->m_groups->getRootId(id));
    }
    const int itemId = *items.begin();
    if (roots.size() == 1) {
        return moveUnitBy(timeline, itemId, 0, delta, undo, redo);
    }
    // Move everything at once through a temporary selection group, like the spacer does
    timeline->requestSetSelection(items);
    bool result = groupMove(timeline, itemId, 0, delta, false, undo, redo);
    timeline->requestClearSelection();
    return result;
}

int MagneticTimeline::parkItems(const std::shared_ptr<TimelineItemModel> &timeline, const std::unordered_set<int> &items, Fun &undo, Fun &redo)
{
    const int start = unitStart(timeline, items);
    if (start < 0) {
        return 0;
    }
    int end = timeline->duration();
    for (const auto &track : timeline->m_allTracks) {
        end = std::max(end, track->trackDuration());
    }
    const int delta = end + 1 - start;
    if (!moveItemsBy(timeline, items, delta, undo, redo)) {
        return -1;
    }
    return delta;
}

bool MagneticTimeline::parkResidents(const std::shared_ptr<TimelineItemModel> &timeline, const Connections &connections, const std::unordered_set<int> &tracks,
                                     Fun &undo, Fun &redo)
{
    std::unordered_set<int> residents;
    for (const Connection &c : connections) {
        for (int id : c.leaves) {
            if ((timeline->isClip(id) || timeline->isComposition(id)) && tracks.count(timeline->getItemTrackId(id)) > 0) {
                residents.insert(c.leaves.begin(), c.leaves.end());
                break;
            }
        }
    }
    return residents.empty() || parkItems(timeline, residents, undo, redo) >= 0;
}

bool MagneticTimeline::bumpToFreeLane(const std::shared_ptr<TimelineItemModel> &timeline, int clipId, int trackId, int position, Fun &undo, Fun &redo)
{
    if (timeline->getClipTrackId(clipId) == -1 || !timeline->isTrack(trackId)) {
        return false;
    }
    const bool audio = timeline->isAudioTrack(trackId);
    const int primary = primaryTrack(timeline);
    const int duration = timeline->getClipPlaytime(clipId);
    const int origin = timeline->getTrackPosition(trackId);
    // Lanes of the same kind, those of the same role first, then the closest, away from the storyline on a tie
    const QString role = timeline->getTrackRole(trackId);
    std::vector<int> lanes;
    for (const auto &track : timeline->m_allTracks) {
        if (track->isAudioTrack() == audio && track->getId() != primary && !track->isLocked()) {
            lanes.push_back(track->getId());
        }
    }
    std::stable_sort(lanes.begin(), lanes.end(), [&](int a, int b) {
        const bool sameA = timeline->getTrackRole(a) == role;
        const bool sameB = timeline->getTrackRole(b) == role;
        if (sameA != sameB) {
            return sameA;
        }
        const int da = timeline->getTrackPosition(a) - origin;
        const int db = timeline->getTrackPosition(b) - origin;
        if (std::abs(da) != std::abs(db)) {
            return std::abs(da) < std::abs(db);
        }
        return audio ? da < db : da > db;
    });
    for (int lane : lanes) {
        if (timeline->getTrackById_const(lane)->isAvailableWithExceptions(position, duration, {clipId})) {
            if (timeline->requestClipMove(clipId, lane, position, true, true, true, true, undo, redo) == TimelineModel::MoveSuccess) {
                return true;
            }
        }
    }
    // Every lane is taken, open a new one: above the video tracks or below the audio tracks
    int newTrack = -1;
    if (!timeline->requestTrackInsertion(audio ? 0 : -1, newTrack, QString(), audio, undo, redo)) {
        return false;
    }
    return timeline->requestClipMove(clipId, newTrack, position, true, true, true, true, undo, redo) == TimelineModel::MoveSuccess;
}

bool MagneticTimeline::restoreConnections(const std::shared_ptr<TimelineItemModel> &timeline, const Connections &connections, Fun &undo, Fun &redo)
{
    struct Pending
    {
        const Connection *connection;
        int target;
    };
    NormalEditScope scope(timeline);
    std::vector<Pending> pending;
    std::unordered_set<int> moving;
    for (const Connection &c : connections) {
        if (!timeline->isItem(c.item) || timeline->getItemTrackId(c.item) == -1) {
            // Deleted with its parent
            continue;
        }
        int target = c.start;
        for (const auto &anchor : c.anchors) {
            if (anchor.first == endAnchor) {
                target = storylineEnd(timeline) + anchor.second;
                break;
            }
            if (timeline->isClip(anchor.first) && timeline->getClipTrackId(anchor.first) > -1) {
                target = timeline->getClipPosition(anchor.first) + anchor.second;
                break;
            }
        }
        target = std::max(0, target);
        const auto leaves = unitOf(timeline, c.item);
        if (unitStart(timeline, leaves) != target) {
            pending.push_back({&c, target});
            moving.insert(leaves.begin(), leaves.end());
        }
    }
    if (pending.empty()) {
        return true;
    }
    // Park the items first, so that no item can block another one on its way back
    if (parkItems(timeline, moving, undo, redo) < 0) {
        return false;
    }
    std::sort(pending.begin(), pending.end(), [](const Pending &a, const Pending &b) { return a.target < b.target; });
    for (const Pending &p : pending) {
        const int item = p.connection->item;
        const int delta = p.target - unitStart(timeline, unitOf(timeline, item));
        if (moveUnitBy(timeline, item, 0, delta, undo, redo)) {
            continue;
        }
        if (timeline->isClip(item) && !timeline->m_groups->isInGroup(item) &&
            bumpToFreeLane(timeline, item, timeline->getClipTrackId(item), p.target, undo, redo)) {
            continue;
        }
        pCore->displayMessage(i18n("Not enough room for the connected clips"), ErrorMessage);
        return false;
    }
    return true;
}

bool MagneticTimeline::shiftStoryline(const std::shared_ptr<TimelineItemModel> &timeline, int from, int delta, const std::unordered_set<int> &exclude,
                                      Fun &undo, Fun &redo)
{
    std::unordered_set<int> items;
    for (int cid : storylineClips(timeline)) {
        if (timeline->getClipPosition(cid) < from || exclude.count(cid) > 0) {
            continue;
        }
        const auto leaves = unitOf(timeline, cid);
        items.insert(leaves.begin(), leaves.end());
    }
    return moveItemsBy(timeline, items, delta, undo, redo);
}

int MagneticTimeline::insertionPoint(const std::shared_ptr<TimelineItemModel> &timeline, int position, const std::unordered_set<int> &exclude)
{
    int previousEnd = 0;
    for (int cid : storylineClips(timeline)) {
        if (exclude.count(cid) > 0) {
            continue;
        }
        const int start = timeline->getClipPosition(cid);
        const int end = start + timeline->getClipPlaytime(cid);
        if (position < start) {
            // Over a gap, join the storyline
            return previousEnd;
        }
        if (position < end) {
            return position - start <= end - position ? start : end;
        }
        previousEnd = end;
    }
    return previousEnd;
}

void MagneticTimeline::clearFakeState(const std::shared_ptr<TimelineItemModel> &timeline, const std::unordered_set<int> &items)
{
    for (int id : items) {
        QModelIndex ix;
        if (timeline->isClip(id)) {
            timeline->m_allClips[id]->setFakeTrackId(-1);
            timeline->m_allClips[id]->setFakePosition(-1);
            ix = timeline->makeClipIndexFromID(id);
        } else if (timeline->isComposition(id)) {
            timeline->m_allCompositions[id]->setFakeTrackId(-1);
            timeline->m_allCompositions[id]->setFakePosition(-1);
            ix = timeline->makeCompositionIndexFromID(id);
        }
        if (ix.isValid()) {
            timeline->notifyChange(ix, ix, {TimelineModel::FakeTrackIdRole, TimelineModel::FakePositionRole});
        }
    }
}

bool MagneticTimeline::deleteItems(const std::shared_ptr<TimelineItemModel> &timeline, const std::unordered_set<int> &itemIds, Fun &undo, Fun &redo)
{
    NormalEditScope scope(timeline);
    const int tid = primaryTrack(timeline);
    // Sort the selection into storyline units and connected units
    std::unordered_set<int> deleted;
    std::vector<int> storyUnits;
    std::vector<int> otherUnits;
    std::unordered_set<int> seenRoots;
    for (int id : itemIds) {
        if (!timeline->isItem(id) || !seenRoots.insert(timeline->m_groups->getRootId(id)).second) {
            continue;
        }
        const auto leaves = unitOf(timeline, id);
        deleted.insert(leaves.begin(), leaves.end());
        (isStorylineItem(timeline, id) ? storyUnits : otherUnits).push_back(id);
    }
    std::unordered_set<int> storyClips;
    for (int id : deleted) {
        if (timeline->isClip(id) && timeline->getClipTrackId(id) == tid) {
            storyClips.insert(id);
        }
    }
    const Connections conns = connections(timeline, deleted);
    // Connected clips leave with the storyline clip they hang from
    for (const Connection &c : conns) {
        if (c.parent > -1 && storyClips.count(c.parent) > 0) {
            otherUnits.push_back(c.item);
        }
    }
    for (int id : otherUnits) {
        if (timeline->isItem(id) && !timeline->requestItemDeletion(id, undo, redo)) {
            return false;
        }
    }
    if (storyUnits.empty()) {
        return true;
    }
    if (!parkResidents(timeline, conns, storylineTracks(timeline), undo, redo)) {
        return false;
    }
    // Close each gap, from the last one so that earlier positions stay valid
    struct Span
    {
        int item;
        int start;
        int end;
    };
    std::vector<Span> spans;
    for (int id : storyUnits) {
        Span span{id, -1, -1};
        for (int leaf : unitOf(timeline, id)) {
            if (timeline->isClip(leaf) && timeline->getClipTrackId(leaf) == tid) {
                const int start = timeline->getClipPosition(leaf);
                const int end = start + timeline->getClipPlaytime(leaf);
                span.start = span.start == -1 ? start : std::min(span.start, start);
                span.end = std::max(span.end, end);
            }
        }
        spans.push_back(span);
    }
    std::sort(spans.begin(), spans.end(), [](const Span &a, const Span &b) { return a.start > b.start; });
    for (const Span &span : spans) {
        if (!timeline->requestItemDeletion(span.item, undo, redo)) {
            return false;
        }
        if (!shiftStoryline(timeline, span.start, span.start - span.end, {}, undo, redo)) {
            return false;
        }
    }
    return restoreConnections(timeline, conns, undo, redo);
}

bool MagneticTimeline::endMove(const std::shared_ptr<TimelineItemModel> &timeline, int itemId, int trackId, int position, Fun &undo, Fun &redo)
{
    if (!timeline->isClip(itemId) || !timeline->isTrack(trackId)) {
        return false;
    }
    const std::unordered_set<int> unit = unitOf(timeline, itemId);
    clearFakeState(timeline, unit);
    NormalEditScope scope(timeline);
    const int sourceTrack = timeline->getClipTrackId(itemId);
    const int sourcePos = timeline->getClipPosition(itemId);
    const int deltaTrack = timeline->getTrackPosition(trackId) - timeline->getTrackPosition(sourceTrack);
    const int deltaPos = position - sourcePos;
    if (deltaTrack == 0 && deltaPos == 0) {
        return true;
    }
    const int tid = primaryTrack(timeline);
    // The part of the unit on the storyline before the move, and the part landing on it
    int gapStart = -1;
    int gapEnd = -1;
    int landing = -1;
    int landingEnd = -1;
    for (int id : unit) {
        if (!timeline->isClip(id)) {
            continue;
        }
        const int track = timeline->getClipTrackId(id);
        const int start = timeline->getClipPosition(id);
        const int end = start + timeline->getClipPlaytime(id);
        if (track == tid) {
            gapStart = gapStart == -1 ? start : std::min(gapStart, start);
            gapEnd = std::max(gapEnd, end);
        }
        if (!timeline->isAudioTrack(track)) {
            const int pos = timeline->getTrackPosition(track) + deltaTrack;
            if (pos >= 0 && pos < timeline->getTracksCount() && timeline->getTrackIndexFromPosition(pos) == tid) {
                if (landing == -1 || start < timeline->getClipPosition(landing)) {
                    landing = id;
                }
                landingEnd = std::max(landingEnd, end);
            }
        }
    }
    if (gapStart == -1 && landing == -1) {
        // A connected clip moving between lanes
        if (moveUnitBy(timeline, itemId, deltaTrack, deltaPos, undo, redo)) {
            return true;
        }
        return unit.size() == 1 && bumpToFreeLane(timeline, itemId, trackId, position, undo, redo);
    }
    const Connections conns = connections(timeline, unit);
    if (!parkResidents(timeline, conns, storylineTracks(timeline), undo, redo)) {
        return false;
    }
    const int landingPos = landing > -1 ? timeline->getClipPosition(landing) + deltaPos : -1;
    const int landingLength = landing > -1 ? landingEnd - timeline->getClipPosition(landing) : 0;
    const int gapLength = gapStart > -1 ? gapEnd - gapStart : 0;
    if (parkItems(timeline, unit, undo, redo) < 0) {
        return false;
    }
    if (gapStart > -1 && !shiftStoryline(timeline, gapStart, -gapLength, unit, undo, redo)) {
        return false;
    }
    // Positions were picked on the layout before the gap closed
    auto closed = [gapStart, gapLength](int frame) {
        if (gapStart < 0 || frame <= gapStart) {
            return frame;
        }
        return frame >= gapStart + gapLength ? frame - gapLength : gapStart;
    };
    bool result = false;
    if (landing > -1) {
        const int at = insertionPoint(timeline, closed(landingPos), unit);
        result = shiftStoryline(timeline, at, landingLength, unit, undo, redo) &&
                 moveUnitBy(timeline, itemId, deltaTrack, at - timeline->getClipPosition(landing), undo, redo);
    } else {
        // Lifted out of the storyline, it becomes a connected clip
        const int at = closed(position);
        result = moveUnitBy(timeline, itemId, deltaTrack, at - timeline->getClipPosition(itemId), undo, redo);
        if (!result && unit.size() == 1) {
            result = bumpToFreeLane(timeline, itemId, trackId, at, undo, redo);
        }
    }
    return result && restoreConnections(timeline, conns, undo, redo);
}

int MagneticTimeline::insertZone(const std::shared_ptr<TimelineItemModel> &timeline, const QList<int> &trackIds, const QString &binId, int position,
                                 QPoint zone, bool snapToEdit, Fun &undo, Fun &redo)
{
    if (trackIds.isEmpty()) {
        return -1;
    }
    NormalEditScope scope(timeline);
    const int tid = primaryTrack(timeline);
    if (!trackIds.contains(tid)) {
        // A connected clip: dropped where asked, in the closest free lane
        QString binClipId = QStringLiteral("%1/%2/%3").arg(binId.section(QLatin1Char('/'), 0, 0)).arg(zone.x()).arg(zone.y() - 1);
        int newId = -1;
        if (timeline->requestClipInsertion(binClipId, trackIds.first(), position, newId, false, true, false, undo, redo)) {
            return position;
        }
        return -1;
    }
    int at = position;
    if (snapToEdit) {
        at = insertionPoint(timeline, position);
    } else {
        // Split the clip under the insert point first, so that the connected clips over its second half follow it
        const int under = timeline->getClipByPosition(tid, at);
        if (under > -1 && timeline->getClipPosition(under) != at && !TimelineFunctions::requestClipCut(timeline, under, at, undo, redo)) {
            return -1;
        }
    }
    const Connections conns = connections(timeline);
    std::unordered_set<int> tracks = storylineTracks(timeline);
    tracks.insert(trackIds.begin(), trackIds.end());
    if (!parkResidents(timeline, conns, tracks, undo, redo)) {
        return -1;
    }
    if (!TimelineFunctions::insertZone(timeline, trackIds, binId, at, zone, false, false, undo, redo)) {
        return -1;
    }
    return restoreConnections(timeline, conns, undo, redo) ? at : -1;
}
