/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "kdenlivestyle.h"

#include <KColorScheme>
#include <QAbstractItemView>
#include <QApplication>
#include <QCursor>
#include <QDialog>
#include <QEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QStyleFactory>
#include <QStyleOption>
#include <QTabBar>
#include <QWidget>

#include <tuple>

namespace {
constexpr qreal Radius = 4.;

enum class Direction { Up, Down, Left, Right };

void drawChevron(QPainter *painter, const QRectF &rect, Direction direction, const QColor &color)
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

QRectF pixelAligned(const QRect &rect)
{
    // Half pixel inset so 1px pens land on whole pixels
    return QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5);
}

void drawRoundedPanel(QPainter *painter, const QRect &rect, const QColor &fill, const QColor &border, qreal radius = Radius)
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(border.alpha() > 0 ? QPen(border, 1) : QPen(Qt::NoPen));
    painter->setBrush(fill.alpha() > 0 ? QBrush(fill) : QBrush(Qt::NoBrush));
    painter->drawRoundedRect(pixelAligned(rect), radius, radius);
    painter->restore();
}

QColor textColor(const QStyleOption *option)
{
    return option->palette.color(option->state & QStyle::State_Enabled ? QPalette::Active : QPalette::Disabled, QPalette::Text);
}

QColor withAlpha(QColor color, qreal alpha)
{
    color.setAlphaF(color.alphaF() * alpha);
    return color;
}

/** @brief Input fields: line edits, spin boxes, editable combo boxes */
void drawInput(QPainter *painter, const QStyleOption *option, const QRect &rect)
{
    const bool enabled = option->state & QStyle::State_Enabled;
    QColor border = KdenliveStyle::overlay(option->palette, enabled && (option->state & QStyle::State_MouseOver) ? 0.24 : 0.13);
    if (enabled && (option->state & QStyle::State_HasFocus)) {
        border = option->palette.color(QPalette::Highlight);
    }
    drawRoundedPanel(painter, rect, option->palette.color(QPalette::Base), border);
}

/** @brief Push buttons and non editable combo boxes */
void drawButton(QPainter *painter, const QStyleOption *option, const QRect &rect, bool isDefault)
{
    const bool enabled = option->state & QStyle::State_Enabled;
    const bool sunken = option->state & (QStyle::State_Sunken | QStyle::State_On);
    const bool hover = enabled && (option->state & QStyle::State_MouseOver);
    QColor fill = option->palette.color(QPalette::Button);
    QColor border = KdenliveStyle::overlay(option->palette, hover ? 0.2 : 0.1);
    if (isDefault && enabled) {
        fill = option->palette.color(QPalette::Highlight);
        border = Qt::transparent;
    }
    if (sunken) {
        fill = fill.darker(115);
    } else if (hover) {
        fill = fill.lighter(isDefault ? 112 : 118);
    }
    if (!enabled) {
        fill = withAlpha(fill, 0.6);
    }
    drawRoundedPanel(painter, rect, fill, border);
}
/** @brief Accent filled buttons: the dialog default, or one explicitly marked as the primary action */
bool isPrimaryButton(const QStyleOptionButton *button, const QWidget *widget)
{
    if (widget && widget->property("_kdenlive_primary").toBool()) {
        return true;
    }
    // Icon only buttons (like file choosers) become default when focused, an accent square there is noise
    return (button->features & QStyleOptionButton::DefaultButton) && !button->text.isEmpty();
}
} // namespace

QColor KdenliveStyle::overlay(const QPalette &palette, qreal alpha)
{
    return withAlpha(palette.color(QPalette::WindowText), alpha);
}

QColor KdenliveStyle::elevatedColor(const QPalette &palette)
{
    const QColor window = palette.color(QPalette::Window);
    const QColor text = palette.color(QPalette::WindowText);
    const qreal mix = 0.07;
    return QColor::fromRgbF(window.redF() + (text.redF() - window.redF()) * mix, window.greenF() + (text.greenF() - window.greenF()) * mix,
                            window.blueF() + (text.blueF() - window.blueF()) * mix);
}

void KdenliveStyle::polish(QWidget *widget)
{
    QProxyStyle::polish(widget);
    if (qobject_cast<QDialog *>(widget)) {
        widget->installEventFilter(this);
    }
}

void KdenliveStyle::unpolish(QWidget *widget)
{
    if (qobject_cast<QDialog *>(widget)) {
        widget->removeEventFilter(this);
    }
    QProxyStyle::unpolish(widget);
}

