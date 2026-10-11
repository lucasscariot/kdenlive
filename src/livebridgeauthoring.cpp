/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "livebridge.h"

#include "bin/clipcreator.hpp"
#include "core.h"
#include "timeline2/model/timelineitemmodel.hpp"

#include <QColor>
#include <QDomDocument>
#include <QFont>
#include <QJsonArray>
#include <QRegularExpression>
#include <cmath>
#include <limits>

namespace {
QJsonObject error(const QString &code, const QString &message)
{
    return {{"ok", false}, {"error", QJsonObject{{"code", code}, {"message", message}}}};
}

bool integer(const QJsonValue &value, int minimum, int maximum = std::numeric_limits<int>::max())
{
    const double number = value.toDouble(-1);
    return value.isDouble() && std::isfinite(number) && number >= minimum && number <= maximum && std::floor(number) == number;
}

bool keys(const QJsonObject &object, const QStringList &allowed)
{
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!allowed.contains(it.key())) return false;
    return true;
}
} // namespace

QJsonObject LiveBridge::createTitle(const QJsonObject &command)
{
    const QString name = command.value(QStringLiteral("name")).toString();
    const auto items = command.value(QStringLiteral("items"));
    if (!keys(command, {QStringLiteral("type"), QStringLiteral("name"), QStringLiteral("duration"), QStringLiteral("items")}) || name.trimmed().isEmpty() ||
        name.size() > 256 || !integer(command.value(QStringLiteral("duration")), 1) || !items.isArray() || items.toArray().isEmpty() ||
        items.toArray().size() > 32)
        return error(QStringLiteral("INVALID_COMMAND"), QStringLiteral("title_create needs a name, positive duration in frames and 1 to 32 text items."));
    QDomDocument doc;
    auto root = doc.createElement(QStringLiteral("kdenlivetitle"));
    doc.appendChild(root);
    auto &profile = pCore->getProjectProfile();
    root.setAttribute(QStringLiteral("width"), profile.width());
    root.setAttribute(QStringLiteral("height"), profile.height());
    const int duration = command.value(QStringLiteral("duration")).toInt();
    root.setAttribute(QStringLiteral("out"), duration - 1);
    root.setAttribute(QStringLiteral("LC_NUMERIC"), QStringLiteral("C"));
    int index = 0;
    for (const auto &value : items.toArray()) {
        const auto item = value.toObject();
        const QString text = item.value(QStringLiteral("text")).toString();
        const QString family = item.value(QStringLiteral("fontFamily")).toString(QStringLiteral("Sans Serif"));
        const QString color = item.value(QStringLiteral("color")).toString(QStringLiteral("#ffffff"));
        const int size = item.value(QStringLiteral("fontPixelSize")).toInt(54);
        if (!value.isObject() ||
            !keys(item, {QStringLiteral("text"), QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("fontPixelSize"), QStringLiteral("fontFamily"),
                         QStringLiteral("bold"), QStringLiteral("color")}) ||
            !item.value(QStringLiteral("text")).isString() || text.size() > 4096 || family.trimmed().isEmpty() || family.size() > 256 ||
            (item.contains(QStringLiteral("fontFamily")) && !item.value(QStringLiteral("fontFamily")).isString()) ||
            (item.contains(QStringLiteral("fontPixelSize")) && !integer(item.value(QStringLiteral("fontPixelSize")), 1, 1024)) ||
            (item.contains(QStringLiteral("bold")) && !item.value(QStringLiteral("bold")).isBool()) ||
            (item.contains(QStringLiteral("color")) && !item.value(QStringLiteral("color")).isString()) ||
            !QRegularExpression(QStringLiteral("^#[0-9a-fA-F]{6}$")).match(color).hasMatch())
            return error(QStringLiteral("INVALID_COMMAND"), QStringLiteral("Invalid title text item %1; read the title_create schema.").arg(index));
        for (const auto &axis : {QStringLiteral("x"), QStringLiteral("y")}) {
            const auto coordinate = item.value(axis);
            if (!coordinate.isDouble() || !std::isfinite(coordinate.toDouble()) || std::abs(coordinate.toDouble()) > 8192)
                return error(QStringLiteral("INVALID_COMMAND"), QStringLiteral("Title item %1 needs finite x/y coordinates within ±8192 pixels.").arg(index));
        }
        auto node = doc.createElement(QStringLiteral("item"));
        node.setAttribute(QStringLiteral("type"), QStringLiteral("QGraphicsTextItem"));
        node.setAttribute(QStringLiteral("z-index"), index++);
        auto position = doc.createElement(QStringLiteral("position"));
        position.setAttribute(QStringLiteral("x"), item.value(QStringLiteral("x")).toDouble());
        position.setAttribute(QStringLiteral("y"), item.value(QStringLiteral("y")).toDouble());
        auto transform = doc.createElement(QStringLiteral("transform"));
        transform.appendChild(doc.createTextNode(QStringLiteral("1,0,0,0,1,0,0,0,1")));
        position.appendChild(transform);
        node.appendChild(position);
        auto content = doc.createElement(QStringLiteral("content"));
        content.setAttribute(QStringLiteral("font"), family);
        content.setAttribute(QStringLiteral("font-pixel-size"), size);
        content.setAttribute(QStringLiteral("font-weight"), int(item.value(QStringLiteral("bold")).toBool() ? QFont::Bold : QFont::Normal));
        const QColor fill(color);
        content.setAttribute(QStringLiteral("font-color"), QStringLiteral("%1,%2,%3,255").arg(fill.red()).arg(fill.green()).arg(fill.blue()));
        content.setAttribute(QStringLiteral("alignment"), int(Qt::AlignLeft));
        content.setAttribute(QStringLiteral("font-outline"), QStringLiteral("0"));
        content.appendChild(doc.createTextNode(text));
        node.appendChild(content);
        root.appendChild(node);
    }
    for (const auto &tag : {QStringLiteral("startviewport"), QStringLiteral("endviewport")}) {
        auto viewport = doc.createElement(tag);
        viewport.setAttribute(QStringLiteral("rect"), QStringLiteral("0,0,%1,%2").arg(profile.width()).arg(profile.height()));
        root.appendChild(viewport);
    }
    auto background = doc.createElement(QStringLiteral("background"));
    background.setAttribute(QStringLiteral("color"), QStringLiteral("0,0,0,0"));
    root.appendChild(background);
    const auto id =
        ClipCreator::createTitleClip({{QStringLiteral("xmldata"), doc.toString(-1)}}, duration, name, QStringLiteral("-1"), pCore->projectItemModel());
    if (id == QLatin1String("-1")) return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native title creation failed."));
    return {{"ok", true}, {"binId", id}, {"duration", duration}, {"width", profile.width()}, {"height", profile.height()}};
}

