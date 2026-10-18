/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "designaudit.h"
#include "designtokens.h"

#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QToolBar>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace {

const QJsonObject &document()
{
    static const QJsonObject root = []() {
        QFile file(QStringLiteral(":/design/tokens.json"));
        return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
    }();
    return root;
}

QString firstTheme()
{
    const QStringList themes = DesignAudit::themeIds();
    return themes.isEmpty() ? QStringLiteral("dark") : themes.first();
}

// Raw color values by name and theme, aliases unresolved
const QHash<QString, QHash<QString, QString>> &rawColors()
{
    static const QHash<QString, QHash<QString, QString>> raw = []() {
        QHash<QString, QHash<QString, QString>> result;
        const QString first = firstTheme();
        for (const QJsonValue &entry : document().value(QStringLiteral("color")).toObject().value(QStringLiteral("tokens")).toArray()) {
            const QJsonObject token = entry.toObject();
            const QJsonValue value = token.value(QStringLiteral("value"));
            QHash<QString, QString> perTheme;
            if (value.isObject()) {
                const QJsonObject object = value.toObject();
                for (auto it = object.begin(); it != object.end(); ++it) {
                    perTheme.insert(it.key(), it.value().toString());
                }
            } else {
                perTheme.insert(first, value.toString());
            }
            result.insert(token.value(QStringLiteral("name")).toString(), perTheme);
        }
        return result;
    }();
    return raw;
}

QColor resolve(const QString &name, const QString &theme, int depth = 0)
{
    if (depth > 16 || !rawColors().contains(name)) {
        return QColor();
    }
    const QHash<QString, QString> &perTheme = rawColors().value(name);
    QString raw = perTheme.value(theme, perTheme.value(firstTheme()));
    if (raw.startsWith(QLatin1Char('{')) && raw.endsWith(QLatin1Char('}'))) {
        return resolve(raw.mid(1, raw.size() - 2), theme, depth + 1);
    }
    if (raw.size() == 9 && raw.startsWith(QLatin1Char('#'))) {
        raw = QLatin1Char('#') + raw.right(2) + raw.mid(1, 6);
    }
    return QColor(raw);
}

/** @brief #rrggbbaa as tokens.json writes colors, #rrggbb when opaque */
QString hex(const QColor &color)
{
    const QString rgb = color.name(QColor::HexRgb);
    return color.alpha() == 255 ? rgb : rgb + QStringLiteral("%1").arg(color.alpha(), 2, 16, QLatin1Char('0'));
}

QJsonArray lengthFamily(const QString &family)
{
    QJsonArray result;
    for (const QJsonValue &entry : document().value(family).toObject().value(QStringLiteral("tokens")).toArray()) {
        const QJsonObject token = entry.toObject();
        result.append(QJsonObject{{QStringLiteral("name"), token.value(QStringLiteral("name"))},
                                  {QStringLiteral("value"), token.value(QStringLiteral("value"))},
                                  {QStringLiteral("usage"), token.value(QStringLiteral("usage"))}});
    }
    return result;
}

int pixels(const QJsonValue &value)
{
    if (value.isDouble()) {
        return qRound(value.toDouble());
    }
    static const QRegularExpression number(QStringLiteral("^(-?[0-9.]+)"));
    const QRegularExpressionMatch match = number.match(value.toString());
    return match.hasMatch() ? qRound(match.captured(1).toDouble()) : 0;
}

QSet<int> scaleValues(const QString &family)
{
    QSet<int> values;
    for (const QJsonValue &entry : lengthFamily(family)) {
        values.insert(pixels(entry.toObject().value(QStringLiteral("value"))));
    }
    return values;
}

QString widgetPath(const QWidget *widget)
{
    QString path = QString::fromLatin1(widget->metaObject()->className());
    if (!widget->objectName().isEmpty()) {
        path += QLatin1Char('#') + widget->objectName();
    }
    return path;
}

QJsonObject fontJson(const QFont &font)
{
    return QJsonObject{{QStringLiteral("family"), font.family()},
                       {QStringLiteral("pt"), font.pointSizeF()},
                       {QStringLiteral("px"), qRound(font.pointSizeF() / 0.75)},
                       {QStringLiteral("weight"), int(font.weight())},
                       {QStringLiteral("italic"), font.italic()}};
}

