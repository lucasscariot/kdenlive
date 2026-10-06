/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <QProxyStyle>

/** @class LegibleFusionStyle
    @brief Fusion derives check box and radio button outlines from a darker window color,
    which makes them invisible on dark palettes. This proxy draws a readable outline on top.
 */
class LegibleFusionStyle : public QProxyStyle
{
    Q_OBJECT
public:
    using QProxyStyle::QProxyStyle;

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget = nullptr) const override;

    /** @brief Install this style if the application uses Fusion */
    static void installIfFusion();
    /** @brief The key to recreate @p style with QStyleFactory, proxies have no name of their own */
    static QString factoryKey(const QStyle *style);
};