bool KdenliveStyle::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Paint) {
        auto *dialog = qobject_cast<QDialog *>(watched);
        if (dialog && dialog->isWindow()) {
            // Dialogs float over the main window without a shadow of their own: a raised surface and a hairline border separate them
            QPainter painter(dialog);
            painter.fillRect(dialog->rect(), elevatedColor(dialog->palette()));
            painter.setPen(overlay(dialog->palette(), 0.24));
            painter.drawRect(dialog->rect().adjusted(0, 0, -1, -1));
        }
    }
    return QProxyStyle::eventFilter(watched, event);
}

QColor KdenliveStyle::headerColor(const QPalette &palette)
{
    KColorScheme scheme(palette.currentColorGroup(), KColorScheme::Header);
    return scheme.background(KColorScheme::NormalBackground).color();
}

QFont KdenliveStyle::chromeFont(const QFont &base)
{
    QFont font(base);
    if (font.pointSizeF() > 0) {
        font.setPointSizeF(font.pointSizeF() * 0.92);
    } else if (font.pixelSize() > 0) {
        font.setPixelSize(qRound(font.pixelSize() * 0.92));
    }
    return font;
}

void KdenliveStyle::installIfFusion()
{
    if (QApplication::style()->name().compare(QLatin1String("fusion"), Qt::CaseInsensitive) == 0) {
        QApplication::setStyle(new KdenliveStyle(QStyleFactory::create(QStringLiteral("fusion"))));
    }
}

QStyle *KdenliveStyle::cloneApplicationStyle()
{
    if (qobject_cast<KdenliveStyle *>(QApplication::style())) {
        return new KdenliveStyle(QStyleFactory::create(QStringLiteral("fusion")));
    }
    return QStyleFactory::create(QApplication::style()->name());
}

int KdenliveStyle::pixelMetric(PixelMetric metric, const QStyleOption *option, const QWidget *widget) const
{
    switch (metric) {
    case PM_ButtonMargin:
        return 10;
    case PM_ButtonShiftHorizontal:
    case PM_ButtonShiftVertical:
    case PM_TabBarTabShiftHorizontal:
    case PM_TabBarTabShiftVertical:
        return 0;
    case PM_ToolBarItemSpacing:
        return 4;
    case PM_ToolBarItemMargin:
        return 4;
    case PM_ToolBarFrameWidth:
        return 0;
    case PM_ToolBarSeparatorExtent:
        return 13;
    case PM_TabBarTabHSpace:
        return 24;
    case PM_TabBarTabVSpace:
        return 12;
    case PM_TabBarBaseOverlap:
        return 0;
    case PM_ScrollBarExtent:
        return 10;
    case PM_ScrollBarSliderMin:
        return 32;
    case PM_IndicatorWidth:
    case PM_IndicatorHeight:
    case PM_ExclusiveIndicatorWidth:
    case PM_ExclusiveIndicatorHeight:
        return 16;
    case PM_CheckBoxLabelSpacing:
    case PM_RadioButtonLabelSpacing:
        return 8;
    case PM_MenuBarItemSpacing:
        return 2;
    case PM_MenuBarHMargin:
        return 4;
    case PM_MenuBarVMargin:
        return 2;
    case PM_MenuHMargin:
    case PM_MenuVMargin:
        return 4;
    case PM_HeaderMargin:
        return 6;
    case PM_TabCloseIndicatorWidth:
    case PM_TabCloseIndicatorHeight:
        return 16;
    case PM_MenuButtonIndicator:
        // Wide enough to aim at the menu part of split buttons
        return 18;
    case PM_LayoutHorizontalSpacing:
    case PM_LayoutVerticalSpacing:
        return 6;
    default:
        return QProxyStyle::pixelMetric(metric, option, widget);
    }
}

int KdenliveStyle::styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget, QStyleHintReturn *returnData) const
{
    switch (hint) {
    case SH_ScrollBar_Transient:
        return 0;
    case SH_ToolBox_SelectedPageTitleBold:
        return 1;
    default:
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }
}

QSize KdenliveStyle::sizeFromContents(ContentsType type, const QStyleOption *option, const QSize &size, const QWidget *widget) const
{
    QSize s = QProxyStyle::sizeFromContents(type, option, size, widget);
    switch (type) {
    case CT_PushButton:
        s.setHeight(qMax(s.height(), option->fontMetrics.height() + 12));
        break;
    case CT_ToolButton:
        s += QSize(4, 4);
        break;
    case CT_ComboBox:
    case CT_LineEdit:
    case CT_SpinBox:
        s.setHeight(qMax(s.height(), option->fontMetrics.height() + 10));
        break;
    case CT_HeaderSection:
        s.setHeight(s.height() + 4);
        break;
    default:
        break;
    }
    return s;
}

