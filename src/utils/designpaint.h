/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QRectF>

class QPainter;
class QWidget;

/** @brief Painting primitives of the Kdenlive Pro design, shared by the widget style and custom widgets
    so every panel, pill, chevron and header strip is drawn the same way.
 */
namespace DesignPaint {

enum class Direction { Up, Down, Left, Right };

/** @brief @p color with its alpha multiplied by @p factor */
QColor withAlpha(QColor color, qreal factor);

/** @brief A rounded rectangle with an optional fill and 1px border, aligned to whole pixels */
void panel(QPainter *painter, const QRectF &rect, const QColor &fill, const QColor &border = Qt::transparent, qreal radius = -1);

/** @brief A pill (fully rounded) shape */
void pill(QPainter *painter, const QRectF &rect, const QColor &fill);

/** @brief A 1.5px chevron centered in @p rect */
void chevron(QPainter *painter, const QRectF &rect, Direction direction, const QColor &color);

/** @brief The header strip behind tabs and title bars: surface-sidebar with a separator hairline below */
void headerStrip(QPainter *painter, const QRect &rect);

/** @brief @p icon rendered at @p size and recolored to @p color, for icons on accent fills or in an active state */
QPixmap tinted(const QIcon &icon, const QSize &size, const QColor &color, qreal devicePixelRatio, QIcon::Mode mode = QIcon::Normal);

/** @brief Wash under a transparent control: none, hover or pressed */
QColor wash(bool hovered, bool pressed);

/** @brief A Qt style sheet for a small badge (labels and flat buttons) in design tokens:
    @p fillToken behind @p textToken, radius-sm corners and space-1 / space-2 padding.
    @p selector is the widget class, for example "QLabel" or "QPushButton". An empty fill means transparent. */
QString badgeStyleSheet(const QString &selector, const QString &fillToken, const QString &textToken, const QString &hoverFillToken = QString());

/** @brief Make @p widget paint its background with a color token instead of the palette */
void setBackground(QWidget *widget, const QString &colorToken);

} // namespace DesignPaint
