/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "designpaint.h"
#include "designtokens.h"

#include <QPainter>
#include <QPainterPath>
#include <QWidget>

namespace DesignPaint {

QColor withAlpha(QColor color, qreal factor)
{
    color.setAlphaF(color.alphaF() * factor);
    return color;
}

void panel(QPainter *painter, const QRectF &rect, const QColor &fill, const QColor &border, qreal radius)
{
    if (radius < 0) {
        radius = DesignTokens::radius(QStringLiteral("radius-md"));
    }
    // Half pixel inset so 1px borders land on whole pixels
    const QRectF aligned = rect.adjusted(0.5, 0.5, -0.5, -0.5);
    radius = qMin(radius, qMin(aligned.width(), aligned.height()) / 2.);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(border.alpha() > 0 ? QPen(border, 1) : QPen(Qt::NoPen));
    painter->setBrush(fill.alpha() > 0 ? QBrush(fill) : QBrush(Qt::NoBrush));
    painter->drawRoundedRect(aligned, radius, radius);
    painter->restore();
}

void pill(QPainter *painter, const QRectF &rect, const QColor &fill)
{
    panel(painter, rect, fill, Qt::transparent, qMin(rect.width(), rect.height()) / 2.);
}

void chevron(QPainter *painter, const QRectF &rect, Direction direction, const QColor &color)
{
    const qreal size = qMin(qMin(rect.width(), rect.height()), 9.) / 2.;
    if (size <= 0) {
        return;
    }
    const QPointF c = rect.center();
    QPainterPath path;
    switch (direction) {
    case Direction::Down:
        path.moveTo(c.x() - size, c.y() - size / 2);
        path.lineTo(c.x(), c.y() + size / 2);
        path.lineTo(c.x() + size, c.y() - size / 2);
        break;
    case Direction::Up:
        path.moveTo(c.x() - size, c.y() + size / 2);
        path.lineTo(c.x(), c.y() - size / 2);
        path.lineTo(c.x() + size, c.y() + size / 2);
        break;
    case Direction::Left:
        path.moveTo(c.x() + size / 2, c.y() - size);
        path.lineTo(c.x() - size / 2, c.y());
        path.lineTo(c.x() + size / 2, c.y() + size);
        break;
    case Direction::Right:
        path.moveTo(c.x() - size / 2, c.y() - size);
        path.lineTo(c.x() + size / 2, c.y());
        path.lineTo(c.x() - size / 2, c.y() + size);
        break;
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(path);
    painter->restore();
}

void headerStrip(QPainter *painter, const QRect &rect)
{
    painter->fillRect(rect, DesignTokens::color(QStringLiteral("surface-sidebar")));
    painter->fillRect(QRect(rect.left(), rect.bottom(), rect.width(), 1), DesignTokens::color(QStringLiteral("separator")));
}

QColor wash(bool hovered, bool pressed)
{
    if (pressed) {
        return DesignTokens::color(QStringLiteral("fill-pressed"));
    }
    return hovered ? DesignTokens::color(QStringLiteral("fill-hover")) : QColor(Qt::transparent);
}

void setBackground(QWidget *widget, const QString &colorToken)
{
    QPalette palette = widget->palette();
    palette.setColor(QPalette::Window, DesignTokens::color(colorToken));
    widget->setPalette(palette);
    widget->setAutoFillBackground(true);
}

} // namespace DesignPaint