QRect KdenliveStyle::subElementRect(SubElement element, const QStyleOption *option, const QWidget *widget) const
{
    if (element == SE_TabBarTearIndicatorLeft || element == SE_TabBarTearIndicatorRight) {
        // Wide fade where tabs are cut by the scroll buttons
        if (const auto *tab = qstyleoption_cast<const QStyleOptionTab *>(option)) {
            const bool vertical = tab->shape == QTabBar::RoundedWest || tab->shape == QTabBar::RoundedEast || tab->shape == QTabBar::TriangularWest ||
                                  tab->shape == QTabBar::TriangularEast;
            const int fade = 28;
            const QRect r = tab->rect;
            if (!vertical) {
                return element == SE_TabBarTearIndicatorLeft ? QRect(r.left(), r.top(), fade, r.height())
                                                             : QRect(r.right() - fade + 1, r.top(), fade, r.height());
            }
            return element == SE_TabBarTearIndicatorLeft ? QRect(r.left(), r.top(), r.width(), fade) : QRect(r.left(), r.bottom() - fade + 1, r.width(), fade);
        }
    }
    return QProxyStyle::subElementRect(element, option, widget);
}

QRect KdenliveStyle::subControlRect(ComplexControl control, const QStyleOptionComplex *option, SubControl subControl, const QWidget *widget) const
{
    if (control == CC_ScrollBar) {
        // Slim scroll bars without arrow buttons: the groove is the whole widget
        if (const auto *bar = qstyleoption_cast<const QStyleOptionSlider *>(option)) {
            const QRect r = bar->rect;
            const bool horizontal = bar->orientation == Qt::Horizontal;
            const int length = horizontal ? r.width() : r.height();
            int sliderLength = length;
            if (bar->maximum != bar->minimum) {
                const qint64 range = qint64(bar->maximum) - bar->minimum;
                sliderLength = int((qint64(bar->pageStep) * length) / (range + bar->pageStep));
                sliderLength = qBound(qMin(proxy()->pixelMetric(PM_ScrollBarSliderMin, bar, widget), length), sliderLength, length);
            }
            const int start = sliderPositionFromValue(bar->minimum, bar->maximum, bar->sliderPosition, length - sliderLength, bar->upsideDown);
            QRect result;
            switch (subControl) {
            case SC_ScrollBarGroove:
                result = r;
                break;
            case SC_ScrollBarSlider:
                result = horizontal ? QRect(r.x() + start, r.y(), sliderLength, r.height()) : QRect(r.x(), r.y() + start, r.width(), sliderLength);
                break;
            case SC_ScrollBarSubPage:
                result = horizontal ? QRect(r.x(), r.y(), start, r.height()) : QRect(r.x(), r.y(), r.width(), start);
                break;
            case SC_ScrollBarAddPage:
                result = horizontal ? QRect(r.x() + start + sliderLength, r.y(), length - start - sliderLength, r.height())
                                    : QRect(r.x(), r.y() + start + sliderLength, r.width(), length - start - sliderLength);
                break;
            default:
                // No arrow buttons
                return QRect();
            }
            return visualRect(bar->direction, r, result);
        }
    }
    return QProxyStyle::subControlRect(control, option, subControl, widget);
}