QString fontKey(const QFont &font)
{
    return QStringLiteral("%1|%2|%3|%4").arg(font.family()).arg(font.pointSizeF(), 0, 'f', 2).arg(int(font.weight())).arg(font.italic() ? 1 : 0);
}

/** @brief The name of the type style @p font is, or an empty string */
QString matchingStyle(const QFont &font)
{
    static const QHash<QString, QString> styles = []() {
        QHash<QString, QString> result;
        for (const QJsonValue &entry : DesignAudit::typeStyles()) {
            const QString name = entry.toObject().value(QStringLiteral("name")).toString();
            result.insert(fontKey(DesignTokens::font(name)), name);
        }
        return result;
    }();
    return styles.value(fontKey(font));
}

QString visibleText(const QWidget *widget)
{
    if (auto *label = qobject_cast<const QLabel *>(widget)) {
        return label->text();
    }
    if (auto *button = qobject_cast<const QAbstractButton *>(widget)) {
        return button->text();
    }
    if (auto *box = qobject_cast<const QGroupBox *>(widget)) {
        return box->title();
    }
    if (auto *edit = qobject_cast<const QLineEdit *>(widget)) {
        return edit->placeholderText();
    }
    return QString();
}

QJsonObject layoutJson(const QLayout *layout)
{
    const QMargins margins = layout->contentsMargins();
    QJsonObject result{{QStringLiteral("class"), QString::fromLatin1(layout->metaObject()->className())},
                       {QStringLiteral("margins"), QJsonArray{margins.left(), margins.top(), margins.right(), margins.bottom()}}};
    if (auto *grid = qobject_cast<const QGridLayout *>(layout)) {
        result.insert(QStringLiteral("spacing"), QJsonArray{grid->horizontalSpacing(), grid->verticalSpacing()});
    } else if (auto *form = qobject_cast<const QFormLayout *>(layout)) {
        result.insert(QStringLiteral("spacing"), QJsonArray{form->horizontalSpacing(), form->verticalSpacing()});
    } else {
        result.insert(QStringLiteral("spacing"), layout->spacing());
    }
    return result;
}

bool isReviewTool(const QWidget *widget);

struct Walker
{
    bool includeHidden;
    int maxDepth;
    QJsonObject visit(QWidget *widget, int depth) const
    {
        QJsonObject node{{QStringLiteral("class"), QString::fromLatin1(widget->metaObject()->className())}};
        if (!widget->objectName().isEmpty()) {
            node.insert(QStringLiteral("name"), widget->objectName());
        }
        node.insert(QStringLiteral("visible"), widget->isVisible());
        const QRect geometry = widget->geometry();
        node.insert(QStringLiteral("geometry"), QJsonArray{geometry.x(), geometry.y(), geometry.width(), geometry.height()});
        if (widget->testAttribute(Qt::WA_SetFont)) {
            node.insert(QStringLiteral("font"), fontJson(widget->font()));
        }
        if (QLayout *layout = widget->layout()) {
            node.insert(QStringLiteral("layout"), layoutJson(layout));
        }
        const QString text = visibleText(widget);
        if (!text.isEmpty()) {
            node.insert(QStringLiteral("text"), text);
        }
        if (!widget->styleSheet().isEmpty()) {
            node.insert(QStringLiteral("styleSheet"), widget->styleSheet());
        }
        if (depth < maxDepth) {
            QJsonArray children;
            for (QObject *child : widget->children()) {
                auto *childWidget = qobject_cast<QWidget *>(child);
                if (childWidget && (includeHidden || childWidget->isVisible()) && !isReviewTool(childWidget)) {
                    children.append(visit(childWidget, depth + 1));
                }
            }
            if (!children.isEmpty()) {
                node.insert(QStringLiteral("children"), children);
            }
        }
        return node;
    }
};

/** @brief The review window reviews, it is not part of the interface under review */
bool isReviewTool(const QWidget *widget)
{
    return widget->window() && widget->window()->objectName() == QLatin1String("designReviewWindow");
}

