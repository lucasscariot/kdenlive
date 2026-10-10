/*
    SPDX-FileCopyrightText: 2026 Lucas Scariot <lucas@scariot.fr>
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include "definitions.h"
#include "undohelper.hpp"
#include <QList>
#include <QPoint>
#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

class TimelineItemModel;
class TimelineModel;

/** @brief Final Cut style magnetic editing.

    In magnetic mode the primary storyline, the lowest video track together with the audio of the clips it holds, never keeps
    the gap an edit leaves: deleting, moving or trimming a storyline clip ripples the clips after it. Every other clip is
    connected to the storyline clip under its first frame and follows that clip when the storyline ripples.

    Connections are derived from the layout instead of being stored, so they survive saving, loading and undo without any
    bookkeeping. Each magnetic edit takes the connections first, edits the storyline tracks only, then puts every connected
    clip back at the same offset from its anchor, all inside one undo step.
 */
struct MagneticTimeline
{
    /** @brief A connected item (a clip or group that is not on the storyline) and where it hangs from */
    struct Connection
    {
        int item{-1};
        std::unordered_set<int> leaves;
        int start{0};
        /** @brief The storyline clip under the first frame, -1 when the item starts over a gap */
        int parent{-1};
        /** @brief Storyline clips (-1 for the end of the storyline) and the item's offset from each, in order of preference */
        std::vector<std::pair<int, int>> anchors;
    };
    using Connections = std::vector<Connection>;

    static bool isEnabled(const std::shared_ptr<TimelineItemModel> &timeline);
    /** @brief The video track holding the primary storyline: the track marked as such, or the lowest video track */
    static int primaryTrack(const TimelineModel *timeline);
    static int primaryTrack(const std::shared_ptr<TimelineItemModel> &timeline);
    /** @brief Mark a video track as the primary storyline (undoable) */
    static bool setPrimaryTrack(const std::shared_ptr<TimelineItemModel> &timeline, int trackId, Fun &undo, Fun &redo);
    /** @brief The clips of the primary storyline track, sorted by position */
    static std::vector<int> storylineClips(const std::shared_ptr<TimelineItemModel> &timeline);
    /** @brief The first frame after the last storyline clip */
    static int storylineEnd(const std::shared_ptr<TimelineItemModel> &timeline);
    /** @brief True if the item, or an item grouped with it, is on the primary storyline track */
    static bool isStorylineItem(const std::shared_ptr<TimelineItemModel> &timeline, int itemId);
    /** @brief The items moving together with the given one (its group leaves, or itself) */
    static std::unordered_set<int> unitOf(const std::shared_ptr<TimelineItemModel> &timeline, int itemId);
    /** @brief The storyline clip a connected item hangs from, -1 if none */
    static int parentOf(const std::shared_ptr<TimelineItemModel> &timeline, int itemId);

    /** @brief Every connected item and its anchors, ignoring the items in @p exclude */
    static Connections connections(const std::shared_ptr<TimelineItemModel> &timeline, const std::unordered_set<int> &exclude = {});
    /** @brief Put connected items back at their offset from their anchor, moving to a free lane when their own is taken */
    static bool restoreConnections(const std::shared_ptr<TimelineItemModel> &timeline, const Connections &connections, Fun &undo, Fun &redo);

    /** @brief Move the storyline clips starting at or after @p from by @p delta, ignoring the items in @p exclude */
    static bool shiftStoryline(const std::shared_ptr<TimelineItemModel> &timeline, int from, int delta, const std::unordered_set<int> &exclude, Fun &undo,
                               Fun &redo);
    /** @brief The storyline edit point closest to @p position, where an inserted clip lands */
    static int insertionPoint(const std::shared_ptr<TimelineItemModel> &timeline, int position, const std::unordered_set<int> &exclude = {});

    /** @brief Delete items; deleted storyline clips close their gap and take their connected clips with them */
    static bool deleteItems(const std::shared_ptr<TimelineItemModel> &timeline, const std::unordered_set<int> &itemIds, Fun &undo, Fun &redo);
    /** @brief Commit a drag: storyline clips are reordered, lifted out of or inserted into the storyline */
    static bool endMove(const std::shared_ptr<TimelineItemModel> &timeline, int itemId, int trackId, int position, Fun &undo, Fun &redo);
    /** @brief Insert a bin clip zone. On the storyline it pushes the clips after it, elsewhere it becomes a connected clip.
        @param snapToEdit when true the position snaps to the closest edit point instead of splitting the clip under it
        @return the frame where the zone was inserted, -1 on failure */
    static int insertZone(const std::shared_ptr<TimelineItemModel> &timeline, const QList<int> &trackIds, const QString &binId, int position, QPoint zone,
                          bool snapToEdit, Fun &undo, Fun &redo);

private:
    class NormalEditScope;
    static int unitStart(const std::shared_ptr<TimelineItemModel> &timeline, const std::unordered_set<int> &leaves);
    static bool moveUnitBy(const std::shared_ptr<TimelineItemModel> &timeline, int itemId, int deltaTrack, int deltaPos, Fun &undo, Fun &redo);
    /** @brief Move a group, refusing a move that would overlap other clips */
    static bool groupMove(const std::shared_ptr<TimelineItemModel> &timeline, int itemId, int deltaTrack, int deltaPos, bool moveMirrorTracks, Fun &undo,
                          Fun &redo);
    static bool moveItemsBy(const std::shared_ptr<TimelineItemModel> &timeline, const std::unordered_set<int> &items, int delta, Fun &undo, Fun &redo);
    /** @brief Move items past the end of the timeline, keeping their layout, to free their place during an edit */
    static int parkItems(const std::shared_ptr<TimelineItemModel> &timeline, const std::unordered_set<int> &items, Fun &undo, Fun &redo);
    /** @brief Park the connected items that live on the given tracks */
    static bool parkResidents(const std::shared_ptr<TimelineItemModel> &timeline, const Connections &connections, const std::unordered_set<int> &tracks,
                              Fun &undo, Fun &redo);
    /** @brief Move a single connected clip to the free lane of its type closest to @p trackId, opening a new lane if needed */
    static bool bumpToFreeLane(const std::shared_ptr<TimelineItemModel> &timeline, int clipId, int trackId, int position, Fun &undo, Fun &redo);
    static std::unordered_set<int> storylineTracks(const std::shared_ptr<TimelineItemModel> &timeline);
    static void clearFakeState(const std::shared_ptr<TimelineItemModel> &timeline, const std::unordered_set<int> &items);
};
