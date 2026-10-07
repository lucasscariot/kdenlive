/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "designtokens.h"

#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QRegularExpression>

namespace {
// Registered family names of the bundled fonts, by tokens.json family key
QHash<QString, QString> &bundledFamilies()
{
    static QHash<QString, QString> families;
    return families;
}

int toPixels(const QJsonValue &value)
{
    if (value.isDouble()) {
        return qRound(value.toDouble());
    }
    static const QRegularExpression number(QStringLiteral("^(-?[0-9.]+)"));
    const QRegularExpressionMatch match = number.match(value.toString());
    return match.hasMatch() ? qRound(match.captured(1).toDouble()) : 0;
}
} // namespace

DesignTokens *DesignTokens::instance()
{
    static DesignTokens *tokens = new DesignTokens;
    return tokens;
}

DesignTokens *DesignTokens::create(QQmlEngine *, QJSEngine *)
{
    QQmlEngine::setObjectOwnership(instance(), QQmlEngine::CppOwnership);
    return instance();
}

DesignTokens::DesignTokens()
    : QObject(qApp)
{
    loadFonts();
    load();
    updateTheme();
    if (qApp) {
        qApp->installEventFilter(this);
    }
}

void DesignTokens::loadFonts()
{
    if (!bundledFamilies().isEmpty()) {
        return;
    }
    const auto add = [](const QString &key, const QString &file) {
        const int id = QFontDatabase::addApplicationFont(file);
        const QStringList families = id >= 0 ? QFontDatabase::applicationFontFamilies(id) : QStringList();
        if (!families.isEmpty()) {
            bundledFamilies().insert(key, families.first());
        }
    };
    add(QStringLiteral("sans"), QStringLiteral(":/design/fonts/InterVariable.ttf"));
    add(QStringLiteral("mono"), QStringLiteral(":/design/fonts/JetBrainsMono-Variable.ttf"));
    if (bundledFamilies().contains(QStringLiteral("sans"))) {
        bundledFamilies().insert(QStringLiteral("display"), bundledFamilies().value(QStringLiteral("sans")));
    }
}