void KdenliveStyle::drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget) const
{
    const bool enabled = option->state & State_Enabled;
    switch (element) {
    case PE_PanelButtonTool: {
        const bool autoRaise = option->state & State_AutoRaise;
        QColor fill = Qt::transparent;
        if (option->state & State_Sunken) {
            fill = overlay(option->palette, 0.16);
        } else if (option->state & State_On) {
            fill = withAlpha(option->palette.color(QPalette::Highlight), 0.35);
        } else if (enabled && (option->state & State_MouseOver)) {
            fill = overlay(option->palette, 0.09);
        }
        if (!autoRaise && fill.alpha() == 0) {
            // Stand alone tool buttons still read as buttons
            drawButton(painter, option, option->rect, false);
            return;
        }
        drawRoundedPanel(painter, option->rect, fill, Qt::transparent);
        return;
    }
    case PE_PanelButtonCommand:
        drawButton(painter, option, option->rect, false);
        return;
    case PE_PanelLineEdit:
        if (const auto *frame = qstyleoption_cast<const QStyleOptionFrame *>(option)) {
            if (frame->lineWidth > 0) {
                drawInput(painter, option, option->rect);
                return;
            }
        }
        break;
    case PE_FrameLineEdit: {
        const bool enabledFocus = enabled && (option->state & State_HasFocus);
        drawRoundedPanel(painter, option->rect, Qt::transparent, enabledFocus ? option->palette.color(QPalette::Highlight) : overlay(option->palette, 0.13));
        return;
    }
    case PE_IndicatorCheckBox:
    case PE_IndicatorItemViewItemCheck: {
        const int side = qMin(option->rect.width(), option->rect.height());
        QRect box(0, 0, side, side);
        box.moveCenter(option->rect.center());
        const bool on = option->state & (State_On | State_NoChange);
        const QColor accent = option->palette.color(QPalette::Highlight);
        QColor border = on ? accent : overlay(option->palette, enabled && (option->state & State_MouseOver) ? 0.6 : 0.42);
        QColor fill = on ? accent : option->palette.color(QPalette::Base);
        if (!enabled) {
            border = withAlpha(border, 0.5);
            fill = withAlpha(fill, 0.5);
        }
        drawRoundedPanel(painter, box, fill, border, 3);
        if (on) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setPen(QPen(option->palette.color(QPalette::HighlightedText), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const QRectF r(box);
            if (option->state & State_NoChange) {
                painter->drawLine(QPointF(r.left() + r.width() * 0.28, r.center().y()), QPointF(r.right() - r.width() * 0.28, r.center().y()));
            } else {
                QPainterPath check;
                check.moveTo(r.left() + r.width() * 0.27, r.top() + r.height() * 0.52);
                check.lineTo(r.left() + r.width() * 0.43, r.top() + r.height() * 0.68);
                check.lineTo(r.left() + r.width() * 0.75, r.top() + r.height() * 0.34);
                painter->drawPath(check);
            }
            painter->restore();
        }
        return;
    }
    case PE_IndicatorRadioButton: {
        const int side = qMin(option->rect.width(), option->rect.height());
        QRect circle(0, 0, side, side);
        circle.moveCenter(option->rect.center());
        const bool on = option->state & State_On;
        const QColor accent = option->palette.color(QPalette::Highlight);
        QColor border = on ? accent : overlay(option->palette, enabled && (option->state & State_MouseOver) ? 0.6 : 0.42);
        QColor fill = on ? accent : option->palette.color(QPalette::Base);
        if (!enabled) {
            border = withAlpha(border, 0.5);
            fill = withAlpha(fill, 0.5);
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen(border, 1));
        painter->setBrush(fill);
        painter->drawEllipse(pixelAligned(circle));
        if (on) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(option->palette.color(QPalette::HighlightedText));
            const qreal dot = side * 0.18;
            painter->drawEllipse(QRectF(circle).center(), dot, dot);
        }
        painter->restore();
        return;
    }
    case PE_FrameFocusRect:
        // Item views show focus through selection; elsewhere a thin accent ring
        if (qobject_cast<const QAbstractItemView *>(widget)) {
            return;
        }
        drawRoundedPanel(painter, option->rect, Qt::transparent, withAlpha(option->palette.color(QPalette::Highlight), 0.7), 3);
        return;
    case PE_IndicatorToolBarSeparator: {
        painter->save();
        painter->setPen(overlay(option->palette, 0.12));
        const QRect r = option->rect;
        if (option->state & State_Horizontal) {
            const int x = r.center().x();
            painter->drawLine(x, r.top() + 6, x, r.bottom() - 6);
        } else {
            const int y = r.center().y();
            painter->drawLine(r.left() + 6, y, r.right() - 6, y);
        }
        painter->restore();
        return;
    }
    case PE_IndicatorToolBarHandle:
    case PE_PanelToolBar:
        return;
    case PE_FrameTabBarBase:
        painter->fillRect(QRect(option->rect.left(), option->rect.bottom(), option->rect.width(), 1), overlay(option->palette, 0.1));
        return;
    case PE_FrameTabWidget:
        drawRoundedPanel(painter, option->rect, Qt::transparent, overlay(option->palette, 0.1));
        return;
    case PE_FrameGroupBox:
        drawRoundedPanel(painter, option->rect, Qt::transparent, overlay(option->palette, 0.12));
        return;
    case PE_PanelTipLabel:
        drawRoundedPanel(painter, option->rect, option->palette.color(QPalette::ToolTipBase), overlay(option->palette, 0.15), 3);
        return;
    case PE_IndicatorArrowDown:
    case PE_IndicatorArrowUp:
    case PE_IndicatorArrowLeft:
    case PE_IndicatorArrowRight: {
        const Direction direction = element == PE_IndicatorArrowDown   ? Direction::Down
                                    : element == PE_IndicatorArrowUp   ? Direction::Up
                                    : element == PE_IndicatorArrowLeft ? Direction::Left
                                                                       : Direction::Right;
        drawChevron(painter, option->rect, direction, withAlpha(textColor(option), 0.8));
        return;
    }
    case PE_IndicatorButtonDropDown:
        return;
    case PE_IndicatorTabTearLeft:
    case PE_IndicatorTabTearRight: {
        // Fade the cut tab into the header instead of clipping it hard
        const QRect r = option->rect;
        const QColor header = headerColor(option->palette);
        QColor clear = header;
        clear.setAlpha(0);
        QLinearGradient gradient(r.topLeft(), r.topRight());
        gradient.setColorAt(element == PE_IndicatorTabTearLeft ? 0 : 1, header);
        gradient.setColorAt(element == PE_IndicatorTabTearLeft ? 1 : 0, clear);
        painter->fillRect(r, gradient);
        return;
    }
    case PE_IndicatorTabClose: {
        const bool hover = enabled && (option->state & (State_MouseOver | State_Raised));
        const bool current = option->state & State_Selected;
        if (!hover && !current && widget) {
            // Only the current tab and the hovered one show their close button
            if (const auto *bar = qobject_cast<const QTabBar *>(widget->parentWidget())) {
                const int hovered = bar->tabAt(bar->mapFromGlobal(QCursor::pos()));
                if (hovered < 0 || (bar->tabButton(hovered, QTabBar::RightSide) != widget && bar->tabButton(hovered, QTabBar::LeftSide) != widget)) {
                    return;
                }
            }
        }
        QRect box(0, 0, 16, 16);
        box.moveCenter(option->rect.center());
        if (hover) {
            const QColor base = current ? option->palette.color(QPalette::HighlightedText) : option->palette.color(QPalette::WindowText);
            drawRoundedPanel(painter, box, withAlpha(base, option->state & State_Sunken ? 0.25 : 0.16), Qt::transparent, box.height() / 2.);
        }
        const QColor color =
            current ? withAlpha(option->palette.color(QPalette::HighlightedText), hover ? 1.0 : 0.75) : overlay(option->palette, hover ? 0.9 : 0.35);
        const QRectF cross = QRectF(box).adjusted(5, 5, -5, -5);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen(color, 1.4, Qt::SolidLine, Qt::RoundCap));
        painter->drawLine(cross.topLeft(), cross.bottomRight());
        painter->drawLine(cross.topRight(), cross.bottomLeft());
        painter->restore();
        return;
    }
    default:
        break;
    }
    QProxyStyle::drawPrimitive(element, option, painter, widget);
}

