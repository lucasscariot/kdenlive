/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "mcptools.h"
#include "livebridge.h"
#include "mcpserver.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUrl>

namespace {
QJsonObject objectSchema(const QJsonObject &properties, const QStringList &optional = {})
{
    QJsonArray required;
    for (auto it = properties.begin(); it != properties.end(); ++it)
        if (!optional.contains(it.key())) required.append(it.key());
    return {{"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}};
}

QJsonObject stringSchema()
{
    return {{"type", "string"}, {"minLength", 1}};
}

QJsonObject frameSchema(int minimum = 0)
{
    return {{"type", "integer"}, {"minimum", minimum}, {"maximum", 2147483647}};
}

struct Command
{
    QJsonObject properties;
    QStringList optional;
};

const QJsonObject markerFormat{{"type", "string"}, {"enum", QJsonArray{"json", "csv", "kdenlive"}}};

const QMap<QString, Command> &commands()
{
    static const auto result = [] {
        const auto text = stringSchema();
        const auto frame = frameSchema();
        const QJsonObject bin{{"type", "string"}, {"pattern", "^[0-9]+$"}};
        const QJsonObject edge{{"type", "string"}, {"enum", QJsonArray{"left", "right"}}};
        const QJsonObject unit{{"type", "number"}, {"minimum", 0}, {"maximum", 1}};
        const QJsonObject params{{"type", "object"}, {"additionalProperties", QJsonObject{{"type", "string"}}}};
        const QJsonObject size{{"type", "integer"}, {"minimum", 16}, {"maximum", 8192}};
        const QJsonObject comment{{"type", "string"}, {"maxLength", 4096}};
        const QJsonObject flag{{"type", "boolean"}};
        const QJsonObject removeMode{{"type", "string"}, {"enum", QJsonArray{"lift", "extract"}}};
        const QJsonObject trackList{{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"uniqueItems", true}, {"items", frame}};
        const QJsonObject category{{"anyOf", QJsonArray{QJsonObject{{"type", "integer"}}, QJsonObject{{"type", "string"}, {"minLength", 1}}}},
                                   {"description", "Category index or name from desktop_capabilities markerCategories"}};
        const QJsonObject titleItem{
            {"type", "object"},
            {"properties", QJsonObject{{"index", frame},
                                       {"text", QJsonObject{{"type", "string"}}},
                                       {"x", QJsonObject{{"type", "number"}}},
                                       {"y", QJsonObject{{"type", "number"}}},
                                       {"fontPixelSize", frameSchema(1)},
                                       {"alignment", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"left", "center", "right"}}}}}},
            {"required", QJsonArray{"index"}},
            {"additionalProperties", false}};
        return QMap<QString, Command>{
            {"save_as", {{{"path", text}}}},
            {"set_profile", {{{"width", size}, {"height", size}}}},
            {"reframe",
             {{{"clipId", frame},
               {"mode", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"fill", "fit"}}}},
               {"focusX", unit},
               {"focusY", unit},
               {"endFocusX", unit},
               {"endFocusY", unit},
               {"zoom", QJsonObject{{"type", "number"}, {"minimum", 0.1}, {"maximum", 10}}}},
              {"focusX", "focusY", "endFocusX", "endFocusY", "zoom"}}},
            {"effect_add", {{{"clipId", frame}, {"binId", bin}, {"effectId", text}, {"params", params}}, {"clipId", "binId", "params"}}},
            {"effect_set", {{{"clipId", frame}, {"binId", bin}, {"index", frame}, {"params", params}}, {"clipId", "binId"}}},
            {"effect_remove", {{{"clipId", frame}, {"binId", bin}, {"index", frame}}, {"clipId", "binId"}}},
            {"title_edit",
             {{{"binId", bin}, {"width", size}, {"height", size}, {"items", QJsonObject{{"type", "array"}, {"items", titleItem}}}},
              {"width", "height", "items"}}},
            {"marker_add",
             {{{"position", frame}, {"duration", frame}, {"comment", comment}, {"category", category}, {"binId", bin}},
              {"duration", "comment", "category", "binId"}}},
            {"marker_edit",
             {{{"position", frame}, {"binId", bin}, {"newPosition", frame}, {"duration", frame}, {"comment", comment}, {"category", category}},
              {"binId", "newPosition", "duration", "comment", "category"}}},
            {"marker_remove",
             {{{"binId", bin},
               {"position", frame},
               {"all", QJsonObject{{"type", "boolean"}, {"const", true}}},
               {"category", category},
               {"range", objectSchema({{"start", frame}, {"end", frameSchema(1)}})}},
              {"binId", "position", "all", "category", "range"}}},
            {"marker_import", {{{"format", markerFormat}, {"text", QJsonObject{{"type", "string"}, {"minLength", 1}}}, {"binId", bin}}, {"binId"}}},
            {"render", {{{"path", text}, {"preset", text}}, {"preset"}}},
            {"import", {{{"path", text}}}},
            {"remove_asset", {{{"binId", bin}}}},
            {"remove_clip",
             {{{"clipId", frame},
               {"mode", removeMode},
               {"group", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"whole", "single"}}}},
               {"allTracks", flag}},
              {"mode", "group", "allTracks"}}},
            {"split", {{{"clipId", frame}, {"trackId", frame}, {"position", frame}, {"allTracks", flag}}, {"clipId", "trackId", "allTracks"}}},
            {"remove_range",
             {{{"start", frame}, {"end", frameSchema(1)}, {"trackIds", trackList}, {"allTracks", flag}, {"mode", removeMode}},
              {"trackIds", "allTracks", "mode"}}},
            {"remove_gap", {{{"position", frame}, {"trackId", frame}, {"allTracks", flag}}, {"trackId", "allTracks"}}},
            {"insert_space", {{{"position", frame}, {"duration", frameSchema(1)}, {"trackId", frame}, {"allTracks", flag}}, {"trackId", "allTracks"}}},
            {"group", {{{"clipIds", QJsonObject{{"type", "array"}, {"minItems", 2}, {"maxItems", 200}, {"uniqueItems", true}, {"items", frame}}}}}},
            {"ungroup", {{{"clipId", frame}, {"groupId", frame}}, {"clipId", "groupId"}}},
            {"speed",
             {{{"clipId", frame},
               {"speed", QJsonObject{{"type", "number"},
                                     {"minimum", -100},
                                     {"maximum", 100},
                                     {"description", "Factor: 1 normal, 0.5 half, 2 double, negative reverse; 0.01 <= |speed|"}}},
               {"pitchCompensation", flag}},
              {"pitchCompensation"}}},
            {"enable", {{{"clipId", frame}, {"enabled", flag}, {"linked", flag}}, {"linked"}}},
            {"replace_media", {{{"binId", bin}, {"replacementBinId", bin}}}},
            {"insert",
             {{{"binId", bin},
               {"trackId", frame},
               {"position", frame},
               {"sourceIn", frame},
               {"sourceOut", frameSchema(1)},
               {"media", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"video", "audio"}}}},
               {"mode", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"normal", "overwrite", "insert"}}}},
               {"linked", flag},
               {"audioTrackId", frame},
               {"allTracks", flag}},
              {"mode", "linked", "audioTrackId", "allTracks"}}},
            {"move", {{{"clipId", frame}, {"trackId", frame}, {"position", frame}}}},
            {"trim", {{{"clipId", frame}, {"duration", frameSchema(1)}, {"edge", edge}}}},
            {"audio_envelope",
             {{{"clipId", frame}, {"fadeIn", frame}, {"fadeOut", frame}, {"gainDb", QJsonObject{{"type", "number"}, {"minimum", -60}, {"maximum", 0}}}}}},
            {"rename_track", {{{"trackId", frame}, {"name", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}}}}},
            {"save", {}},
            {"undo",
             {{{"count", frameSchema(1)}, {"toIndex", frame}, {"revertSession", QJsonObject{{"type", "boolean"}, {"const", true}}}},
              {"count", "toIndex", "revertSession"}}},
            {"redo", {{{"count", frameSchema(1)}, {"toIndex", frame}}, {"count", "toIndex"}}}};
    }();
    return result;
}

