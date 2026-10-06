/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "legiblestyle.h"

#include <QApplication>
#include <QPainter>
#include <QStyleFactory>
#include <QStyleOption>

void LegibleFusionStyle::drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget) const
{
    QProxyStyle::drawPrimitive(element, option, painter, widget);
    if (element != PE_IndicatorCheckBox && element != PE_IndicatorRadioButton) {
        return;
    }
    const bool enabled = option->state & State_Enabled;
    QColor border;
    if (enabled && (option->state & (State_MouseOver | State_HasFocus))) {
        border = option->palette.color(QPalette::Highlight);
    } else {
        border = option->palette.color(QPalette::Text);
        border.setAlphaF(enabled ? 0.45 : 0.2);
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(QPen(border, 1));
    painter->setBrush(Qt::NoBrush);
    const QRectF rect = QRectF(option->rect).adjusted(0.5, 0.5, -1.5, -1.5);
    if (element == PE_IndicatorRadioButton) {
        painter->drawEllipse(rect.adjusted(0.5, 0.5, -0.5, -0.5));
    } else {
        painter->drawRoundedRect(rect, 1.5, 1.5);
    }
    painter->restore();
}

void LegibleFusionStyle::installIfFusion()
{
    if (QApplication::style()->name().compare(QLatin1String("fusion"), Qt::CaseInsensitive) == 0) {
        QApplication::setStyle(new LegibleFusionStyle(QStyleFactory::create(QStringLiteral("fusion"))));
    }
}

QString LegibleFusionStyle::factoryKey(const QStyle *style)
{
    QString key = style->name();
    if (key.isEmpty()) {
        if (const auto *proxy = qobject_cast<const QProxyStyle *>(style)) {
            key = proxy->baseStyle()->name();
        }
    }
    return key;
}
