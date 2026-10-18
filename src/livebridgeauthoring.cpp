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

namespace {
QString rgba(const QString &hex, double opacity)
{
    const QColor color(hex);
    return QStringLiteral("%1,%2,%3,%4").arg(color.red()).arg(color.green()).arg(color.blue()).arg(qRound(qBound(0.0, opacity, 1.0) * 255));
}

bool hexColor(const QJsonObject &object, const QString &key)
{
    return !object.contains(key) ||
           (object.value(key).isString() && QRegularExpression(QStringLiteral("^#[0-9a-fA-F]{6}$")).match(object.value(key).toString()).hasMatch());
}

bool number(const QJsonObject &object, const QString &key, double minimum, double maximum)
{
    if (!object.contains(key)) return true;
    const auto value = object.value(key);
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= minimum && value.toDouble() <= maximum;
}
} // namespace

QJsonObject LiveBridge::createTitle(const QJsonObject &command)
{
    const QString name = command.value(QStringLiteral("name")).toString();
    const auto items = command.value(QStringLiteral("items"));
    const auto background = command.value(QStringLiteral("background")).toObject();
    if (!keys(command, {QStringLiteral("type"), QStringLiteral("name"), QStringLiteral("duration"), QStringLiteral("items"), QStringLiteral("background")}) ||
        name.trimmed().isEmpty() || name.size() > 256 || !integer(command.value(QStringLiteral("duration")), 1) || !items.isArray() ||
        items.toArray().isEmpty() || items.toArray().size() > 32 ||
        (command.contains(QStringLiteral("background")) &&
         (!command.value(QStringLiteral("background")).isObject() || !keys(background, {QStringLiteral("color"), QStringLiteral("opacity")}) ||
          !hexColor(background, QStringLiteral("color")) || !number(background, QStringLiteral("opacity"), 0, 1))))
        return error(QStringLiteral("INVALID_COMMAND"),
                     QStringLiteral("title_create needs a name, positive duration in frames, 1 to 32 items and an optional background {color, opacity}."));
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
    const auto rejectItem = [&index](const QString &reason) {
        return error(QStringLiteral("INVALID_COMMAND"), QStringLiteral("Title item %1: %2").arg(index).arg(reason));
    };
    for (const auto &value : items.toArray()) {
        const auto item = value.toObject();
        if (!value.isObject()) return rejectItem(QStringLiteral("must be an object."));
        for (const auto &axis : {QStringLiteral("x"), QStringLiteral("y")}) {
            const auto coordinate = item.value(axis);
            if (!coordinate.isDouble() || !std::isfinite(coordinate.toDouble()) || std::abs(coordinate.toDouble()) > 8192)
                return rejectItem(QStringLiteral("needs finite x/y coordinates within ±8192 pixels."));
        }
        const QString kind = item.value(QStringLiteral("kind")).toString(QStringLiteral("text"));
        auto node = doc.createElement(QStringLiteral("item"));
        node.setAttribute(QStringLiteral("z-index"), index);
        auto position = doc.createElement(QStringLiteral("position"));
        position.setAttribute(QStringLiteral("x"), item.value(QStringLiteral("x")).toDouble());
        position.setAttribute(QStringLiteral("y"), item.value(QStringLiteral("y")).toDouble());
        auto transform = doc.createElement(QStringLiteral("transform"));
        transform.appendChild(doc.createTextNode(QStringLiteral("1,0,0,0,1,0,0,0,1")));
        position.appendChild(transform);
        node.appendChild(position);
        auto content = doc.createElement(QStringLiteral("content"));
        if (kind == QLatin1String("rect")) {
            // A backing box behind text, as the title editor's rectangle item.
            if (!keys(item, {QStringLiteral("kind"), QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("width"), QStringLiteral("height"),
                             QStringLiteral("color"), QStringLiteral("opacity"), QStringLiteral("borderColor"), QStringLiteral("borderWidth")}) ||
                !item.contains(QStringLiteral("width")) || !item.contains(QStringLiteral("height")) || !number(item, QStringLiteral("width"), 1, 16384) ||
                !number(item, QStringLiteral("height"), 1, 16384) || !hexColor(item, QStringLiteral("color")) ||
                !hexColor(item, QStringLiteral("borderColor")) || !number(item, QStringLiteral("opacity"), 0, 1) ||
                !number(item, QStringLiteral("borderWidth"), 0, 200))
                return rejectItem(QStringLiteral("a rect needs width, height and optional color, opacity, borderColor, borderWidth."));
            const QString color = item.value(QStringLiteral("color")).toString(QStringLiteral("#000000"));
            const double opacity = item.value(QStringLiteral("opacity")).toDouble(1);
            const double borderWidth = item.value(QStringLiteral("borderWidth")).toDouble(0);
            node.setAttribute(QStringLiteral("type"), QStringLiteral("QGraphicsRectItem"));
            content.setAttribute(
                QStringLiteral("rect"),
                QStringLiteral("0,0,%1,%2").arg(item.value(QStringLiteral("width")).toDouble()).arg(item.value(QStringLiteral("height")).toDouble()));
            content.setAttribute(QStringLiteral("brushcolor"), rgba(color, opacity));
            content.setAttribute(QStringLiteral("pencolor"),
                                 borderWidth > 0 ? rgba(item.value(QStringLiteral("borderColor")).toString(color), opacity) : QStringLiteral("0,0,0,0"));
            content.setAttribute(QStringLiteral("penwidth"), QString::number(borderWidth));
        } else if (kind == QLatin1String("text")) {
            const QString text = item.value(QStringLiteral("text")).toString();
            const QString family = item.value(QStringLiteral("fontFamily")).toString(QStringLiteral("Sans Serif"));
            const QString alignment = item.value(QStringLiteral("alignment")).toString(QStringLiteral("left"));
            const auto outline = item.value(QStringLiteral("outline")).toObject();
            const auto shadow = item.value(QStringLiteral("shadow")).toObject();
            if (!keys(item, {QStringLiteral("kind"), QStringLiteral("text"), QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("fontPixelSize"),
                             QStringLiteral("fontFamily"), QStringLiteral("bold"), QStringLiteral("italic"), QStringLiteral("color"), QStringLiteral("opacity"),
                             QStringLiteral("alignment"), QStringLiteral("boxWidth"), QStringLiteral("outline"), QStringLiteral("shadow"),
                             QStringLiteral("letterSpacing")}) ||
                !item.value(QStringLiteral("text")).isString() || text.size() > 4096 || family.trimmed().isEmpty() || family.size() > 256 ||
                (item.contains(QStringLiteral("fontFamily")) && !item.value(QStringLiteral("fontFamily")).isString()) ||
                (item.contains(QStringLiteral("fontPixelSize")) && !integer(item.value(QStringLiteral("fontPixelSize")), 1, 1024)) ||
                (item.contains(QStringLiteral("bold")) && !item.value(QStringLiteral("bold")).isBool()) ||
                (item.contains(QStringLiteral("italic")) && !item.value(QStringLiteral("italic")).isBool()) || !hexColor(item, QStringLiteral("color")) ||
                !number(item, QStringLiteral("opacity"), 0, 1) || !number(item, QStringLiteral("boxWidth"), 1, 16384) ||
                !number(item, QStringLiteral("letterSpacing"), -100, 100) ||
                !QStringList{QStringLiteral("left"), QStringLiteral("center"), QStringLiteral("right")}.contains(alignment) ||
                (item.contains(QStringLiteral("outline")) && (!item.value(QStringLiteral("outline")).isObject() ||
                                                              !keys(outline, {QStringLiteral("width"), QStringLiteral("color"), QStringLiteral("opacity")}) ||
                                                              !number(outline, QStringLiteral("width"), 0, 50) || !hexColor(outline, QStringLiteral("color")) ||
                                                              !number(outline, QStringLiteral("opacity"), 0, 1))) ||
                (item.contains(QStringLiteral("shadow")) &&
                 (!item.value(QStringLiteral("shadow")).isObject() ||
                  !keys(shadow,
                        {QStringLiteral("color"), QStringLiteral("opacity"), QStringLiteral("blur"), QStringLiteral("xOffset"), QStringLiteral("yOffset")}) ||
                  !hexColor(shadow, QStringLiteral("color")) || !number(shadow, QStringLiteral("opacity"), 0, 1) ||
                  !number(shadow, QStringLiteral("blur"), 0, 100) || !number(shadow, QStringLiteral("xOffset"), -200, 200) ||
                  !number(shadow, QStringLiteral("yOffset"), -200, 200))))
                return rejectItem(QStringLiteral("invalid text fields; read the title_create schema."));
            node.setAttribute(QStringLiteral("type"), QStringLiteral("QGraphicsTextItem"));
            content.setAttribute(QStringLiteral("font"), family);
            content.setAttribute(QStringLiteral("font-pixel-size"), item.value(QStringLiteral("fontPixelSize")).toInt(54));
            content.setAttribute(QStringLiteral("font-weight"), int(item.value(QStringLiteral("bold")).toBool() ? QFont::Bold : QFont::Normal));
            content.setAttribute(QStringLiteral("font-italic"), int(item.value(QStringLiteral("italic")).toBool()));
            content.setAttribute(QStringLiteral("font-color"), rgba(item.value(QStringLiteral("color")).toString(QStringLiteral("#ffffff")),
                                                                    item.value(QStringLiteral("opacity")).toDouble(1)));
            content.setAttribute(QStringLiteral("alignment"), int(alignment == QLatin1String("center")  ? Qt::AlignHCenter
                                                                  : alignment == QLatin1String("right") ? Qt::AlignRight
                                                                                                        : Qt::AlignLeft));
            // Centered and right-aligned text needs a box to align inside; default to the rest of the frame.
            if (item.contains(QStringLiteral("boxWidth")))
                content.setAttribute(QStringLiteral("box-width"), QString::number(item.value(QStringLiteral("boxWidth")).toDouble()));
            else if (alignment != QLatin1String("left"))
                content.setAttribute(QStringLiteral("box-width"), QString::number(qMax(1.0, profile.width() - item.value(QStringLiteral("x")).toDouble())));
            if (item.contains(QStringLiteral("letterSpacing")))
                content.setAttribute(QStringLiteral("letter-spacing"), QString::number(item.value(QStringLiteral("letterSpacing")).toDouble()));
            if (item.contains(QStringLiteral("outline"))) {
                content.setAttribute(QStringLiteral("font-outline"), QString::number(outline.value(QStringLiteral("width")).toDouble(1)));
                content.setAttribute(QStringLiteral("font-outline-color"), rgba(outline.value(QStringLiteral("color")).toString(QStringLiteral("#000000")),
                                                                                outline.value(QStringLiteral("opacity")).toDouble(1)));
            } else {
                content.setAttribute(QStringLiteral("font-outline"), QStringLiteral("0"));
            }
            if (item.contains(QStringLiteral("shadow"))) {
                QColor shadowColor(shadow.value(QStringLiteral("color")).toString(QStringLiteral("#000000")));
                shadowColor.setAlphaF(shadow.value(QStringLiteral("opacity")).toDouble(0.6));
                // The title editor's format: enabled;#AARRGGBB;blur;xOffset;yOffset.
                content.setAttribute(QStringLiteral("shadow"), QStringLiteral("1;%1;%2;%3;%4")
                                                                   .arg(shadowColor.name(QColor::HexArgb))
                                                                   .arg(shadow.value(QStringLiteral("blur")).toDouble(4))
                                                                   .arg(shadow.value(QStringLiteral("xOffset")).toDouble(2))
                                                                   .arg(shadow.value(QStringLiteral("yOffset")).toDouble(2)));
            }
            content.appendChild(doc.createTextNode(text));
        } else {
            return rejectItem(QStringLiteral("kind must be text or rect."));
        }
        node.appendChild(content);
        root.appendChild(node);
        ++index;
    }
    for (const auto &tag : {QStringLiteral("startviewport"), QStringLiteral("endviewport")}) {
        auto viewport = doc.createElement(tag);
        viewport.setAttribute(QStringLiteral("rect"), QStringLiteral("0,0,%1,%2").arg(profile.width()).arg(profile.height()));
        root.appendChild(viewport);
    }
    auto backgroundNode = doc.createElement(QStringLiteral("background"));
    backgroundNode.setAttribute(QStringLiteral("color"), command.contains(QStringLiteral("background"))
                                                             ? rgba(background.value(QStringLiteral("color")).toString(QStringLiteral("#000000")),
                                                                    background.value(QStringLiteral("opacity")).toDouble(1))
                                                             : QStringLiteral("0,0,0,0"));
    root.appendChild(backgroundNode);
    const auto id =
        ClipCreator::createTitleClip({{QStringLiteral("xmldata"), doc.toString(-1)}}, duration, name, QStringLiteral("-1"), pCore->projectItemModel());
    if (id == QLatin1String("-1")) return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native title creation failed."));
    return {{"ok", true}, {"binId", id}, {"duration", duration}, {"width", profile.width()}, {"height", profile.height()}, {"items", index}};
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
        QJsonObject command = value.toObject();
        QJsonObject rejected;
        if (!value.isObject() || command.value(QStringLiteral("type")) != QLatin1String("insert") ||
            command.value(QStringLiteral("mode")).toString(QStringLiteral("normal")) != QLatin1String("normal")) {
            rejected = error(QStringLiteral("VALIDATION_UNSUPPORTED"), QStringLiteral("Preflight currently supports only normal insert commands."));
        } else if (auto unresolved = resolveTrackReferences(command); !unresolved.isEmpty()) {
            rejected = unresolved;
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