QJsonObject editFields()
{
    return {{"sessionId", QJsonObject{{"type", "string"}, {"format", "uuid"}}},
            {"expectedRevision", QJsonObject{{"type", "integer"}, {"minimum", 0}}},
            {"requestId", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}}};
}

// MCP tool annotations. Hints describe the edit itself; requestId receipts make any identical retry safe regardless.
struct Hints
{
    bool readOnly = false;
    bool destructive = true;
    bool idempotent = false;
};
const Hints readOnlyTool{true, false, true};
const Hints additive{false, false, false};
const Hints additiveRepeatable{false, false, true};
const Hints overwriting{false, true, false};
const Hints overwritingRepeatable{false, true, true};

struct Tool
{
    QString name;
    QString description;
    QString command;
    Hints hints;
};

const QList<Tool> &editingTools()
{
    static const QList<Tool> result{
        {"desktop_media_import", "Import a local file into the visible project bin. Returns binId; poll desktop_state until ready. Reuses existing paths.",
         "import", additiveRepeatable},
        {"desktop_media_remove", "Remove an unused binId using native Undo. Rejects media used in any sequence. Keeps the source file on disk.", "remove_asset",
         overwritingRepeatable},
        {"desktop_clip_insert",
         "Insert ready bin media [sourceIn, sourceOut) (exclusive, project frames) at position on trackId; media picks the stream placed there. With "
         "media video an A/V clip is linked by default: its audio goes to audioTrackId (default: the mirror or nearest unlocked audio track) and both "
         "halves are grouped and linked (linked: false inserts video only). mode normal (default) refuses an occupied range (OVERLAP), overwrite "
         "replaces what is there, insert cuts at position and pushes later clips right on the receiving tracks (allTracks: true on every unlocked "
         "track). Returns clipId, audioClipId and groupId.",
         "insert", overwriting},
        {"desktop_clip_remove",
         "Remove a timeline clip. group whole (default) removes its whole group, as the GUI does; single takes the clip out of its group first. mode "
         "lift (default) leaves a gap; extract closes it on each removed clip's track, or with allTracks: true removes the clip's time range from "
         "every unlocked track (Extract zone). Keeps bin assets and files. Returns removedClipIds.",
         "remove_clip", overwritingRepeatable},
        {"desktop_clip_split",
         "Cut at an absolute frame strictly inside a clip: one clip (clipId), the clip under position on trackId, or every clip under position on "
         "all unlocked tracks (allTracks: true). Grouped and linked clips are cut together and give two linked groups. Returns pieces "
         "[{trackId, leftClipId, rightClipId}] (and leftClipId/rightClipId of the named clip).",
         "split", overwritingRepeatable},
        {"desktop_range_remove",
         "Remove the timeline range [start, end) from trackIds or all unlocked tracks (allTracks: true), cutting clips at both ends. mode lift "
         "(default) leaves a gap, extract closes it (ripple). One Undo step: the way to cut a span such as a silence out of linked audio and video. "
         "Returns removedClipIds and newClipIds.",
         "remove_range", overwriting},
        {"desktop_gap_remove",
         "Close the gap (blank) at position on trackId, or on all unlocked tracks (allTracks: true, the gap must be blank on each), moving later "
         "clips left as the GUI's Remove space does. Refuses a gap between two clips of one group. Returns removed (frames).",
         "remove_gap", overwriting},
        {"desktop_space_insert",
         "Insert duration blank frames at position on trackId or all unlocked tracks (allTracks: true): clips at or after position, a clip spanning it "
         "included, move right with their groups, as the GUI's Insert space does.",
         "insert_space", overwriting},
        {"desktop_clip_group",
         "Group 2 or more timeline clips (clipIds). The video and audio of one bin clip become a linked A/V pair. Returns groupId and the group's "
         "clipIds.",
         "group", additiveRepeatable},
        {"desktop_clip_ungroup",
         "Dissolve the topmost group of clipId, or groupId from desktop_state. Ungrouping a linked A/V pair unlinks it. Returns the former member "
         "clipIds.",
         "ungroup", overwritingRepeatable},
        {"desktop_clip_speed",
         "Change a clip's playback speed as a factor (1 normal, 0.5 half, 2 double, negative reverse); its linked clip changes too. The source range "
         "is kept, so duration scales (OVERLAP when it would run into the next clip). pitchCompensation keeps audio pitch. Returns speed and "
         "duration.",
         "speed", overwritingRepeatable},
        {"desktop_clip_enable",
         "Enable or disable a timeline clip, and its linked A/V partner unless linked: false. A disabled clip stays in place but is not played.", "enable",
         overwritingRepeatable},
        {"desktop_media_replace",
         "Replace every use of binId with replacementBinId. Both must be ready with matching streams; replacement must cover the original "
         "duration.",
         "replace_media", overwritingRepeatable},
        {"desktop_clip_move", "Move a native clip to a track and frame position, respecting locks, groups and collisions.", "move", overwritingRepeatable},
        {"desktop_clip_trim", "Trim a clip edge to duration frames. Read actualDuration in the result.", "trim", overwritingRepeatable},
        {"desktop_audio_envelope",
         "Add native volume automation to an audio clip. fadeIn/fadeOut use project frames; gainDb is -60..0. Rejects existing volume effects and "
         "overlapping fades.",
         "audio_envelope", additiveRepeatable},
        {"desktop_track_rename", "Rename a native timeline track using Undo.", "rename_track", overwritingRepeatable},
        {"desktop_project_save", "Save the open project to its existing local file. All bin media must be ready.", "save", overwritingRepeatable},
        {"desktop_project_save_as",
         "Save the project to a new .kdenlive file in the project folder or additional media folder and keep editing that copy. Use it before "
         "destructive format changes such as desktop_project_profile.",
         "save_as", additiveRepeatable},
        {"desktop_project_profile",
         "Change the project frame size, e.g. 1080x1920 for vertical video. Keeps the frame rate. Not undoable: save a copy first. Existing clips are "
         "letterboxed until reframed.",
         "set_profile", overwritingRepeatable},
        {"desktop_clip_reframe",
         "Fill or fit a video clip in the project frame with a native Transform effect. focusX/focusY (0..1, default 0.5) choose the visible part: 0 "
         "shows the left/top edge, 1 the right/bottom. endFocusX/endFocusY animate a pan to the clip end. zoom scales further. Verify with "
         "desktop_frame_capture.",
         "reframe", overwritingRepeatable},
        {"desktop_effect_add",
         "Add a native effect by effectId (see desktop_effect_list) with optional MLT parameter strings. Target one timeline clip (clipId) or a bin "
         "clip (binId), whose effects apply to all its timeline instances.",
         "effect_add", additive},
        {"desktop_effect_set",
         "Set parameters of the effect at index on a clip (clipId) or bin clip (binId), as MLT strings. Animated values use "
         "'frame=value;frame=value' with frames counted from keyframeOrigin (desktop_effect_list).",
         "effect_set", overwritingRepeatable},
        {"desktop_effect_remove", "Remove the effect at index from a timeline clip (clipId) or bin clip (binId).", "effect_remove", overwriting},
        {"desktop_title_edit",
         "Edit a title clip: canvas width/height (match the project frame after a profile change) and text items by index (text, x, y, "
         "fontPixelSize, alignment). Read items with desktop_title_read first.",
         "title_edit", overwritingRepeatable},
        {"desktop_marker_add",
         "Add a guide to the active sequence, or with binId a marker to that bin clip (frames relative to the clip). duration > 0 makes a range "
         "marker. category is an index or name from desktop_capabilities markerCategories (default: the configured default category). A marker "
         "already at position is replaced: its comment, category and duration all take the new values (replaced: true).",
         "marker_add", overwritingRepeatable},
        {"desktop_marker_edit",
         "Change the guide (or, with binId, clip marker) at position: newPosition, duration (0 makes a point), comment and/or category. Fields left "
         "out keep their value. Returns marker and previous.",
         "marker_edit", overwritingRepeatable},
        {"desktop_marker_remove",
         "Remove guides (or, with binId, clip markers) as one Undo step: the one at position, all:true, or those matching category and/or range "
         "{start, end} (positions in [start, end)). Returns the removed markers.",
         "marker_remove", overwritingRepeatable},
        {"desktop_marker_import",
         "Add guides (or, with binId, clip markers) from text produced by desktop_marker_export: format json, csv (header row; position in frames "
         "or timecode) or kdenlive (native guide JSON). One Undo step; markers at existing positions are replaced. All entries are validated "
         "first.",
         "marker_import", overwritingRepeatable},
        {"desktop_render",
         "Start rendering the active sequence to a new file in the project folder or additional media folder. preset defaults to the configured one "
         "(usually MP4-H264/AAC). Poll desktop_render_status.",
         "render", additive},
        {"desktop_undo",
         "Undo in shared Kdenlive history, including manual edits: the latest step, count steps, or down to toIndex (an undo index from "
         "desktop_history). revertSession: true undoes every edit this MCP connection made in the current editing session, only when no other edit "
         "lies between them (otherwise HISTORY_INTERLEAVED lists the blocking entries). Returns undone entries.",
         "undo", overwriting},
        {"desktop_redo", "Redo in shared Kdenlive history: the next step, count steps, or up to toIndex. Returns redone entries.", "redo", overwriting},
        {"desktop_batch",
         "Apply up to 200 editing commands (import, remove_asset, remove_clip, audio_envelope, rename_track, insert, move, trim, reframe, effect_*, "
         "title_edit, marker_*, split, remove_range, remove_gap, insert_space, group, ungroup, speed, enable) as one Undo step. Position-addressed "
         "commands chain well: apply range removals from the last range to the first. Each command has the same fields as desktop_apply commands. Stops and "
         "rolls back everything on the first "
         "failure, reporting failedIndex.",
         "batch", overwriting}};
    return result;
}

