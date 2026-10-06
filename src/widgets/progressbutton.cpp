/*
SPDX-FileCopyrightText: 2016 Jean-Baptiste Mardelle <jb@kdenlive.org>
This file is part of Kdenlive. See www.kdenlive.org.

SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "progressbutton.h"

#include <KLocalizedString>
#include <QAction>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionToolButton>

ProgressButton::ProgressButton(const QString &text, double max, QWidget *parent)
    : QToolButton(parent)
    , m_defaultAction(nullptr)
    , m_max(max)
{
    setPopupMode(MenuButtonPopup);
    m_progress = width() - 6;
    m_iconSize = height() - 4;
    m_progressFont = font();
    m_progressFont.setPixelSize(m_iconSize / 2);
    initStyleOption(&m_buttonStyle);
    QPixmap pix(m_iconSize, m_iconSize);
    pix.fill(Qt::transparent);
    m_dummyAction = new QAction(QIcon(pix), text, this);
}

ProgressButton::~ProgressButton()
{
    delete m_dummyAction;
}

void ProgressButton::defineDefaultAction(QAction *action, QAction *actionInProgress)
{
    setDefaultAction(action);
    m_defaultAction = action;
    if (actionInProgress) {
        connect(m_dummyAction, &QAction::triggered, actionInProgress, &QAction::trigger);
    }
}

void ProgressButton::setProgress(int progress)
{
    int prog = m_iconSize * progress / m_max;
    QString remaining;
    if (m_timer.isValid() && progress > 0) {
        // calculate remaining time
        qint64 ms = m_timer.elapsed() * (m_max - progress) / progress;
        if (ms < 60000)
        // xgettext:no-c-format
        {
            remaining = i18nc("s as seconds", "%1s", ms / 1000);
        } else if (ms < 3600000)
        // xgettext:no-c-format
        {
            remaining = i18nc("m as minutes", "%1m", ms / 60000);
        } else {
            remaining = i18nc("h as hours", "%1h", qMin(99, (int)(ms / 3600000)));
        }
    }
    if (progress < 0) {
        setDefaultAction(m_defaultAction);
        m_remainingTime.clear();
        m_timer.invalidate();
        m_progress = -1;
        update();
        return;
    }
    if (!m_timer.isValid() || progress == 0) {
        setDefaultAction(m_dummyAction);
        m_timer.start();
    }
    if (progress == m_max) {
        setDefaultAction(m_defaultAction);
        m_remainingTime.clear();
        m_timer.invalidate();
    }
    if (remaining != m_remainingTime || m_progress != prog) {
        m_progress = prog;
        m_remainingTime = remaining;
        update();
    } else {
        m_progress = prog;
        m_remainingTime = remaining;
    }
}

void ProgressButton::setPrimary(bool primary)
{
    m_primary = primary;
    updateGeometry();
    update();
}

QSize ProgressButton::sizeHint() const
{
    QSize size = QToolButton::sizeHint();
    if (m_primary) {
        // The toolbar may size us as icon only, but the primary look shows the bold label
        QFont bold = font();
        bold.setBold(true);
        const QFontMetrics metrics(bold);
        const int menuWidth = popupMode() == MenuButtonPopup ? style()->pixelMetric(QStyle::PM_MenuButtonIndicator, nullptr, this) + 6 : 0;
        size.setWidth(metrics.horizontalAdvance(text()) + 2 * 14 + menuWidth);
        size.setHeight(metrics.height() + 14);
    }
    return size;
}

int ProgressButton::progress() const
{
    return m_progress;
}

void ProgressButton::paintEvent(QPaintEvent *event)
{
    if (m_primary && m_progress >= m_iconSize) {
        QPainter painter(this);
        QStyleOptionToolButton opt;
        initStyleOption(&opt);
        const bool hasMenu = popupMode() == MenuButtonPopup;
        // Use the style's menu area, so the drawn segment matches where clicks open the menu
        const QRect menuRect = hasMenu ? style()->subControlRect(QStyle::CC_ToolButton, &opt, QStyle::SC_ToolButtonMenu, this) : QRect();
        const QRectF body = QRectF(rect()).adjusted(0.5, 2.5, -0.5, -2.5);
        QColor bg = palette().highlight().color();
        if (!isEnabled()) {
            bg.setAlphaF(0.35);
        } else if (opt.state & QStyle::State_Sunken) {
            bg = bg.darker(115);
        } else if (opt.state & QStyle::State_MouseOver) {
            bg = bg.lighter(112);
        }
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(bg);
        painter.drawRoundedRect(body, 4, 4);

        const QColor fg = palette().highlightedText().color();
        QFont bold = font();
        bold.setBold(true);
        painter.setFont(bold);
        painter.setPen(fg);
        QRectF label = body;
        if (hasMenu) {
            label.setRight(menuRect.left());
            // Divider and chevron for the menu segment
            QColor divider = fg;
            divider.setAlphaF(0.35);
            painter.setPen(QPen(divider, 1));
            painter.drawLine(QPointF(menuRect.left() + 0.5, body.top() + 6), QPointF(menuRect.left() + 0.5, body.bottom() - 6));
            const QPointF c = QRectF(menuRect).center() + QPointF(0.5, 0.5);
            QPainterPath chevron;
            chevron.moveTo(c.x() - 3.5, c.y() - 1.75);
            chevron.lineTo(c.x(), c.y() + 1.75);
            chevron.lineTo(c.x() + 3.5, c.y() - 1.75);
            painter.setPen(QPen(fg, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(chevron);
            painter.setPen(fg);
        }
        painter.drawText(label, Qt::AlignCenter, text());
        return;
    }
    QToolButton::paintEvent(event);
    if (m_progress < m_iconSize) {
        QPainter painter(this);
        painter.setFont(m_progressFont);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QRect rect(3, (height() - m_iconSize) / 2, m_iconSize, m_iconSize);
        // draw remaining time
        if (m_remainingTime.isEmpty() && m_progress >= 0) {
            // We just started task, no time estimation yet, display starting status
            if (m_defaultAction) {
                painter.drawPixmap(rect.adjusted(4, 0, -4, -8), m_defaultAction->icon().pixmap(m_iconSize - 8, m_iconSize - 8));
            }
        } else {
            painter.drawText(rect, Qt::AlignHCenter, m_remainingTime);
        }
        if (m_progress < 0) {
            painter.fillRect(rect.x(), rect.bottom() - 5, rect.width(), 3, Qt::red);
        } else {
            QColor w(Qt::white);
            w.setAlpha(40);
            painter.fillRect(rect.x(), rect.bottom() - 6, m_progress, 4, palette().highlight().color());
            painter.fillRect(rect.x(), rect.bottom() - 6, rect.width(), 4, w);
        }
        painter.setPen(palette().shadow().color());
        painter.drawRoundedRect(rect.x(), rect.bottom() - 7, rect.width(), 6, 2, 2);
    }
}
