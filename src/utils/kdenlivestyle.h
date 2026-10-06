/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <QProxyStyle>

/** @class KdenliveStyle
    @brief The Kdenlive Pro widget style, layered on Fusion.

    Every metric, radius, color and font comes from DesignTokens and every shape from
    DesignPaint, so widgets match the design system and the QML views reading the same tokens.

    Widgets opt into variants with dynamic properties:
    - `_kdenlive_primary`: the accent filled primary action of a dialog or bar (push and tool buttons)
    - `_kdenlive_segmented`: a segment of a segmented control (checkable push buttons)
    - `_kdenlive_pagebar`: a page of the bottom page bar (checkable push buttons)
    - `_kdenlive_panel_toggle`: a quiet toggle in the panel top bar (checkable tool buttons)
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

    /** @brief Install this style and the design fonts if the application uses Fusion */
    static void installIfFusion();
    /** @brief A new instance of the application style, for widgets that need their own style object */
    static QStyle *cloneApplicationStyle();
    /** @brief True when the application uses this style, and with it the design fonts */
    static bool isActive();
};