QJsonObject definition(const QString &name, const QString &description, const QJsonObject &properties, const Hints &hints, const QStringList &optional = {})
{
    return {{"name", name},
            {"description", description},
            {"inputSchema", objectSchema(properties, optional)},
            {"annotations",
             QJsonObject{
                 {"readOnlyHint", hints.readOnly}, {"destructiveHint", hints.destructive}, {"idempotentHint", hints.idempotent}, {"openWorldHint", false}}}};
}

QJsonObject failure(const QString &code, const QString &message)
{
    return {{"ok", false}, {"error", QJsonObject{{"code", code}, {"message", message}}}};
}

bool within(const QString &path, const QString &root)
{
    if (root.isEmpty()) return false;
    return path == root || path.startsWith(root.endsWith(QDir::separator()) ? root : root + QDir::separator());
}
} // namespace

QJsonArray McpTools::definitions()
{
    QJsonArray result;
    result.append(definition("desktop_capabilities", "Read native editing capabilities of this Kdenlive instance.", {}, readOnlyTool));
    result.append(
        definition("desktop_state",
                   "Read the visible sequence with native IDs, sessionId, revision and shared Undo history. Default sections, also embedded in every "
                   "edit result: tracks (type, lock, mute/hide, active), clips (source range, speed, enabled, group, linked A/V partner, mixes) and "
                   "gaps, compositions, markers (guides), bin items and folders, sequences. include picks sections and adds subtitles, effects "
                   "(per-clip effect ids) or media (bin frame size, fps, streams). trackId and range {start, end} (end exclusive) narrow the timeline "
                   "sections; counts always cover the whole sequence.",
                   {{"include", QJsonObject{{"type", "array"},
                                            {"uniqueItems", true},
                                            {"items", QJsonObject{{"type", "string"}, {"enum", QJsonArray::fromStringList(LiveBridge::stateSectionNames())}}}}},
                    {"trackId", frameSchema()},
                    {"range", objectSchema({{"start", frameSchema()}, {"end", frameSchema(1)}})}},
                   readOnlyTool, {"include", "trackId", "range"}));
    result.append(definition("desktop_frame_capture",
                             "Render one frame of the active sequence at a timeline position as a PNG image (default 540 px wide). Does not move the "
                             "playhead.",
                             {{"position", frameSchema()}, {"width", QJsonObject{{"type", "integer"}, {"minimum", 64}, {"maximum", 1920}}}}, readOnlyTool,
                             {"width"}));
    result.append(definition("desktop_effect_list",
                             "List the effects on a timeline clip (clipId) or bin clip (binId, e.g. text effects on title-like color clips) with their "
                             "parameters and keyframeOrigin, and/or search the effect catalog by query.",
                             {{"clipId", frameSchema()}, {"binId", QJsonObject{{"type", "string"}, {"pattern", "^[0-9]+$"}}}, {"query", stringSchema()}},
                             readOnlyTool, {"clipId", "binId", "query"}));
    result.append(definition("desktop_title_read", "Read a title clip's canvas size, the project frame size and its items (text, position, font size, box).",
                             {{"binId", QJsonObject{{"type", "string"}, {"pattern", "^[0-9]+$"}}}}, readOnlyTool));
    result.append(definition("desktop_render_status", "Read progress, status and errors of renders started with desktop_render.", {}, readOnlyTool));
    result.append(
        definition("desktop_history",
                   "Read the shared Undo history as a change log, oldest first: each entry's undoIndex, text, state (applied/undone) and origin (mcp, user or "
                   "unknown). MCP entries add sessionId, requestId, command, tool, client, revisions, a summary of affected ids and, for desktop_batch, "
                   "children. Filters: origin, sessionId, since (an undoIndex, exclusive, or an ISO 8601 time) and includeUndone; limit keeps the newest "
                   "(default 50). Also returns counts and the logFile path.",
                   {{"limit", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}},
                    {"origin", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"all", "mcp", "user", "unknown"}}}},
                    {"sessionId", stringSchema()},
                    {"since", QJsonObject{{"anyOf", QJsonArray{frameSchema(), QJsonObject{{"type", "string"}, {"format", "date-time"}}}}}},
                    {"includeUndone", QJsonObject{{"type", "boolean"}}}},
                   readOnlyTool, {"limit", "origin", "sessionId", "since", "includeUndone"}));
    result.append(definition("desktop_marker_export",
                             "Export the active sequence guides, or with binId a bin clip's markers, as text: json (the desktop_state marker shape), csv "
                             "(position in frames and HH:MM:SS:FF timecode, duration, category, categoryName, color, comment) or kdenlive (native guide "
                             "JSON, as Kdenlive's own export). desktop_marker_import reads all three.",
                             {{"format", markerFormat}, {"binId", QJsonObject{{"type", "string"}, {"pattern", "^[0-9]+$"}}}}, readOnlyTool, {"binId"}));
    auto fields = editFields();
    QJsonArray variants;
    QJsonArray batchVariants;
    const QStringList unbatchable{"save", "save_as", "set_profile", "render", "replace_media", "undo", "redo"};
    for (auto it = commands().begin(); it != commands().end(); ++it) {
        auto properties = it.value().properties;
        properties.insert("type", QJsonObject{{"const", it.key()}, {"type", "string"}});
        variants.append(objectSchema(properties, it.value().optional));
        if (!unbatchable.contains(it.key())) batchVariants.append(objectSchema(properties, it.value().optional));
    }
    const QJsonObject batch{{"commands", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 200}, {"items", QJsonObject{{"oneOf", batchVariants}}}}}};
    {
        auto properties = batch;
        properties.insert("type", QJsonObject{{"const", "batch"}, {"type", "string"}});
        variants.append(objectSchema(properties));
    }
    fields.insert("command", QJsonObject{{"oneOf", variants}});
    result.append(definition("desktop_apply",
                             "Apply one native operation with sessionId/revision from desktop_state. Retry uncertain outcomes only with the identical "
                             "requestId and payload. Import/replacement load asynchronously. All edits use native Undo; saving does not add history.",
                             {{"request", objectSchema(fields)}}, overwriting));
    for (const auto &tool : editingTools()) {
        auto properties = editFields();
        const auto command = tool.command == QLatin1String("batch") ? Command{batch, {}} : commands().value(tool.command);
        for (auto it = command.properties.begin(); it != command.properties.end(); ++it)
            properties.insert(it.key(), it.value());
        result.append(definition(tool.name, tool.description + QStringLiteral(" Use sessionId/revision from desktop_state and a unique requestId."), properties,
                                 tool.hints, command.optional));
    }
    return result;
}

