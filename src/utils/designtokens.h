/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <QColor>
#include <QFont>
#include <QHash>
#include <QObject>
#include <QQmlEngine>
#include <QVariantMap>
#include <QtQmlIntegration>

/** @class DesignTokens
    @brief The Kdenlive Pro design tokens, loaded once from src/design/tokens.json.

    This is the single source for colors, spacing, radii, sizes and type styles, shared by the
    widget style, custom widgets and QML (as the K.Design singleton), so the interface cannot
    drift from the design system. Colors follow the active palette: dark tokens on a dark
    palette, light tokens otherwise.
 */
class DesignTokens : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(Design)
    QML_SINGLETON

    Q_PROPERTY(bool dark READ isDark NOTIFY themeChanged)
    /** Color tokens of the active theme, by token name (for example colors["role-video"]) */
    Q_PROPERTY(QVariantMap colors READ colorMap NOTIFY themeChanged)
    /** Type styles by name, as fonts (for example fonts["text-caption"]) */
    Q_PROPERTY(QVariantMap fonts READ fontMap CONSTANT)

    Q_PROPERTY(int space1 READ space1 CONSTANT)
    Q_PROPERTY(int space2 READ space2 CONSTANT)
    Q_PROPERTY(int space3 READ space3 CONSTANT)
    Q_PROPERTY(int space4 READ space4 CONSTANT)
    Q_PROPERTY(int space5 READ space5 CONSTANT)
    Q_PROPERTY(int space6 READ space6 CONSTANT)

    Q_PROPERTY(int radiusXs READ radiusXs CONSTANT)
    Q_PROPERTY(int radiusSm READ radiusSm CONSTANT)
    Q_PROPERTY(int radiusMd READ radiusMd CONSTANT)
    Q_PROPERTY(int radiusLg READ radiusLg CONSTANT)
    Q_PROPERTY(int radiusXl READ radiusXl CONSTANT)

    Q_PROPERTY(int controlSm READ controlSm CONSTANT)
    Q_PROPERTY(int controlMd READ controlMd CONSTANT)
    Q_PROPERTY(int controlLg READ controlLg CONSTANT)
    Q_PROPERTY(int iconSm READ iconSm CONSTANT)
    Q_PROPERTY(int iconMd READ iconMd CONSTANT)
    Q_PROPERTY(int iconLg READ iconLg CONSTANT)
    Q_PROPERTY(int laneVideo READ laneVideo CONSTANT)
    Q_PROPERTY(int laneAudio READ laneAudio CONSTANT)

    Q_PROPERTY(qreal opacityDisabled READ opacityDisabled CONSTANT)
    Q_PROPERTY(qreal opacityWaveform READ opacityWaveform CONSTANT)

public:
    static DesignTokens *instance();
    static DesignTokens *create(QQmlEngine *, QJSEngine *);

    /** @brief Register the bundled fonts; call once before the first font() */
    static void loadFonts();

    // C++ accessors, by token name as written in tokens.json
    static QColor color(const QString &name);
    /** @brief A spacing step, 1 (2px) to 7 (128px): every step doubles */
    static int space(int step);
    static int radius(const QString &name);
    static int size(const QString &name);
    static qreal opacity(const QString &name);
    /** @brief A type style as a font, for example "text-body" or "text-timecode" */
    static QFont font(const QString &style);

    /** @brief A color token with its alpha multiplied by @p factor, for washes and translucent overlays */
    Q_INVOKABLE QColor alpha(const QString &name, qreal factor) const;

    bool isDark() const;
    QVariantMap colorMap() const;
    QVariantMap fontMap() const;

    int space1() const { return space(1); }
    int space2() const { return space(2); }
    int space3() const { return space(3); }
    int space4() const { return space(4); }
    int space5() const { return space(5); }
    int space6() const { return space(6); }
    int radiusXs() const { return radius(QStringLiteral("radius-xs")); }
    int radiusSm() const { return radius(QStringLiteral("radius-sm")); }
    int radiusMd() const { return radius(QStringLiteral("radius-md")); }
    int radiusLg() const { return radius(QStringLiteral("radius-lg")); }
    int radiusXl() const { return radius(QStringLiteral("radius-xl")); }
    int controlSm() const { return size(QStringLiteral("control-sm")); }
    int controlMd() const { return size(QStringLiteral("control-md")); }
    int controlLg() const { return size(QStringLiteral("control-lg")); }
    int iconSm() const { return size(QStringLiteral("icon-sm")); }
    int iconMd() const { return size(QStringLiteral("icon-md")); }
    int iconLg() const { return size(QStringLiteral("icon-lg")); }
    int laneVideo() const { return size(QStringLiteral("lane-video")); }
    int laneAudio() const { return size(QStringLiteral("lane-audio")); }
    qreal opacityDisabled() const { return opacity(QStringLiteral("opacity-disabled")); }
    qreal opacityWaveform() const { return opacity(QStringLiteral("opacity-waveform")); }

Q_SIGNALS:
    void themeChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    DesignTokens();
    void load();
    void updateTheme();
    /** @brief Refresh the theme if the application palette changed since the last lookup */
    void ensureCurrentTheme();
    QColor resolveColor(const QString &name, const QString &theme, int depth = 0) const;

    // name -> theme id -> raw value (hex or {alias})
    QHash<QString, QHash<QString, QString>> m_rawColors;
    QString m_firstTheme;
    QHash<QString, QColor> m_colors;
    QHash<QString, int> m_lengths;
    QHash<QString, qreal> m_numbers;
    QHash<QString, QFont> m_fonts;
    QHash<QString, QString> m_families;
    bool m_dark{true};
    qint64 m_paletteKey{0};
};