QList<QWidget *> allWidgets(QWidget *root, bool includeHidden)
{
    QList<QWidget *> result{root};
    const QList<QWidget *> descendants = root->findChildren<QWidget *>();
    for (QWidget *widget : descendants) {
        if ((includeHidden || widget->isVisible()) && !isReviewTool(widget)) {
            result.append(widget);
        }
    }
    return result;
}

/** @brief Groups equal values and lists sample widgets, for the findings tables */
struct Tally
{
    struct Entry
    {
        int count = 0;
        QStringList samples;
        QJsonObject extra;
    };
    QMap<QString, Entry> entries;
    void add(const QString &key, const QWidget *widget, const QJsonObject &extra = {})
    {
        Entry &entry = entries[key];
        entry.count++;
        if (entry.samples.size() < 6) {
            entry.samples.append(widgetPath(widget));
        }
        if (entry.extra.isEmpty()) {
            entry.extra = extra;
        }
    }
    QJsonArray toArray(const QString &keyName) const
    {
        QList<QPair<QString, Entry>> sorted;
        for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
            sorted.append({it.key(), it.value()});
        }
        std::stable_sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second.count > b.second.count; });
        QJsonArray result;
        for (const auto &[key, entry] : sorted) {
            QJsonObject object = entry.extra;
            object.insert(keyName, key);
            object.insert(QStringLiteral("count"), entry.count);
            object.insert(QStringLiteral("samples"), QJsonArray::fromStringList(entry.samples));
            result.append(object);
        }
        return result;
    }
};

} // namespace

namespace DesignAudit {

qreal contrastRatio(QColor foreground, QColor background)
{
    const auto composite = [](const QColor &top, const QColor &under) {
        const qreal a = top.alphaF();
        return QColor::fromRgbF(top.redF() * a + under.redF() * (1 - a), top.greenF() * a + under.greenF() * (1 - a),
                                top.blueF() * a + under.blueF() * (1 - a));
    };
    const auto luminance = [](const QColor &color) {
        const auto channel = [](qreal value) { return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4); };
        return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) + 0.0722 * channel(color.blueF());
    };
    if (background.alpha() < 255) {
        background = composite(background, DesignTokens::color(QStringLiteral("surface-window")));
    }
    if (foreground.alpha() < 255) {
        foreground = composite(foreground, background);
    }
    const qreal lighter = qMax(luminance(foreground), luminance(background));
    const qreal darker = qMin(luminance(foreground), luminance(background));
    return (lighter + 0.05) / (darker + 0.05);
}

QJsonObject tokensDocument()
{
    return document();
}

QStringList themeIds()
{
    QStringList result;
    for (const QJsonValue &entry : document().value(QStringLiteral("color")).toObject().value(QStringLiteral("themes")).toArray()) {
        result.append(entry.toObject().value(QStringLiteral("id")).toString());
    }
    return result;
}

QJsonArray colorTokens()
{
    QJsonArray result;
    const QStringList themes = themeIds();
    for (const QJsonValue &entry : document().value(QStringLiteral("color")).toObject().value(QStringLiteral("tokens")).toArray()) {
        const QJsonObject token = entry.toObject();
        const QString name = token.value(QStringLiteral("name")).toString();
        QJsonObject values;
        for (const QString &theme : themes) {
            values.insert(theme, hex(resolve(name, theme)));
        }
        const QJsonValue raw = token.value(QStringLiteral("value"));
        const QString alias = raw.isString() && raw.toString().startsWith(QLatin1Char('{')) ? raw.toString().mid(1).chopped(1) : QString();
        QJsonObject object{{QStringLiteral("name"), name}, {QStringLiteral("usage"), token.value(QStringLiteral("usage"))}, {QStringLiteral("values"), values}};
        if (!alias.isEmpty()) {
            object.insert(QStringLiteral("alias"), alias);
        }
        result.append(object);
    }
    return result;
}