QJsonObject McpTools::call(LiveBridge &engine, const QString &name, const QJsonObject &arguments, const QString &additionalMediaRoot, const McpCaller &caller)
{
    if (name == QLatin1String("desktop_state")) return engine.stateFor(arguments);
    if (name == QLatin1String("desktop_capabilities") || name == QLatin1String("desktop_render_status")) {
        if (!arguments.isEmpty()) return failure("INVALID_ARGUMENTS", "This tool takes no arguments.");
        if (name == QLatin1String("desktop_render_status")) return engine.renderStatus();
        return QJsonDocument::fromJson(engine.capabilities().toUtf8()).object();
    }
    if (name == QLatin1String("desktop_frame_capture")) return engine.frameCapture(arguments);
    if (name == QLatin1String("desktop_effect_list")) return engine.effectList(arguments);
    if (name == QLatin1String("desktop_title_read")) return engine.titleRead(arguments);
    if (name == QLatin1String("desktop_marker_export")) return engine.markerExport(arguments);
    if (name == QLatin1String("desktop_history")) return engine.history(arguments);
    QJsonObject request;
    if (name == QLatin1String("desktop_apply")) {
        if (arguments.size() != 1 || !arguments.value("request").isObject()) return failure("INVALID_ARGUMENTS", "Expected a request object.");
        request = arguments.value("request").toObject();
    } else {
        for (const auto &tool : editingTools())
            if (tool.name == name) {
                auto command = arguments;
                for (const auto &key : {QStringLiteral("sessionId"), QStringLiteral("expectedRevision"), QStringLiteral("requestId")})
                    request.insert(key, command.take(key));
                command.insert("type", tool.command);
                request.insert("command", command);
                break;
            }
    }
    if (request.isEmpty()) return failure("UNKNOWN_TOOL", "Unknown native tool.");
    const auto authorize = [&]() -> QJsonObject {
        const auto state = QJsonDocument::fromJson(engine.state().toUtf8()).object();
        if (!state.value("ok").toBool()) return state;
        const QUrl projectUrl(state.value("documentUrl").toString());
        const QFileInfo project(projectUrl.toLocalFile());
        if (!projectUrl.isLocalFile() || !project.isFile())
            return failure("DESKTOP_SAVE_REQUIRED", "Save the active project to a local file before editing through MCP.");
        const QString projectRoot = QFileInfo(project.canonicalFilePath()).absolutePath();
        const QString extraRoot = additionalMediaRoot.isEmpty() ? QString{} : QFileInfo(additionalMediaRoot).canonicalFilePath();
        const auto check = [&](const QJsonObject &command) -> QJsonObject {
            const auto type = command.value("type").toString();
            if (type == QLatin1String("save_as") || type == QLatin1String("render")) {
                // New files: check the folder they will be written to.
                const QFileInfo target(command.value("path").toString());
                const QString folder = QFileInfo(target.absolutePath()).canonicalFilePath();
                if (!target.isAbsolute() || folder.isEmpty()) return failure("INVALID_PATH", "Output must be an absolute path in an existing folder.");
                if (!within(folder, projectRoot) && !within(folder, extraRoot))
                    return failure("PATH_NOT_ALLOWED", "Output is outside the project folder and the additional media folder configured in MCP API settings.");
                return {};
            }
            if (type != QLatin1String("import") && type != QLatin1String("replace_media")) return {};
            QString path;
            if (type == QLatin1String("import")) {
                path = command.value("path").toString();
            } else {
                for (const auto &item : state.value("bin").toArray()) {
                    const auto clip = item.toObject();
                    if (clip.value("id") == command.value("replacementBinId")) path = clip.value("url").toString();
                }
                if (path.isEmpty()) return failure("MEDIA_NOT_READY", "Replacement media is not available.");
            }
            const QFileInfo media(path);
            const QString canonical = media.canonicalFilePath();
            if (!media.isAbsolute() || !media.isFile() || canonical.isEmpty())
                return failure("INVALID_MEDIA", "Media must be an existing absolute local file.");
            if (!within(canonical, projectRoot) && !within(canonical, extraRoot))
                return failure("PATH_NOT_ALLOWED", "Media is outside the project folder and the additional media folder configured in MCP API settings.");
            return {};
        };
        const auto command = request.value("command").toObject();
        if (command.value("type") == QLatin1String("batch")) {
            // Nested imports obey the same folder policy.
            for (const auto &item : command.value("commands").toArray())
                if (auto rejected = check(item.toObject()); !rejected.isEmpty()) return rejected;
            return {};
        }
        return check(command);
    };
    return QJsonDocument::fromJson(engine
                                       .applyAuthorized(QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact)), authorize,
                                                        {name, caller.client, caller.clientName})
                                       .toUtf8())
        .object();
}
