/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "mcptools.h"
#include "livebridge.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUrl>

namespace {
QJsonObject objectSchema(const QJsonObject &properties)
{
    QJsonArray required;
    for (auto it = properties.begin(); it != properties.end(); ++it)
        required.append(it.key());
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

const QMap<QString, QJsonObject> &commands()
{
    static const auto result = [] {
        const auto text = stringSchema();
        const auto frame = frameSchema();
        const QJsonObject bin{{"type", "string"}, {"pattern", "^[0-9]+$"}};
        const QJsonObject edge{{"type", "string"}, {"enum", QJsonArray{"left", "right"}}};
        return QMap<QString, QJsonObject>{
            {"import", {{"path", text}}},
            {"remove_asset", {{"binId", bin}}},
            {"remove_clip", {{"clipId", frame}}},
            {"replace_media", {{"binId", bin}, {"replacementBinId", bin}}},
            {"insert",
             {{"binId", bin},
              {"trackId", frame},
              {"position", frame},
              {"sourceIn", frame},
              {"sourceOut", frameSchema(1)},
              {"media", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"video", "audio"}}}}}},
            {"move", {{"clipId", frame}, {"trackId", frame}, {"position", frame}}},
            {"trim", {{"clipId", frame}, {"duration", frameSchema(1)}, {"edge", edge}}},
            {"audio_envelope",
             {{"clipId", frame}, {"fadeIn", frame}, {"fadeOut", frame}, {"gainDb", QJsonObject{{"type", "number"}, {"minimum", -60}, {"maximum", 0}}}}},
            {"rename_track", {{"trackId", frame}, {"name", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}}}},
            {"save", {}},
            {"undo", {}},
            {"redo", {}}};
    }();
    return result;
}

QJsonObject editFields()
{
    return {{"sessionId", QJsonObject{{"type", "string"}, {"format", "uuid"}}},
            {"expectedRevision", QJsonObject{{"type", "integer"}, {"minimum", 0}}},
            {"requestId", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}}};
}

struct Tool
{
    QString name;
    QString description;
    QString command;
};

const QList<Tool> &editingTools()
{
    static const QList<Tool> result{
        {"desktop_media_import", "Import a local file into the visible project bin. Returns binId; poll desktop_state until ready. Reuses existing paths.",
         "import"},
        {"desktop_media_remove", "Remove an unused binId using native Undo. Rejects media used in any sequence. Keeps the source file on disk.",
         "remove_asset"},
        {"desktop_clip_insert", "Insert ready bin media into a track. Frames use project FPS; sourceOut is exclusive. Choose audio or video explicitly.",
         "insert"},
        {"desktop_clip_remove", "Remove one ungrouped clip without rippling. Keeps its bin asset and source file. Native Undo restores it.", "remove_clip"},
        {"desktop_media_replace",
         "Replace every use of binId with replacementBinId. Both must be ready with matching streams; replacement must cover the original "
         "duration.",
         "replace_media"},
        {"desktop_clip_move", "Move a native clip to a track and frame position, respecting locks, groups and collisions.", "move"},
        {"desktop_clip_trim", "Trim a clip edge to duration frames. Read actualDuration in the result.", "trim"},
        {"desktop_audio_envelope",
         "Add native volume automation to an audio clip. fadeIn/fadeOut use project frames; gainDb is -60..0. Rejects existing volume effects and "
         "overlapping fades.",
         "audio_envelope"},
        {"desktop_track_rename", "Rename a native timeline track using Undo.", "rename_track"},
        {"desktop_project_save", "Save the open project to its existing local file. All bin media must be ready.", "save"},
        {"desktop_undo", "Undo the latest action in shared Kdenlive history, including manual edits.", "undo"},
        {"desktop_redo", "Redo the latest action in shared Kdenlive history.", "redo"}};
    return result;
}

QJsonObject definition(const QString &name, const QString &description, const QJsonObject &properties, bool readOnly)
{
    return {{"name", name},
            {"description", description},
            {"inputSchema", objectSchema(properties)},
            {"annotations", QJsonObject{{"readOnlyHint", readOnly}, {"destructiveHint", !readOnly}, {"idempotentHint", true}, {"openWorldHint", false}}}};
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
    result.append(definition("desktop_capabilities", "Read native editing capabilities of this Kdenlive instance.", {}, true));
    result.append(definition(
        "desktop_state", "Read the actual visible sequence, bin readiness and usage, native IDs, effect counts, sessionId, revision and shared Undo history.",
        {}, true));
    auto fields = editFields();
    QJsonArray variants;
    for (auto it = commands().begin(); it != commands().end(); ++it) {
        auto properties = it.value();
        properties.insert("type", QJsonObject{{"const", it.key()}, {"type", "string"}});
        variants.append(objectSchema(properties));
    }
    fields.insert("command", QJsonObject{{"oneOf", variants}});
    result.append(definition("desktop_apply",
                             "Apply one native operation with sessionId/revision from desktop_state. Retry uncertain outcomes only with the identical "
                             "requestId and payload. Import/replacement load asynchronously. All edits use native Undo; saving does not add history.",
                             {{"request", objectSchema(fields)}}, false));
    for (const auto &tool : editingTools()) {
        auto properties = editFields();
        const auto arguments = commands().value(tool.command);
        for (auto it = arguments.begin(); it != arguments.end(); ++it)
            properties.insert(it.key(), it.value());
        result.append(
            definition(tool.name, tool.description + QStringLiteral(" Use sessionId/revision from desktop_state and a unique requestId."), properties, false));
    }
    return result;
}

QJsonObject McpTools::call(LiveBridge &engine, const QString &name, const QJsonObject &arguments, const QString &additionalMediaRoot)
{
    if (name == QLatin1String("desktop_capabilities") || name == QLatin1String("desktop_state")) {
        if (!arguments.isEmpty()) return failure("INVALID_ARGUMENTS", "This tool takes no arguments.");
        return QJsonDocument::fromJson((name == QLatin1String("desktop_state") ? engine.state() : engine.capabilities()).toUtf8()).object();
    }
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
        const auto command = request.value("command").toObject();
        QString path;
        if (command.value("type") == QLatin1String("import")) {
            path = command.value("path").toString();
        } else if (command.value("type") == QLatin1String("replace_media")) {
            for (const auto &item : state.value("bin").toArray()) {
                const auto clip = item.toObject();
                if (clip.value("id") == command.value("replacementBinId")) path = clip.value("url").toString();
            }
            if (path.isEmpty()) return failure("MEDIA_NOT_READY", "Replacement media is not available.");
        }
        if (command.value("type") == QLatin1String("import") || command.value("type") == QLatin1String("replace_media")) {
            const QFileInfo media(path);
            const QString canonical = media.canonicalFilePath();
            const QString projectRoot = QFileInfo(project.canonicalFilePath()).absolutePath();
            const QString extraRoot = additionalMediaRoot.isEmpty() ? QString{} : QFileInfo(additionalMediaRoot).canonicalFilePath();
            if (!media.isAbsolute() || !media.isFile() || canonical.isEmpty())
                return failure("INVALID_MEDIA", "Media must be an existing absolute local file.");
            if (!within(canonical, projectRoot) && !within(canonical, extraRoot))
                return failure("PATH_NOT_ALLOWED", "Media is outside the project folder and the additional media folder configured in MCP API settings.");
        }
        return {};
    };
    return QJsonDocument::fromJson(engine.applyAuthorized(QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact)), authorize).toUtf8())
        .object();
}