void KdenliveStyle::drawControl(ControlElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget) const
{
    switch (element) {
    case CE_PushButtonBevel:
        if (const auto *button = qstyleoption_cast<const QStyleOptionButton *>(option)) {
            if (widget && widget->property("_kdenlive_segmented").toBool()) {
                // Segmented control: the current segment is raised, the others only react to hover
                if (option->state & State_On) {
                    drawRoundedPanel(painter, option->rect, option->palette.color(QPalette::Button).lighter(125), overlay(option->palette, 0.08), 3.5);
                } else if ((option->state & State_MouseOver) && (option->state & State_Enabled)) {
                    drawRoundedPanel(painter, option->rect, overlay(option->palette, 0.07), Qt::transparent, 3.5);
                }
                return;
            }
            const bool flat = button->features & QStyleOptionButton::Flat;
            if (!flat || (option->state & (State_Sunken | State_On | State_MouseOver))) {
                drawButton(painter, option, option->rect, isPrimaryButton(button, widget));
            }
            if (button->features & QStyleOptionButton::HasMenu) {
                const int indicator = proxy()->pixelMetric(PM_MenuButtonIndicator, option, widget);
                const QRect arrow(option->rect.right() - indicator - 4, option->rect.top(), indicator, option->rect.height());
                drawChevron(painter, arrow, Direction::Down, withAlpha(textColor(option), 0.8));
            }
            return;
        }
        break;
    case CE_PushButtonLabel:
        if (const auto *button = qstyleoption_cast<const QStyleOptionButton *>(option)) {
            if (widget && widget->property("_kdenlive_segmented").toBool() && !(option->state & State_On)) {
                QStyleOptionButton dimmed(*button);
                dimmed.palette.setColor(QPalette::ButtonText, withAlpha(option->palette.color(QPalette::ButtonText), 0.65));
                QProxyStyle::drawControl(element, &dimmed, painter, widget);
                return;
            }
            if (isPrimaryButton(button, widget) && (option->state & State_Enabled)) {
                QStyleOptionButton accent(*button);
                accent.palette.setColor(QPalette::ButtonText, option->palette.color(QPalette::HighlightedText));
                QProxyStyle::drawControl(element, &accent, painter, widget);
                return;
            }
        }
        break;
    case CE_ToolBar:
        return;
    case CE_MenuBarEmptyArea:
        painter->fillRect(option->rect, option->palette.window());
        return;
    case CE_MenuBarItem:
        if (const auto *item = qstyleoption_cast<const QStyleOptionMenuItem *>(option)) {
            QStyleOptionMenuItem plain(*item);
            const bool active = (option->state & State_Selected) && (option->state & State_Enabled);
            plain.state &= ~(State_Selected | State_Sunken);
            painter->fillRect(option->rect, option->palette.window());
            if (active) {
                drawRoundedPanel(painter, option->rect.adjusted(0, 2, 0, -2), overlay(option->palette, option->state & State_Sunken ? 0.16 : 0.1),
                                 Qt::transparent);
            }
            QCommonStyle::drawControl(element, &plain, painter, widget);
            return;
        }
        break;
    case CE_HeaderSection: {
        const QRect r = option->rect;
        painter->fillRect(r, headerColor(option->palette));
        painter->fillRect(QRect(r.left(), r.bottom(), r.width(), 1), overlay(option->palette, 0.12));
        painter->fillRect(QRect(r.right(), r.top() + 4, 1, r.height() - 8), overlay(option->palette, 0.08));
        return;
    }
    case CE_HeaderLabel:
        if (const auto *header = qstyleoption_cast<const QStyleOptionHeader *>(option)) {
            QStyleOptionHeader dimmed(*header);
            dimmed.palette.setColor(QPalette::ButtonText, withAlpha(option->palette.color(QPalette::WindowText), 0.7));
            QProxyStyle::drawControl(element, &dimmed, painter, widget);
            return;
        }
        break;
    case CE_HeaderEmptyArea:
        painter->fillRect(option->rect, headerColor(option->palette));
        painter->fillRect(QRect(option->rect.left(), option->rect.bottom(), option->rect.width(), 1), overlay(option->palette, 0.12));
        return;
    case CE_TabBarTabShape:
        if (const auto *tab = qstyleoption_cast<const QStyleOptionTab *>(option)) {
            // Pill tabs: the current one is filled with the accent, others only react to hover
            const QRect pill = tab->rect.adjusted(3, 4, -3, -4);
            const qreal radius = qMin(pill.width(), pill.height()) / 2.;
            if (tab->state & State_Selected) {
                QColor accent = tab->palette.color(QPalette::Highlight);
                if (!(tab->state & State_Enabled)) {
                    accent = withAlpha(accent, 0.5);
                }
                drawRoundedPanel(painter, pill, accent, Qt::transparent, radius);
            } else if ((tab->state & State_MouseOver) && (tab->state & State_Enabled)) {
                drawRoundedPanel(painter, pill, overlay(tab->palette, 0.08), Qt::transparent, radius);
            }
            return;
        }
        break;
    case CE_TabBarTabLabel:
        if (const auto *tab = qstyleoption_cast<const QStyleOptionTab *>(option)) {
            QStyleOptionTab label(*tab);
            if (tab->state & State_Selected) {
                label.palette.setColor(QPalette::WindowText, tab->palette.color(QPalette::HighlightedText));
                painter->save();
                QFont font = painter->font();
                font.setWeight(QFont::DemiBold);
                painter->setFont(font);
                QProxyStyle::drawControl(element, &label, painter, widget);
                painter->restore();
            } else {
                label.palette.setColor(QPalette::WindowText, withAlpha(tab->palette.color(QPalette::WindowText), 0.6));
                QProxyStyle::drawControl(element, &label, painter, widget);
            }
            return;
        }
        break;
    case CE_Splitter:
        painter->fillRect(option->rect, option->palette.window());
        if (option->state & State_Horizontal) {
            painter->fillRect(QRect(option->rect.center().x(), option->rect.top(), 1, option->rect.height()), overlay(option->palette, 0.1));
        } else {
            painter->fillRect(QRect(option->rect.left(), option->rect.center().y(), option->rect.width(), 1), overlay(option->palette, 0.1));
        }
        return;
    default:
        break;
    }
    QProxyStyle::drawControl(element, option, painter, widget);
}