QJsonArray typeStyles()
{
    QJsonArray result;
    const QJsonObject type = document().value(QStringLiteral("type")).toObject();
    for (const QJsonValue &groupValue : type.value(QStringLiteral("groups")).toArray()) {
        const QJsonObject group = groupValue.toObject();
        for (const QJsonValue &styleValue : group.value(QStringLiteral("styles")).toArray()) {
            const QJsonObject style = styleValue.toObject();
            const QString name = style.value(QStringLiteral("name")).toString();
            const int px = pixels(style.value(QStringLiteral("fontSize")));
            result.append(
                QJsonObject{{QStringLiteral("name"), name},
                            {QStringLiteral("group"), group.value(QStringLiteral("name"))},
                            {QStringLiteral("family"), style.value(QStringLiteral("family")).toString(group.value(QStringLiteral("family")).toString())},
                            {QStringLiteral("resolvedFamily"), DesignTokens::font(name).family()},
                            {QStringLiteral("px"), px},
                            {QStringLiteral("pt"), px * 0.75},
                            {QStringLiteral("weight"), style.value(QStringLiteral("fontWeight")).toInt(400)},
                            {QStringLiteral("lineHeight"), pixels(style.value(QStringLiteral("lineHeight")))},
                            {QStringLiteral("letterSpacing"), style.value(QStringLiteral("letterSpacing")).toString()},
                            {QStringLiteral("sample"), style.value(QStringLiteral("sample")).toString()},
                            {QStringLiteral("usage"), style.value(QStringLiteral("usage")).toString()}});
        }
    }
    return result;
}

QJsonObject lengthTokens()
{
    QJsonObject result;
    for (const QString &family :
         {QStringLiteral("spacing"), QStringLiteral("radius"), QStringLiteral("size"), QStringLiteral("opacity"), QStringLiteral("shadow")}) {
        result.insert(family, lengthFamily(family));
    }
    return result;
}

QStringList inkTokens()
{
    return {QStringLiteral("ink"),     QStringLiteral("ink-secondary"), QStringLiteral("ink-tertiary"), QStringLiteral("accent"),
            QStringLiteral("success"), QStringLiteral("warning"),       QStringLiteral("danger"),       QStringLiteral("on-accent")};
}

QStringList surfaceTokens()
{
    return {QStringLiteral("surface-window"), QStringLiteral("surface-panel"),   QStringLiteral("surface-sidebar"),
            QStringLiteral("surface-raised"), QStringLiteral("surface-control"), QStringLiteral("surface-timeline"),
            QStringLiteral("accent-fill"),    QStringLiteral("danger-fill"),     QStringLiteral("success-fill")};
}

QJsonArray contrastMatrix(const QString &theme)
{
    QJsonArray result;
    for (const QString &ink : inkTokens()) {
        for (const QString &surface : surfaceTokens()) {
            const qreal ratio = contrastRatio(resolve(ink, theme), resolve(surface, theme));
            result.append(QJsonObject{{QStringLiteral("ink"), ink},
                                      {QStringLiteral("surface"), surface},
                                      {QStringLiteral("ratio"), std::round(ratio * 100) / 100},
                                      {QStringLiteral("aa"), ratio >= 4.5},
                                      {QStringLiteral("large"), ratio >= 3}});
        }
    }
    return result;
}

QJsonArray contrastFailures(const QString &theme)
{
    // on-accent belongs on the fills, the other inks on the surfaces: only those pairs are design claims
    QJsonArray result;
    for (const QJsonValue &entry : contrastMatrix(theme)) {
        const QJsonObject pair = entry.toObject();
        const QString ink = pair.value(QStringLiteral("ink")).toString();
        const QString surface = pair.value(QStringLiteral("surface")).toString();
        const bool fill = surface.endsWith(QLatin1String("-fill"));
        if ((ink == QLatin1String("on-accent")) != fill) {
            continue;
        }
        const bool required = ink == QLatin1String("ink-tertiary") ? pair.value(QStringLiteral("large")).toBool() : pair.value(QStringLiteral("aa")).toBool();
        if (!required) {
            result.append(pair);
        }
    }
    return result;
}

QStringList themeIcons()
{
    static const QStringList names = []() {
        QStringList result;
        const QFileInfoList files =
            QDir(QStringLiteral(":/design/icons/KdenlivePro-dark/scalable")).entryInfoList({QStringLiteral("*.svg")}, QDir::Files, QDir::Name);
        for (const QFileInfo &file : files) {
            result.append(file.completeBaseName());
        }
        return result;
    }();
    return names;
}

