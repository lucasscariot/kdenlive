/*
    SPDX-FileCopyrightText: 2026 Lucas Scariot <lucas@scariot.fr>
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "catch.hpp"
#include "test_utils.hpp"
// test specific headers
#include "core.h"
#include "doc/docundostack.hpp"
#include "doc/kdenlivedoc.h"
#include "timeline2/model/magnetictimeline.hpp"

using namespace fakeit;

TEST_CASE("Magnetic timeline", "[Magnetic]")
{
    auto binModel = pCore->projectItemModel();
    binModel->clean();
    std::shared_ptr<DocUndoStack> undoStack = std::make_shared<DocUndoStack>(nullptr);

    KdenliveDoc document(undoStack, {1, 2});
    pCore->projectManager()->testSetDocument(&document);
    QDateTime documentDate = QDateTime::currentDateTime();
    KdenliveTests::updateTimeline(false, QString(), QString(), documentDate, 0);
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);

    // Track positions: 0 is the audio track, 1 the lowest video track (the storyline), 2 the video track above
    int v1 = timeline->getTrackIndexFromPosition(1);
    int v2 = timeline->getTrackIndexFromPosition(2);
    REQUIRE_FALSE(timeline->isAudioTrack(v1));
    REQUIRE_FALSE(timeline->isAudioTrack(v2));
    REQUIRE(MagneticTimeline::primaryTrack(timeline) == v1);

    // 20 frame video only clips
    QString binId = KdenliveTests::createProducer(pCore->getProjectProfile(), "red", binModel, 20);

    // Storyline: c1 [0,20) c2 [20,40) c3 [40,60). Connected on V2: t0 over c1, t1 over c2, t2 over c3
    int c1, c2, c3, t0, t1, t2;
    REQUIRE(timeline->requestClipInsertion(binId, v1, 0, c1));
    REQUIRE(timeline->requestClipInsertion(binId, v1, 20, c2));
    REQUIRE(timeline->requestClipInsertion(binId, v1, 40, c3));
    REQUIRE(timeline->requestClipInsertion(binId, v2, 2, t0));
    REQUIRE(timeline->requestItemResize(t0, 10, true) == 10);
    REQUIRE(timeline->requestClipInsertion(binId, v2, 25, t1));
    REQUIRE(timeline->requestItemResize(t1, 10, true) == 10);
    REQUIRE(timeline->requestClipInsertion(binId, v2, 45, t2));
    REQUIRE(timeline->requestItemResize(t2, 10, true) == 10);
    undoStack->clear();
    timeline->setEditMode(TimelineMode::MagneticEdit);

    auto initialState = [&]() {
        REQUIRE(timeline->checkConsistency());
        REQUIRE(timeline->getClipsCount() == 6);
        REQUIRE(timeline->getClipPosition(c1) == 0);
        REQUIRE(timeline->getClipPosition(c2) == 20);
        REQUIRE(timeline->getClipPosition(c3) == 40);
        REQUIRE(timeline->getClipPosition(t0) == 2);
        REQUIRE(timeline->getClipPosition(t1) == 25);
        REQUIRE(timeline->getClipPosition(t2) == 45);
        REQUIRE(timeline->getClipTrackId(t0) == v2);
        REQUIRE(timeline->getClipTrackId(t1) == v2);
        REQUIRE(timeline->getClipTrackId(t2) == v2);
    };
    auto commit = [&](bool ok, Fun &undo, Fun &redo) {
        REQUIRE(ok);
        pCore->pushUndo(undo, redo, QStringLiteral("magnetic"));
    };
    initialState();

    SECTION("Connections hang from the storyline clip under the first frame")
    {
        REQUIRE(MagneticTimeline::parentOf(timeline, t0) == c1);
        REQUIRE(MagneticTimeline::parentOf(timeline, t1) == c2);
        REQUIRE(MagneticTimeline::parentOf(timeline, t2) == c3);
        REQUIRE(MagneticTimeline::parentOf(timeline, c2) == -1);
        REQUIRE(MagneticTimeline::isStorylineItem(timeline, c2));
        REQUIRE_FALSE(MagneticTimeline::isStorylineItem(timeline, t1));
    }

    SECTION("Deleting a storyline clip closes the gap and takes its connected clips")
    {
        Fun undo = []() { return true; };
        Fun redo = []() { return true; };
        commit(MagneticTimeline::deleteItems(timeline, {c2}, undo, redo), undo, redo);
        REQUIRE(timeline->checkConsistency());
        REQUIRE_FALSE(timeline->isClip(c2));
        REQUIRE_FALSE(timeline->isClip(t1));
        REQUIRE(timeline->getClipPosition(c1) == 0);
        REQUIRE(timeline->getClipPosition(c3) == 20);
        REQUIRE(timeline->getClipPosition(t0) == 2);
        REQUIRE(timeline->getClipPosition(t2) == 25);
        undoStack->undo();
        initialState();
        undoStack->redo();
        REQUIRE(timeline->getClipPosition(c3) == 20);
        REQUIRE(timeline->getClipPosition(t2) == 25);
        undoStack->undo();
        initialState();
    }

    SECTION("Deleting a connected clip leaves the storyline alone")
    {
        Fun undo = []() { return true; };
        Fun redo = []() { return true; };
        commit(MagneticTimeline::deleteItems(timeline, {t1}, undo, redo), undo, redo);
        REQUIRE(timeline->checkConsistency());
        REQUIRE_FALSE(timeline->isClip(t1));
        REQUIRE(timeline->getClipPosition(c3) == 40);
        REQUIRE(timeline->getClipPosition(t2) == 45);
        undoStack->undo();
        initialState();
    }

    SECTION("Reordering the storyline carries the connected clips")
    {
        Fun undo = []() { return true; };
        Fun redo = []() { return true; };
        commit(MagneticTimeline::endMove(timeline, c3, v1, 0, undo, redo), undo, redo);
        REQUIRE(timeline->checkConsistency());
        REQUIRE(timeline->getClipPosition(c3) == 0);
        REQUIRE(timeline->getClipPosition(c1) == 20);
        REQUIRE(timeline->getClipPosition(c2) == 40);
        REQUIRE(timeline->getClipPosition(t2) == 5);
        REQUIRE(timeline->getClipPosition(t0) == 22);
        REQUIRE(timeline->getClipPosition(t1) == 45);
        undoStack->undo();
        initialState();
    }

    SECTION("Moving a storyline clip right lands on the closest edit point")
    {
        Fun undo = []() { return true; };
        Fun redo = []() { return true; };
        // Dropped with its start at 44, inside c3 near its start: after the gap of c1 closes, it lands before c3
        commit(MagneticTimeline::endMove(timeline, c1, v1, 44, undo, redo), undo, redo);
        REQUIRE(timeline->checkConsistency());
        REQUIRE(timeline->getClipPosition(c2) == 0);
        REQUIRE(timeline->getClipPosition(c1) == 20);
        REQUIRE(timeline->getClipPosition(c3) == 40);
        REQUIRE(timeline->getClipPosition(t1) == 5);
        REQUIRE(timeline->getClipPosition(t0) == 22);
        REQUIRE(timeline->getClipPosition(t2) == 45);
        undoStack->undo();
        initialState();
    }

    SECTION("Lifting a clip out of the storyline makes it a connected clip")
    {
        Fun undo = []() { return true; };
        Fun redo = []() { return true; };
        commit(MagneticTimeline::endMove(timeline, c2, v2, 100, undo, redo), undo, redo);
        REQUIRE(timeline->checkConsistency());
        REQUIRE(timeline->getClipPosition(c1) == 0);
        REQUIRE(timeline->getClipPosition(c3) == 20);
        REQUIRE(timeline->getClipTrackId(c2) == v2);
        // Dropped at 100 on the layout before the gap closed, that is 80 after
        REQUIRE(timeline->getClipPosition(c2) == 80);
        REQUIRE(timeline->getClipPosition(t2) == 25);
        // t1 hung from c2 and follows it, in another lane since c2 took its place
        REQUIRE(timeline->getClipPosition(t1) == 85);
        REQUIRE(timeline->getClipTrackId(t1) != v2);
        REQUIRE(timeline->checkConsistency());
        undoStack->undo();
        initialState();
        REQUIRE(timeline->getTracksCount() == 3);
    }

    SECTION("A connected clip dropped on the storyline joins it")
    {
        Fun undo = []() { return true; };
        Fun redo = []() { return true; };
        // Dropped at 18, inside c1 near its end
        commit(MagneticTimeline::endMove(timeline, t0, v1, 18, undo, redo), undo, redo);
        REQUIRE(timeline->checkConsistency());
        REQUIRE(timeline->getClipTrackId(t0) == v1);
        REQUIRE(timeline->getClipPosition(t0) == 20);
        REQUIRE(timeline->getClipPosition(c2) == 30);
        REQUIRE(timeline->getClipPosition(c3) == 50);
        REQUIRE(timeline->getClipPosition(t1) == 35);
        REQUIRE(timeline->getClipPosition(t2) == 55);
        undoStack->undo();
        initialState();
    }

    SECTION("A connected clip moving onto a taken lane moves to a free one")
    {
        Fun undo = []() { return true; };
        Fun redo = []() { return true; };
        commit(MagneticTimeline::endMove(timeline, t0, v2, 27, undo, redo), undo, redo);
        REQUIRE(timeline->checkConsistency());
        REQUIRE(timeline->getClipPosition(t0) == 27);
        REQUIRE(timeline->getClipTrackId(t0) != v2);
        REQUIRE(timeline->getClipTrackId(t0) != v1);
        REQUIRE(timeline->getClipPosition(t1) == 25);
        undoStack->undo();
        initialState();
    }

    SECTION("Inserting on the storyline pushes it and its connected clips")
    {
        Fun undo = []() { return true; };
        Fun redo = []() { return true; };
        int at = MagneticTimeline::insertZone(timeline, {v1}, binId, 31, QPoint(0, 10), true, undo, redo);
        commit(at == 40, undo, redo);
        REQUIRE(timeline->checkConsistency());
        REQUIRE(timeline->getClipsCount() == 7);
        REQUIRE(timeline->getClipPosition(c2) == 20);
        REQUIRE(timeline->getClipPosition(c3) == 50);
        REQUIRE(timeline->getClipPosition(t1) == 25);
        REQUIRE(timeline->getClipPosition(t2) == 55);
        REQUIRE(timeline->getClipByPosition(v1, 40) > -1);
        undoStack->undo();
        initialState();
    }

    SECTION("Inserting at the playhead splits the storyline clip under it")
    {
        Fun undo = []() { return true; };
        Fun redo = []() { return true; };
        // t1 starts at 25, over the second half of c2 once it is split at 22
        int at = MagneticTimeline::insertZone(timeline, {v1}, binId, 22, QPoint(0, 10), false, undo, redo);
        commit(at == 22, undo, redo);
        REQUIRE(timeline->checkConsistency());
        REQUIRE(timeline->getClipsCount() == 8);
        REQUIRE(timeline->getClipPosition(c2) == 20);
        REQUIRE(timeline->getClipPlaytime(c2) == 2);
        REQUIRE(timeline->getClipPosition(t1) == 35);
        REQUIRE(timeline->getClipPosition(c3) == 50);
        REQUIRE(timeline->getClipPosition(t2) == 55);
        undoStack->undo();
        initialState();
    }

    SECTION("Trimming a storyline clip ripples the connected clips after it")
    {
        REQUIRE(timeline->requestItemRippleResize(timeline, c1, 10, true) == 10);
        REQUIRE(timeline->checkConsistency());
        REQUIRE(timeline->getClipPlaytime(c1) == 10);
        REQUIRE(timeline->getClipPosition(c2) == 10);
        REQUIRE(timeline->getClipPosition(c3) == 30);
        REQUIRE(timeline->getClipPosition(t0) == 2);
        REQUIRE(timeline->getClipPosition(t1) == 15);
        REQUIRE(timeline->getClipPosition(t2) == 35);
        undoStack->undo();
        initialState();
    }

    SECTION("Another track can hold the storyline")
    {
        Fun undo = []() { return true; };
        Fun redo = []() { return true; };
        commit(MagneticTimeline::setPrimaryTrack(timeline, v2, undo, redo), undo, redo);
        REQUIRE(MagneticTimeline::primaryTrack(timeline) == v2);
        REQUIRE(MagneticTimeline::isStorylineItem(timeline, t1));
        REQUIRE_FALSE(MagneticTimeline::isStorylineItem(timeline, c2));
        // c2 starts at 20, before t1: it hangs over a gap of the new storyline
        REQUIRE(MagneticTimeline::parentOf(timeline, c2) == -1);
        REQUIRE(MagneticTimeline::parentOf(timeline, c3) == -1);
        undoStack->undo();
        REQUIRE(MagneticTimeline::primaryTrack(timeline) == v1);
    }

    SECTION("Track roles are guessed from the name until assigned")
    {
        REQUIRE(timeline->getTrackRole(v1) == QStringLiteral("video"));
        int a1 = timeline->getTrackIndexFromPosition(0);
        REQUIRE(timeline->getTrackRole(a1) == QStringLiteral("dialogue"));
        timeline->setTrackName(a1, QStringLiteral("Music bed"));
        REQUIRE(timeline->getTrackRole(a1) == QStringLiteral("music"));
        Fun undo = []() { return true; };
        Fun redo = []() { return true; };
        commit(timeline->setTrackRole(a1, QStringLiteral("effects"), undo, redo), undo, redo);
        REQUIRE(timeline->getTrackRole(a1) == QStringLiteral("effects"));
        REQUIRE_FALSE(timeline->setTrackRole(a1, QStringLiteral("titles"), undo, redo));
        undoStack->undo();
        REQUIRE(timeline->getTrackRole(a1) == QStringLiteral("music"));
    }

    binModel->clean();
    pCore->projectManager()->closeCurrentDocument(false, false);
}

TEST_CASE("Magnetic timeline with linked audio", "[Magnetic]")
{
    auto binModel = pCore->projectItemModel();
    binModel->clean();
    std::shared_ptr<DocUndoStack> undoStack = std::make_shared<DocUndoStack>(nullptr);

    KdenliveDoc document(undoStack, {2, 2});
    pCore->projectManager()->testSetDocument(&document);
    QDateTime documentDate = QDateTime::currentDateTime();
    KdenliveTests::updateTimeline(false, QString(), QString(), documentDate, 0);
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);

    // Track positions: A2, A1, V1, V2
    int a2 = timeline->getTrackIndexFromPosition(0);
    int a1 = timeline->getTrackIndexFromPosition(1);
    int v1 = timeline->getTrackIndexFromPosition(2);
    REQUIRE(timeline->isAudioTrack(a1));
    REQUIRE(timeline->isAudioTrack(a2));
    REQUIRE(MagneticTimeline::primaryTrack(timeline) == v1);

    QMap<int, QString> audioInfo;
    audioInfo.insert(1, QStringLiteral("stream1"));
    KdenliveTests::setAudioTargets(timeline, audioInfo);

    // Two 10 frame clips with audio on the storyline, their sound on A1
    QString avBinId = KdenliveTests::createProducerWithSound(pCore->getProjectProfile(), binModel, 10);
    int av1, av2;
    REQUIRE(timeline->requestClipInsertion(avBinId, v1, 0, av1));
    REQUIRE(timeline->requestClipInsertion(avBinId, v1, 10, av2));
    int sound1 = timeline->getClipByPosition(a1, 0);
    int sound2 = timeline->getClipByPosition(a1, 10);
    REQUIRE(sound1 > -1);
    REQUIRE(sound2 > -1);
    REQUIRE(timeline->getGroupElements(av2).count(sound2) == 1);
    // A free clip on A1 past the storyline end: its audio part, ungrouped from its video
    int voice;
    REQUIRE(timeline->requestClipInsertion(avBinId, v1, 30, voice));
    int voiceSound = timeline->getClipByPosition(a1, 30);
    REQUIRE(timeline->requestClipUngroup(voice));
    REQUIRE(timeline->requestItemDeletion(voice));
    undoStack->clear();
    timeline->setEditMode(TimelineMode::MagneticEdit);

    auto initialState = [&]() {
        REQUIRE(timeline->checkConsistency());
        REQUIRE(timeline->getClipPosition(av1) == 0);
        REQUIRE(timeline->getClipPosition(sound1) == 0);
        REQUIRE(timeline->getClipPosition(av2) == 10);
        REQUIRE(timeline->getClipPosition(sound2) == 10);
        REQUIRE(timeline->getClipPosition(voiceSound) == 30);
        REQUIRE(timeline->getClipTrackId(voiceSound) == a1);
    };
    initialState();
    REQUIRE(MagneticTimeline::isStorylineItem(timeline, sound1));
    REQUIRE_FALSE(MagneticTimeline::isStorylineItem(timeline, voiceSound));

    SECTION("Deleting a clip ripples its sound and the free audio after it")
    {
        Fun undo = []() { return true; };
        Fun redo = []() { return true; };
        REQUIRE(MagneticTimeline::deleteItems(timeline, {sound1}, undo, redo));
        pCore->pushUndo(undo, redo, QStringLiteral("magnetic"));
        REQUIRE(timeline->checkConsistency());
        REQUIRE_FALSE(timeline->isClip(av1));
        REQUIRE_FALSE(timeline->isClip(sound1));
        REQUIRE(timeline->getClipPosition(av2) == 0);
        REQUIRE(timeline->getClipPosition(sound2) == 0);
        REQUIRE(timeline->getClipPosition(voiceSound) == 20);
        REQUIRE(timeline->getClipTrackId(voiceSound) == a1);
        undoStack->undo();
        initialState();
    }

    SECTION("Reordering moves a clip with its sound")
    {
        Fun undo = []() { return true; };
        Fun redo = []() { return true; };
        REQUIRE(MagneticTimeline::endMove(timeline, av2, v1, 0, undo, redo));
        pCore->pushUndo(undo, redo, QStringLiteral("magnetic"));
        REQUIRE(timeline->checkConsistency());
        REQUIRE(timeline->getClipPosition(av2) == 0);
        REQUIRE(timeline->getClipPosition(sound2) == 0);
        REQUIRE(timeline->getClipPosition(av1) == 10);
        REQUIRE(timeline->getClipPosition(sound1) == 10);
        REQUIRE(timeline->getClipPosition(voiceSound) == 30);
        undoStack->undo();
        initialState();
    }
    Q_UNUSED(a2)

    binModel->clean();
    pCore->projectManager()->closeCurrentDocument(false, false);
}