QJsonObject LiveBridge::validateBatch(const QJsonObject &arguments)
{
    if (!bind()) return error(QStringLiteral("NOT_READY"), QStringLiteral("No fully loaded active timeline."));
    const auto commands = arguments.value(QStringLiteral("commands"));
    if (!keys(arguments, {QStringLiteral("commands")}) || !commands.isArray() || commands.toArray().isEmpty() || commands.toArray().size() > 200)
        return error(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("Expected 1 to 200 commands."));
    QMap<int, QList<ClipSpan>> projected;
    for (int row = 0; row < m_timeline->rowCount(); ++row) {
        const int track = int(m_timeline->index(row, 0).internalId());
        projected.insert(track, trackClips(track));
    }
    QJsonArray issues;
    for (int i = 0; i < commands.toArray().size(); ++i) {
        const auto value = commands.toArray().at(i);
        const auto command = value.toObject();
        QJsonObject rejected;
        if (!value.isObject() || command.value(QStringLiteral("type")) != QLatin1String("insert") ||
            command.value(QStringLiteral("mode")).toString(QStringLiteral("normal")) != QLatin1String("normal")) {
            rejected = error(QStringLiteral("VALIDATION_UNSUPPORTED"), QStringLiteral("Preflight currently supports only normal insert commands."));
        } else {
            const auto plan = insertionPlan(command);
            if (!plan.value(QStringLiteral("ok")).toBool()) {
                rejected = plan;
            } else {
                QList<int> tracks{plan.value(QStringLiteral("trackId")).toInt()};
                if (plan.value(QStringLiteral("linked")).toBool()) tracks.append(plan.value(QStringLiteral("audioTrackId")).toInt());
                const int start = plan.value(QStringLiteral("position")).toInt();
                const int end = start + plan.value(QStringLiteral("sourceOut")).toInt() - plan.value(QStringLiteral("sourceIn")).toInt();
                for (int track : tracks) {
                    for (const auto &span : projected.value(track)) {
                        if (span.start < end && span.end > start) {
                            rejected = error(QStringLiteral("OVERLAP"), QStringLiteral("Insertion overlaps %1 on track %2.")
                                                                            .arg(span.id >= 0 ? QStringLiteral("clip %1").arg(span.id)
                                                                                              : QStringLiteral("proposed command %1").arg(-span.id - 1))
                                                                            .arg(track));
                            rejected.insert(
                                QStringLiteral("details"),
                                QJsonObject{{"trackId", track}, {"start", start}, {"end", end}, {"occupiedStart", span.start}, {"occupiedEnd", span.end}});
                            break;
                        }
                    }
                    if (!rejected.isEmpty()) break;
                }
                if (rejected.isEmpty())
                    for (int track : tracks)
                        projected[track].append({-i - 1, start, end});
            }
        }
        if (!rejected.isEmpty()) {
            rejected.remove(QStringLiteral("ok"));
            rejected.insert(QStringLiteral("commandIndex"), i);
            issues.append(rejected);
        }
    }
    return {{"ok", true},
            {"valid", issues.isEmpty()},
            {"issues", issues},
            {"sessionId", m_session},
            {"revision", m_revision},
            {"supportedCommands", QJsonArray{QStringLiteral("insert")}}};
}