QJsonArray iconUsage(QWidget *root)
{
    QMap<QString, int> counts;
    if (root) {
        const QList<QAction *> actions = root->findChildren<QAction *>();
        for (const QAction *action : actions) {
            if (!action->icon().isNull() && !action->icon().name().isEmpty()) {
                counts[action->icon().name()]++;
            }
        }
        const QList<QAbstractButton *> buttons = root->findChildren<QAbstractButton *>();
        for (const QAbstractButton *button : buttons) {
            if (!button->icon().isNull() && !button->icon().name().isEmpty()) {
                counts[button->icon().name()]++;
            }
        }
    }
    const QStringList theme = themeIcons();
    QList<QPair<QString, int>> sorted;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
        sorted.append({it.key(), it.value()});
    }
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    QJsonArray result;
    for (const auto &[name, count] : sorted) {
        result.append(QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("count"), count}, {QStringLiteral("inTheme"), theme.contains(name)}});
    }
    return result;
}

QJsonObject inventory(QWidget *root, bool includeHidden, int maxDepth)
{
    if (!root) {
        return {};
    }
    return Walker{includeHidden, maxDepth}.visit(root, 0);
}

QJsonObject findings(QWidget *root, bool includeHidden)
{
    QJsonObject result;
    if (!root) {
        return result;
    }
    const QSet<int> spacing = scaleValues(QStringLiteral("spacing")) + QSet<int>{0};
    const QSet<int> sizes = scaleValues(QStringLiteral("size"));
    Tally fonts, offScale, heights, iconSizes, sheets, palettes, texts;
    static const QRegularExpression doubleSpace(QStringLiteral("  "));
    const QList<QWidget *> widgets = allWidgets(root, includeHidden);
    for (QWidget *widget : widgets) {
        // Fonts: every font set explicitly on a widget, matched against the type styles
        if (widget->testAttribute(Qt::WA_SetFont)) {
            const QFont font = widget->font();
            QJsonObject extra = fontJson(font);
            extra.insert(QStringLiteral("style"), matchingStyle(font));
            fonts.add(fontKey(font), widget, extra);
        }
        // Layout margins and spacings outside the doubling scale (the style's own -1 defaults are fine)
        if (QLayout *layout = widget->layout()) {
            const QMargins margins = layout->contentsMargins();
            QList<int> values{margins.left(), margins.top(), margins.right(), margins.bottom()};
            if (auto *grid = qobject_cast<QGridLayout *>(layout)) {
                values << grid->horizontalSpacing() << grid->verticalSpacing();
            } else if (auto *form = qobject_cast<QFormLayout *>(layout)) {
                values << form->horizontalSpacing() << form->verticalSpacing();
            } else {
                values << layout->spacing();
            }
            for (int value : values) {
                if (value > 0 && !spacing.contains(value)) {
                    offScale.add(QString::number(value), widget, {{QStringLiteral("px"), value}, {QStringLiteral("layout"), layoutJson(layout)}});
                }
            }
        }
        // Control heights pinned to a pixel value that is not a control size
        const bool control = qobject_cast<QAbstractButton *>(widget) || qobject_cast<QLineEdit *>(widget) || qobject_cast<QComboBox *>(widget) ||
                             qobject_cast<QAbstractSpinBox *>(widget);
        if (control && widget->minimumHeight() == widget->maximumHeight() && !sizes.contains(widget->minimumHeight())) {
            heights.add(QString::number(widget->minimumHeight()), widget, {{QStringLiteral("px"), widget->minimumHeight()}});
        }
        if (auto *button = qobject_cast<QAbstractButton *>(widget)) {
            const int side = button->iconSize().width();
            if (!button->icon().isNull() && !sizes.contains(side) && !qobject_cast<QToolBar *>(widget->parentWidget())) {
                iconSizes.add(QString::number(side), widget, {{QStringLiteral("px"), side}});
            }
        } else if (auto *bar = qobject_cast<QToolBar *>(widget)) {
            if (!sizes.contains(bar->iconSize().width())) {
                iconSizes.add(QString::number(bar->iconSize().width()), widget, {{QStringLiteral("px"), bar->iconSize().width()}});
            }
        }
        if (!widget->styleSheet().isEmpty()) {
            sheets.add(widget->styleSheet().simplified().left(160), widget);
        }
        if (widget->testAttribute(Qt::WA_SetPalette)) {
            palettes.add(widgetPath(widget), widget);
        }
        // Texts: mechanical slips a reader notices
        const QString text = visibleText(widget);
        if (!text.isEmpty()) {
            QStringList issues;
            if (text.contains(QLatin1String("..."))) issues << QStringLiteral("ascii ellipsis");
            if (text.contains(doubleSpace)) issues << QStringLiteral("double space");
            if (text != text.trimmed()) issues << QStringLiteral("surrounding whitespace");
            if (text.contains(QLatin1String(" :")) || text.contains(QLatin1String(" ,"))) issues << QStringLiteral("space before punctuation");
            if (!issues.isEmpty()) {
                texts.add(text, widget, {{QStringLiteral("issues"), QJsonArray::fromStringList(issues)}});
            }
        }
    }
    result.insert(QStringLiteral("fonts"), fonts.toArray(QStringLiteral("key")));
    result.insert(QStringLiteral("spacing"), offScale.toArray(QStringLiteral("value")));
    result.insert(QStringLiteral("sizes"), heights.toArray(QStringLiteral("value")));
    result.insert(QStringLiteral("iconSizes"), iconSizes.toArray(QStringLiteral("value")));
    result.insert(QStringLiteral("styleSheets"), sheets.toArray(QStringLiteral("styleSheet")));
    result.insert(QStringLiteral("palettes"), palettes.toArray(QStringLiteral("widget")));
    result.insert(QStringLiteral("texts"), texts.toArray(QStringLiteral("text")));
    QJsonArray missingIcons;
    for (const QJsonValue &entry : iconUsage(root)) {
        if (!entry.toObject().value(QStringLiteral("inTheme")).toBool()) {
            missingIcons.append(entry);
        }
    }
    result.insert(QStringLiteral("icons"), missingIcons);
    result.insert(QStringLiteral("widgets"), widgets.size());
    return result;
}