void KdenliveStyle::drawComplexControl(ComplexControl control, const QStyleOptionComplex *option, QPainter *painter, const QWidget *widget) const
{
    const bool enabled = option->state & State_Enabled;
    switch (control) {
    case CC_ToolButton:
        if (const auto *tool = qstyleoption_cast<const QStyleOptionToolButton *>(option)) {
            if (widget && qobject_cast<const QTabBar *>(widget->parentWidget()) && tool->arrowType != Qt::NoArrow) {
                // Tab bar scroll buttons sit on top of the tabs: give them the header background and a bare chevron
                painter->fillRect(tool->rect, headerColor(option->palette));
                if (enabled && (tool->state & (State_MouseOver | State_Sunken))) {
                    drawRoundedPanel(painter, tool->rect.adjusted(1, 4, -1, -4), overlay(option->palette, tool->state & State_Sunken ? 0.16 : 0.1),
                                     Qt::transparent, 3);
                }
                const Direction direction = tool->arrowType == Qt::LeftArrow    ? Direction::Left
                                            : tool->arrowType == Qt::RightArrow ? Direction::Right
                                            : tool->arrowType == Qt::UpArrow    ? Direction::Up
                                                                                : Direction::Down;
                drawChevron(painter, tool->rect, direction, overlay(option->palette, enabled ? 0.8 : 0.25));
                return;
            }
            const QRect button = proxy()->subControlRect(control, tool, SC_ToolButton, widget);
            const QRect menu = proxy()->subControlRect(control, tool, SC_ToolButtonMenu, widget);
            State flags = tool->state & ~State_Sunken;
            if ((flags & State_AutoRaise) && (!(flags & State_MouseOver) || !enabled)) {
                flags &= ~State_Raised;
            }
            if ((tool->state & State_Sunken) && (tool->activeSubControls & (SC_ToolButton | SC_ToolButtonMenu))) {
                flags |= State_Sunken;
            }
            QStyleOption panel(*tool);
            // One panel for button and menu part, so split buttons read as a single control
            panel.rect = (tool->subControls & SC_ToolButtonMenu) ? tool->rect : button;
            panel.state = flags;
            proxy()->drawPrimitive(PE_PanelButtonTool, &panel, painter, widget);
            if (tool->subControls & SC_ToolButtonMenu) {
                drawChevron(painter, menu, Direction::Down, withAlpha(textColor(option), 0.75));
            } else if (tool->features & QStyleOptionToolButton::HasMenu) {
                const int indicator = qMax(5, proxy()->pixelMetric(PM_MenuButtonIndicator, tool, widget) / 2);
                const QRect arrow(tool->rect.right() - indicator - 1, tool->rect.bottom() - indicator - 1, indicator, indicator);
                drawChevron(painter, arrow, Direction::Down, withAlpha(textColor(option), 0.75));
            }
            QStyleOptionToolButton label(*tool);
            label.state = flags & ~State_Sunken;
            const int frame = proxy()->pixelMetric(PM_DefaultFrameWidth, tool, widget);
            label.rect = button.adjusted(frame, frame, -frame, -frame);
            proxy()->drawControl(CE_ToolButtonLabel, &label, painter, widget);
            return;
        }
        break;
    case CC_ComboBox:
        if (const auto *combo = qstyleoption_cast<const QStyleOptionComboBox *>(option)) {
            if (!combo->frame) {
                break;
            }
            if (combo->editable) {
                drawInput(painter, option, combo->rect);
            } else {
                drawButton(painter, option, combo->rect, false);
            }
            const QRect arrow = proxy()->subControlRect(control, combo, SC_ComboBoxArrow, widget);
            drawChevron(painter, arrow, Direction::Down, withAlpha(textColor(option), 0.75));
            return;
        }
        break;
    case CC_SpinBox:
        if (const auto *spin = qstyleoption_cast<const QStyleOptionSpinBox *>(option)) {
            if (!spin->frame) {
                break;
            }
            drawInput(painter, option, spin->rect);
            if (spin->buttonSymbols != QAbstractSpinBox::NoButtons) {
                const QRect up = proxy()->subControlRect(control, spin, SC_SpinBoxUp, widget);
                const QRect down = proxy()->subControlRect(control, spin, SC_SpinBoxDown, widget);
                for (const auto &[rect, sub, upEnabled] : {std::tuple{up, SC_SpinBoxUp, bool(spin->stepEnabled & QAbstractSpinBox::StepUpEnabled)},
                                                           std::tuple{down, SC_SpinBoxDown, bool(spin->stepEnabled & QAbstractSpinBox::StepDownEnabled)}}) {
                    if (enabled && upEnabled && (spin->activeSubControls & sub) && (spin->state & (State_MouseOver | State_Sunken))) {
                        drawRoundedPanel(painter, rect.adjusted(1, 1, -1, -1), overlay(option->palette, spin->state & State_Sunken ? 0.16 : 0.09),
                                         Qt::transparent, 3);
                    }
                    QColor color = withAlpha(textColor(option), upEnabled ? 0.75 : 0.3);
                    if (spin->buttonSymbols == QAbstractSpinBox::PlusMinus) {
                        painter->save();
                        painter->setPen(QPen(color, 1.5));
                        const QPointF c = QRectF(rect).center();
                        painter->drawLine(QPointF(c.x() - 3, c.y()), QPointF(c.x() + 3, c.y()));
                        if (sub == SC_SpinBoxUp) {
                            painter->drawLine(QPointF(c.x(), c.y() - 3), QPointF(c.x(), c.y() + 3));
                        }
                        painter->restore();
                    } else {
                        drawChevron(painter, rect, sub == SC_SpinBoxUp ? Direction::Up : Direction::Down, color);
                    }
                }
            }
            return;
        }
        break;
    case CC_ScrollBar:
        if (const auto *bar = qstyleoption_cast<const QStyleOptionSlider *>(option)) {
            const QRect slider = proxy()->subControlRect(control, bar, SC_ScrollBarSlider, widget);
            if (slider.isValid() && bar->maximum != bar->minimum) {
                const bool active = bar->activeSubControls & SC_ScrollBarSlider;
                qreal alpha = 0.22;
                if (active && (bar->state & State_Sunken)) {
                    alpha = 0.5;
                } else if (active && (bar->state & State_MouseOver)) {
                    alpha = 0.36;
                }
                const QRect handle = bar->orientation == Qt::Horizontal ? slider.adjusted(1, 2, -1, -2) : slider.adjusted(2, 1, -2, -1);
                const qreal radius = qMin(handle.width(), handle.height()) / 2.;
                painter->save();
                painter->setRenderHint(QPainter::Antialiasing, true);
                painter->setPen(Qt::NoPen);
                painter->setBrush(overlay(option->palette, enabled ? alpha : 0.1));
                painter->drawRoundedRect(QRectF(handle), radius, radius);
                painter->restore();
            }
            return;
        }
        break;
    case CC_Slider:
        if (const auto *slider = qstyleoption_cast<const QStyleOptionSlider *>(option)) {
            const QRect groove = proxy()->subControlRect(control, slider, SC_SliderGroove, widget);
            const QRect handle = proxy()->subControlRect(control, slider, SC_SliderHandle, widget);
            const bool horizontal = slider->orientation == Qt::Horizontal;
            if (slider->subControls & SC_SliderTickmarks) {
                QStyleOptionSlider ticks(*slider);
                ticks.subControls = SC_SliderTickmarks;
                QProxyStyle::drawComplexControl(control, &ticks, painter, widget);
            }
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setPen(Qt::NoPen);
            if (slider->subControls & SC_SliderGroove) {
                const qreal thickness = 4;
                QRectF track = horizontal ? QRectF(groove.left(), groove.center().y() - thickness / 2 + 0.5, groove.width(), thickness)
                                          : QRectF(groove.center().x() - thickness / 2 + 0.5, groove.top(), thickness, groove.height());
                painter->setBrush(overlay(option->palette, 0.16));
                painter->drawRoundedRect(track, thickness / 2, thickness / 2);
                QRectF filled = track;
                const QPointF handleCenter = QRectF(handle).center();
                if (horizontal) {
                    (slider->upsideDown ? filled.setLeft(handleCenter.x()) : filled.setRight(handleCenter.x()));
                } else {
                    (slider->upsideDown ? filled.setBottom(handleCenter.y()) : filled.setTop(handleCenter.y()));
                }
                painter->setBrush(enabled ? option->palette.color(QPalette::Highlight) : overlay(option->palette, 0.25));
                painter->drawRoundedRect(filled, thickness / 2, thickness / 2);
            }
            if (slider->subControls & SC_SliderHandle) {
                const qreal diameter = qMin(qMin(handle.width(), handle.height()), 14);
                const QPointF c = QRectF(handle).center();
                const bool hover = enabled && (slider->activeSubControls & SC_SliderHandle) && (slider->state & State_MouseOver);
                painter->setBrush(enabled ? (hover ? option->palette.color(QPalette::BrightText) : option->palette.color(QPalette::WindowText))
                                          : overlay(option->palette, 0.35));
                painter->drawEllipse(c, diameter / 2, diameter / 2);
            }
            painter->restore();
            return;
        }
        break;
    default:
        break;
    }
    QProxyStyle::drawComplexControl(control, option, painter, widget);
}
