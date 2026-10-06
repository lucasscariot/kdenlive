/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <QProxyStyle>

/** @class KdenliveStyle
    @brief Flat, evenly spaced widget style layered on Fusion.

    Fusion is what Qt falls back to when no desktop style (like Breeze) is available. Its
    gradients and outlines derived from a darker window color look dated and disappear on
    dark palettes, so this proxy repaints the common controls with one set of radii,
    paddings and translucent overlays computed from the palette.
 */
class KdenliveStyle : public QProxyStyle
{
    Q_OBJECT
public:
    using QProxyStyle::QProxyStyle;

    int pixelMetric(PixelMetric metric, const QStyleOption *option = nullptr, const QWidget *widget = nullptr) const override;
    int styleHint(StyleHint hint, const QStyleOption *option = nullptr, const QWidget *widget = nullptr, QStyleHintReturn *returnData = nullptr) const override;
    QSize sizeFromContents(ContentsType type, const QStyleOption *option, const QSize &size, const QWidget *widget) const override;
    QRect subControlRect(ComplexControl control, const QStyleOptionComplex *option, SubControl subControl, const QWidget *widget = nullptr) const override;
    QRect subElementRect(SubElement element, const QStyleOption *option, const QWidget *widget = nullptr) const override;
    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget = nullptr) const override;
    void drawControl(ControlElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget = nullptr) const override;
    void drawComplexControl(ComplexControl control, const QStyleOptionComplex *option, QPainter *painter, const QWidget *widget = nullptr) const override;
    void polish(QWidget *widget) override;
    void unpolish(QWidget *widget) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

    /** @brief Install this style if the application uses Fusion */
    static void installIfFusion();
    /** @brief A new instance of the application style, for widgets that need their own style object */
    static QStyle *cloneApplicationStyle();
    /** @brief Background of panel headers (tab strips, dock title bars) */
    static QColor headerColor(const QPalette &palette);
    /** @brief The smaller font used for panel chrome, so content stays dominant */
    static QFont chromeFont(const QFont &base);
    /** @brief Background of dialogs, slightly raised above the main window so they read as a separate layer */
    static QColor elevatedColor(const QPalette &palette);
    /** @brief Text color at the given opacity, for hover overlays and hairlines */
    static QColor overlay(const QPalette &palette, qreal alpha);
};