QStringList reportSections()
{
    return {QStringLiteral("tokens"), QStringLiteral("colors"),   QStringLiteral("contrast"), QStringLiteral("type"),
            QStringLiteral("icons"),  QStringLiteral("findings"), QStringLiteral("inventory")};
}

QJsonObject report(QWidget *root, const QStringList &sections)
{
    const QStringList wanted = sections.isEmpty() ? QStringList{QStringLiteral("tokens"), QStringLiteral("colors"), QStringLiteral("contrast"),
                                                                QStringLiteral("type"),   QStringLiteral("icons"),  QStringLiteral("findings")}
                                                  : sections;
    QJsonObject result{{QStringLiteral("theme"), DesignTokens::instance()->isDark() ? QStringLiteral("dark") : QStringLiteral("light")},
                       {QStringLiteral("themes"), QJsonArray::fromStringList(themeIds())}};
    if (wanted.contains(QLatin1String("tokens"))) {
        result.insert(QStringLiteral("tokens"), lengthTokens());
    }
    if (wanted.contains(QLatin1String("colors"))) {
        result.insert(QStringLiteral("colors"), colorTokens());
    }
    if (wanted.contains(QLatin1String("contrast"))) {
        QJsonObject contrast, failures;
        for (const QString &theme : themeIds()) {
            contrast.insert(theme, contrastMatrix(theme));
            failures.insert(theme, contrastFailures(theme));
        }
        result.insert(QStringLiteral("contrast"), contrast);
        result.insert(QStringLiteral("contrastFailures"), failures);
    }
    if (wanted.contains(QLatin1String("type"))) {
        result.insert(QStringLiteral("type"), typeStyles());
    }
    if (wanted.contains(QLatin1String("icons"))) {
        result.insert(QStringLiteral("themeIcons"), QJsonArray::fromStringList(themeIcons()));
        result.insert(QStringLiteral("iconUsage"), iconUsage(root));
    }
    if (wanted.contains(QLatin1String("findings"))) {
        result.insert(QStringLiteral("findings"), findings(root));
    }
    if (wanted.contains(QLatin1String("inventory"))) {
        result.insert(QStringLiteral("inventory"), inventory(root));
    }
    return result;
}

} // namespace DesignAudit