void DesignTokens::load()
{
    QFile file(QStringLiteral(":/design/tokens.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Design tokens missing";
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();

    // Colors, per theme; a plain string is the first theme's value
    const QJsonObject color = root.value(QStringLiteral("color")).toObject();
    const QJsonArray themes = color.value(QStringLiteral("themes")).toArray();
    m_firstTheme = themes.isEmpty() ? QStringLiteral("dark") : themes.first().toObject().value(QStringLiteral("id")).toString();
    for (const QJsonValue &entry : color.value(QStringLiteral("tokens")).toArray()) {
        const QJsonObject token = entry.toObject();
        const QJsonValue value = token.value(QStringLiteral("value"));
        QHash<QString, QString> perTheme;
        if (value.isObject()) {
            const QJsonObject obj = value.toObject();
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                perTheme.insert(it.key(), it.value().toString());
            }
        } else {
            perTheme.insert(m_firstTheme, value.toString());
        }
        m_rawColors.insert(token.value(QStringLiteral("name")).toString(), perTheme);
    }

    // Lengths and plain numbers from every other family
    for (const QString &family : {QStringLiteral("spacing"), QStringLiteral("radius"), QStringLiteral("size")}) {
        for (const QJsonValue &entry : root.value(family).toObject().value(QStringLiteral("tokens")).toArray()) {
            const QJsonObject token = entry.toObject();
            m_lengths.insert(token.value(QStringLiteral("name")).toString(), toPixels(token.value(QStringLiteral("value"))));
        }
    }
    for (const QJsonValue &entry : root.value(QStringLiteral("opacity")).toObject().value(QStringLiteral("tokens")).toArray()) {
        const QJsonObject token = entry.toObject();
        m_numbers.insert(token.value(QStringLiteral("name")).toString(), token.value(QStringLiteral("value")).toString().toDouble());
    }

    // Type styles: bundled family when available, else the first family of the stack
    const QJsonObject type = root.value(QStringLiteral("type")).toObject();
    const QJsonObject families = type.value(QStringLiteral("families")).toObject();
    for (auto it = families.begin(); it != families.end(); ++it) {
        QString family = bundledFamilies().value(it.key());
        if (family.isEmpty()) {
            family = it.value().toString().section(QLatin1Char(','), 0, 0).remove(QLatin1Char('"')).trimmed();
        }
        m_families.insert(it.key(), family);
    }
    for (const QJsonValue &groupValue : type.value(QStringLiteral("groups")).toArray()) {
        const QJsonObject group = groupValue.toObject();
        const QString groupFamily = group.value(QStringLiteral("family")).toString();
        for (const QJsonValue &styleValue : group.value(QStringLiteral("styles")).toArray()) {
            const QJsonObject style = styleValue.toObject();
            const QString familyKey = style.value(QStringLiteral("family")).toString(groupFamily);
            QFont font(m_families.value(familyKey));
            const int pixelSize = toPixels(style.value(QStringLiteral("fontSize")));
            // Points rather than pixels: much of the app scales fonts with pointSize() arithmetic,
            // which breaks on pixel sized fonts. 1px is 0.75pt at Qt's 96 dpi logical resolution.
            font.setPointSizeF(pixelSize * 0.75);
            font.setWeight(QFont::Weight(style.value(QStringLiteral("fontWeight")).toInt(400)));
            const QString spacing = style.value(QStringLiteral("letterSpacing")).toString();
            if (spacing.endsWith(QLatin1String("em"))) {
                font.setLetterSpacing(QFont::AbsoluteSpacing, spacing.chopped(2).toDouble() * pixelSize);
            }
            if (familyKey == QLatin1String("mono")) {
                font.setStyleHint(QFont::Monospace);
                font.setFixedPitch(true);
            }
            m_fonts.insert(style.value(QStringLiteral("name")).toString(), font);
        }
    }
}

QColor DesignTokens::resolveColor(const QString &name, const QString &theme, int depth) const
{
    if (depth > 16 || !m_rawColors.contains(name)) {
        return QColor();
    }
    const QHash<QString, QString> &perTheme = m_rawColors.value(name);
    QString raw = perTheme.value(theme, perTheme.value(m_firstTheme));
    if (raw.startsWith(QLatin1Char('{')) && raw.endsWith(QLatin1Char('}'))) {
        return resolveColor(raw.mid(1, raw.size() - 2), theme, depth + 1);
    }
    // tokens.json writes alpha last (#rrggbbaa), Qt reads it first (#aarrggbb)
    if (raw.size() == 9 && raw.startsWith(QLatin1Char('#'))) {
        raw = QLatin1Char('#') + raw.right(2) + raw.mid(1, 6);
    }
    return QColor(raw);
}

void DesignTokens::ensureCurrentTheme()
{
    // Widgets may repaint for a palette change before our event filter sees it
    if (QGuiApplication::palette().cacheKey() != m_paletteKey) {
        updateTheme();
    }
}

void DesignTokens::updateTheme()
{
    const QPalette palette = QGuiApplication::palette();
    m_paletteKey = palette.cacheKey();
    m_dark = palette.color(QPalette::Window).lightness() < palette.color(QPalette::WindowText).lightness();
    const QString theme = m_dark ? QStringLiteral("dark") : QStringLiteral("light");
    m_colors.clear();
    for (auto it = m_rawColors.constBegin(); it != m_rawColors.constEnd(); ++it) {
        m_colors.insert(it.key(), resolveColor(it.key(), theme));
    }
    Q_EMIT themeChanged();
}

bool DesignTokens::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == qApp && event->type() == QEvent::ApplicationPaletteChange) {
        ensureCurrentTheme();
    }
    return QObject::eventFilter(watched, event);
}

QColor DesignTokens::color(const QString &name)
{
    instance()->ensureCurrentTheme();
    const QColor value = instance()->m_colors.value(name);
    Q_ASSERT_X(value.isValid(), "DesignTokens::color", qPrintable(name));
    return value;
}

int DesignTokens::space(int step)
{
    return instance()->m_lengths.value(QStringLiteral("space-%1").arg(step));
}

int DesignTokens::radius(const QString &name)
{
    return instance()->m_lengths.value(name);
}

int DesignTokens::size(const QString &name)
{
    return instance()->m_lengths.value(name);
}

qreal DesignTokens::opacity(const QString &name)
{
    return instance()->m_numbers.value(name, 1.0);
}

QFont DesignTokens::font(const QString &style)
{
    return instance()->m_fonts.value(style, QGuiApplication::font());
}

QColor DesignTokens::alpha(const QString &name, qreal factor) const
{
    QColor value = color(name);
    value.setAlphaF(value.alphaF() * factor);
    return value;
}

bool DesignTokens::isDark() const
{
    return m_dark;
}

QVariantMap DesignTokens::colorMap() const
{
    QVariantMap map;
    for (auto it = m_colors.constBegin(); it != m_colors.constEnd(); ++it) {
        map.insert(it.key(), it.value());
    }
    return map;
}

QVariantMap DesignTokens::fontMap() const
{
    QVariantMap map;
    for (auto it = m_fonts.constBegin(); it != m_fonts.constEnd(); ++it) {
        map.insert(it.key(), it.value());
    }
    return map;
}
