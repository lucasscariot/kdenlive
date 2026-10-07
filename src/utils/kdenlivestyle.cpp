/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "kdenlivestyle.h"
#include "designpaint.h"
#include "designtokens.h"

#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QCursor>
#include <QDialog>
#include <QEvent>
#include <QFrame>
#include <QIcon>
#include <QLinearGradient>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QStyleFactory>
#include <QStyleOption>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QWidget>

#include <tuple>

using DesignPaint::Direction;
using DesignPaint::withAlpha;

namespace {
QColor token(const char *name)
{
    return DesignTokens::color(QLatin1String(name));
}

int radius(const char *name)
{
    return DesignTokens::radius(QLatin1String(name));
}

int size(const char *name)
{
    return DesignTokens::size(QLatin1String(name));
}

QColor textColor(const QStyleOption *option)
{
    return option->state & QStyle::State_Enabled ? token("ink") : token("ink-tertiary");
}

bool hasFlag(const QWidget *widget, const char *property)
{
    return widget && widget->property(property).toBool();
}

/** @brief Input fields: line edits, spin boxes, editable combo boxes */
void drawInput(QPainter *painter, const QStyleOption *option, const QRect &rect)
{
    const bool enabled = option->state & QStyle::State_Enabled;
    QColor border = token("border-control");
    if (enabled && (option->state & QStyle::State_HasFocus)) {
        border = token("focus-ring");
    } else if (enabled && (option->state & QStyle::State_MouseOver)) {
        border = token("fill-thumb");
    }
    DesignPaint::panel(painter, rect, token("surface-control"), border, radius("radius-md"));
}

/** @brief Push buttons, non editable combo boxes and stand alone tool buttons */
void drawButton(QPainter *painter, const QStyleOption *option, const QRect &rect, bool primary)
{
    const bool enabled = option->state & QStyle::State_Enabled;
    const bool sunken = option->state & (QStyle::State_Sunken | QStyle::State_On);
    const bool hover = enabled && (option->state & QStyle::State_MouseOver);
    QColor fill = hover ? token("surface-control-hover") : token("surface-control");
    QColor border = token("border-control");
    if (primary) {
        fill = token("accent-fill");
        border = Qt::transparent;
        if (hover) {
            fill = fill.lighter(112);
        }
    }
    if (sunken) {
        fill = fill.darker(115);
    }
    if (!enabled) {
        fill = withAlpha(fill, DesignTokens::opacity(QStringLiteral("opacity-disabled")));
    }
    DesignPaint::panel(painter, rect, fill, border, radius("radius-md"));
}

/** @brief Accent filled buttons: the dialog default, or one explicitly marked as the primary action */
bool isPrimaryButton(const QStyleOptionButton *button, const QWidget *widget)
{
    if (hasFlag(widget, "_kdenlive_primary")) {
        return true;
    }
    // Icon only buttons (like file choosers) become default when focused, an accent square there is noise
    return (button->features & QStyleOptionButton::DefaultButton) && !button->text.isEmpty();
}

/** @brief Menu row metrics: control-sm rows, space-3 side padding, an icon-md column */
struct MenuMetrics
{
    int rowHeight = DesignTokens::size(QStringLiteral("control-sm"));
    int padding = DesignTokens::space(3);
    int iconColumn = DesignTokens::size(QStringLiteral("icon-md")) + DesignTokens::space(3);
    int shortcutGap = DesignTokens::space(5);
    int arrowColumn = DesignTokens::space(4);
    int inset = DesignTokens::space(1);
    int separatorHeight = DesignTokens::space(3) + 1;
};

/** @brief Inner geometry shared by every input and button, so all controls align on the spacing scale */
struct ControlMetrics
{
    int textInset = DesignTokens::space(3);
    int labelPadding = DesignTokens::space(4);
    int arrowColumn = DesignTokens::size(QStringLiteral("icon-md")) + DesignTokens::space(3);
    int menuColumn = DesignTokens::size(QStringLiteral("icon-md")) + 2 * DesignTokens::space(2);
    int spinButtonColumn = DesignTokens::size(QStringLiteral("icon-md"));
};

/** @brief Height of a pill tab, centered in its tab rectangle */
QRect pillRect(const QRect &tabRect)
{
    QRect pill(0, 0, tabRect.width() - 2 * DesignTokens::space(1), size("control-sm"));
    pill.moveCenter(tabRect.center());
    return pill;
}
} // namespace

void KdenliveStyle::polish(QWidget *widget)
{
    QProxyStyle::polish(widget);
    if (auto *menu = qobject_cast<QMenu *>(widget)) {
        // Rounded popups need a transparent window behind the panel's corners
        widget->setAttribute(Qt::WA_TranslucentBackground, true);
        widget->setFont(DesignTokens::font(QStringLiteral("text-body")));
        // A popup holds the pointer, so the button that opened it never sees the mouse leave: repaint it on close
        connect(menu, &QMenu::aboutToHide, menu, [menu]() {
            for (QWidget *top : QApplication::topLevelWidgets()) {
                for (QToolButton *button : top->findChildren<QToolButton *>()) {
                    if (button->menu() == menu) {
                        QTimer::singleShot(0, button, [button]() {
                            button->setAttribute(Qt::WA_UnderMouse, button->rect().contains(button->mapFromGlobal(QCursor::pos())));
                            button->update();
                        });
                    }
                }
            }
        });
    }
    if (widget->inherits("QTipLabel")) {
        // Tooltips: caption text in ink on a rounded raised panel
        widget->setAttribute(Qt::WA_TranslucentBackground, true);
        widget->setFont(DesignTokens::font(QStringLiteral("text-caption")));
        QPalette palette = widget->palette();
        palette.setColor(QPalette::ToolTipText, token("ink"));
        palette.setColor(QPalette::WindowText, token("ink"));
        palette.setColor(QPalette::ToolTipBase, token("surface-raised"));
        widget->setPalette(palette);
    }
    if (qobject_cast<QDialog *>(widget)) {
        widget->installEventFilter(this);
    }
}

void KdenliveStyle::unpolish(QWidget *widget)
{
    if (qobject_cast<QMenu *>(widget)) {
        widget->setAttribute(Qt::WA_TranslucentBackground, false);
    }
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
            painter.fillRect(dialog->rect(), token("surface-panel"));
            painter.setPen(token("border-control"));
            painter.drawRect(dialog->rect().adjusted(0, 0, -1, -1));
        }
    }
    return QProxyStyle::eventFilter(watched, event);
}

void KdenliveStyle::installIfFusion()
{
    if (QApplication::style()->name().compare(QLatin1String("fusion"), Qt::CaseInsensitive) != 0) {
        return;
    }
    QApplication::setStyle(new KdenliveStyle(QStyleFactory::create(QStringLiteral("fusion"))));
    QApplication::setFont(DesignTokens::font(QStringLiteral("text-body")));

    // Lucide based icons drawn in the theme's icon ink; names they do not cover fall back to Breeze
    QIcon::setThemeSearchPaths(QStringList{QStringLiteral(":/design/icons")} + QIcon::themeSearchPaths());
    const auto applyIconTheme = []() {
        QIcon::setThemeName(DesignTokens::instance()->isDark() ? QStringLiteral("KdenlivePro-dark") : QStringLiteral("KdenlivePro-light"));
    };
    applyIconTheme();
    QObject::connect(DesignTokens::instance(), &DesignTokens::themeChanged, qApp, applyIconTheme);
}

bool KdenliveStyle::isActive()
{
    return qobject_cast<KdenliveStyle *>(QApplication::style()) != nullptr;
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
    // Spacing steps: space-1 2px, space-2 4px, space-3 8px, space-4 16px
    case PM_LayoutLeftMargin:
    case PM_LayoutTopMargin:
    case PM_LayoutRightMargin:
    case PM_LayoutBottomMargin:
    case PM_LayoutHorizontalSpacing:
    case PM_LayoutVerticalSpacing:
    case PM_CheckBoxLabelSpacing:
    case PM_RadioButtonLabelSpacing:
    case PM_HeaderMargin:
        return DesignTokens::space(3);
    case PM_ButtonMargin:
        return DesignTokens::space(3) + DesignTokens::space(2);
    case PM_ToolBarItemSpacing:
    case PM_ToolBarItemMargin:
    case PM_MenuBarHMargin:
        return DesignTokens::space(2);
    case PM_MenuHMargin:
    case PM_MenuVMargin:
        return DesignTokens::space(1);
    case PM_MenuPanelWidth:
        return 1;
    case PM_SubMenuOverlap:
        return -DesignTokens::space(1);
    case PM_MenuBarItemSpacing:
    case PM_MenuBarVMargin:
        return DesignTokens::space(1);
    case PM_TabBarTabHSpace:
        return DesignTokens::space(4) + DesignTokens::space(3);
    case PM_ToolBarSeparatorExtent:
        return DesignTokens::space(3) + DesignTokens::space(2) + 1;
    case PM_ButtonShiftHorizontal:
    case PM_ButtonShiftVertical:
    case PM_TabBarTabShiftHorizontal:
    case PM_TabBarTabShiftVertical:
    case PM_TabBarBaseOverlap:
    case PM_ToolBarFrameWidth:
        return 0;
    case PM_ScrollBarExtent:
        return size("scrollbar");
    case PM_ScrollBarSliderMin:
        return 2 * DesignTokens::space(4);
    case PM_IndicatorWidth:
    case PM_IndicatorHeight:
    case PM_ExclusiveIndicatorWidth:
    case PM_ExclusiveIndicatorHeight:
    case PM_TabCloseIndicatorWidth:
    case PM_TabCloseIndicatorHeight:
        return size("indicator");
    case PM_SliderThickness:
    case PM_SliderLength:
        return size("slider-thumb") + 2;
    case PM_MenuButtonIndicator:
        // Wide enough to aim at the menu part of split buttons
        return size("icon-md") + DesignTokens::space(1);
    case PM_ToolTipLabelFrameWidth:
        return DesignTokens::space(2);
    case PM_DefaultFrameWidth:
    case PM_SpinBoxFrameWidth:
    case PM_ComboBoxFrameWidth:
        return 1;
    case PM_SmallIconSize:
    case PM_ToolBarIconSize:
    case PM_ButtonIconSize:
        return size("icon-md");
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
    case SH_UnderlineShortcut:
        return 0;
    case SH_Menu_SubMenuPopupDelay:
        return 120;
    case SH_Menu_Scrollable:
        return 1;
    default:
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }
}

QSize KdenliveStyle::sizeFromContents(ContentsType type, const QStyleOption *option, const QSize &contentsSize, const QWidget *widget) const
{
    QSize s = QProxyStyle::sizeFromContents(type, option, contentsSize, widget);
    switch (type) {
    case CT_PushButton:
        s.setHeight(qMax(s.height(), size("control-md")));
        break;
    case CT_ToolButton:
        s = s.expandedTo(QSize(size("control-sm"), size("control-sm")));
        if (hasFlag(widget, "_kdenlive_panel_toggle")) {
            // Same height as the primary button beside them
            s.rwidth() += 2 * DesignTokens::space(3);
            s.setHeight(size("control-md"));
        } else if (hasFlag(widget, "_kdenlive_primary")) {
            s.setHeight(qMax(s.height(), size("control-md")));
        }
        break;
    case CT_ComboBox:
    case CT_LineEdit:
    case CT_SpinBox:
        s.setHeight(qMax(s.height(), size("control-md")));
        break;
    case CT_HeaderSection:
        s.setHeight(qMax(s.height(), size("control-md")));
        break;
    case CT_TabBarTab:
        if (const auto *tab = qstyleoption_cast<const QStyleOptionTab *>(option)) {
            const bool vertical = tab->shape == QTabBar::RoundedWest || tab->shape == QTabBar::RoundedEast;
            if (!vertical) {
                // Room for the pill and space-2 above and below it
                s.setHeight(size("control-sm") + 2 * DesignTokens::space(2));
            }
        }
        break;
    case CT_MenuItem:
        if (const auto *item = qstyleoption_cast<const QStyleOptionMenuItem *>(option)) {
            const MenuMetrics m;
            if (item->menuItemType == QStyleOptionMenuItem::Separator) {
                // A section title carries text, a plain separator does not
                return QSize(s.width(), item->text.isEmpty() ? m.separatorHeight : m.rowHeight + DesignTokens::space(2));
            }
            const QString label = item->text.section(QLatin1Char('\t'), 0, 0);
            const QString shortcut = item->text.section(QLatin1Char('\t'), 1);
            const QFontMetrics labelMetrics(DesignTokens::font(QStringLiteral("text-body")));
            int width = 2 * (m.padding + m.inset) + m.iconColumn + labelMetrics.horizontalAdvance(label);
            if (!shortcut.isEmpty()) {
                width += m.shortcutGap + labelMetrics.horizontalAdvance(shortcut);
            }
            if (item->menuItemType == QStyleOptionMenuItem::SubMenu) {
                width += m.arrowColumn + DesignTokens::space(2);
            }
            return QSize(width, m.rowHeight + DesignTokens::space(1));
        }
        break;
    default:
        break;
    }
    return s;
}

QRect KdenliveStyle::subElementRect(SubElement element, const QStyleOption *option, const QWidget *widget) const
{
    if (element == SE_LineEditContents) {
        // Text starts space-3 inside the field, like every other input
        const ControlMetrics m;
        const auto *frame = qstyleoption_cast<const QStyleOptionFrame *>(option);
        const int inset = frame && frame->lineWidth > 0 ? m.textInset : 0;
        return option->rect.adjusted(inset, 1, -inset, -1);
    }
    if (element == SE_TabBarTearIndicatorLeft || element == SE_TabBarTearIndicatorRight) {
        // Wide fade where tabs are cut by the scroll buttons
        if (const auto *tab = qstyleoption_cast<const QStyleOptionTab *>(option)) {
            const bool vertical = tab->shape == QTabBar::RoundedWest || tab->shape == QTabBar::RoundedEast || tab->shape == QTabBar::TriangularWest ||
                                  tab->shape == QTabBar::TriangularEast;
            const int fade = DesignTokens::space(5) - DesignTokens::space(2);
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
    const ControlMetrics m;
    const QRect r = option->rect;
    if (control == CC_ComboBox) {
        // Label inset space-3, chevron in a fixed column on the right
        switch (subControl) {
        case SC_ComboBoxFrame:
            return r;
        case SC_ComboBoxArrow:
            return visualRect(option->direction, r, QRect(r.right() - m.arrowColumn + 1, r.top(), m.arrowColumn, r.height()));
        case SC_ComboBoxEditField:
            return visualRect(option->direction, r, QRect(r.left() + m.textInset, r.top() + 1, r.width() - m.textInset - m.arrowColumn, r.height() - 2));
        case SC_ComboBoxListBoxPopup:
            return r;
        default:
            break;
        }
    }
    if (control == CC_SpinBox) {
        if (const auto *spin = qstyleoption_cast<const QStyleOptionSpinBox *>(option)) {
            const bool buttons = spin->buttonSymbols != QAbstractSpinBox::NoButtons;
            const int column = buttons ? m.spinButtonColumn : 0;
            const QRect buttonArea(r.right() - column - DesignTokens::space(1) + 1, r.top() + 1, column, r.height() - 2);
            switch (subControl) {
            case SC_SpinBoxFrame:
                return r;
            case SC_SpinBoxUp:
                return buttons ? visualRect(option->direction, r, QRect(buttonArea.left(), buttonArea.top(), column, buttonArea.height() / 2)) : QRect();
            case SC_SpinBoxDown:
                return buttons ? visualRect(option->direction, r,
                                            QRect(buttonArea.left(), buttonArea.top() + buttonArea.height() / 2, column,
                                                  buttonArea.height() - buttonArea.height() / 2))
                               : QRect();
            case SC_SpinBoxEditField: {
                const int right = buttons ? buttonArea.left() - DesignTokens::space(1) : r.right() - m.textInset;
                return visualRect(option->direction, r, QRect(r.left() + m.textInset, r.top() + 1, right - r.left() - m.textInset + 1, r.height() - 2));
            }
            default:
                break;
            }
        }
    }
    if (control == CC_ToolButton && widget && widget->property("_kdenlive_primary").toBool()) {
        if (const auto *tool = qstyleoption_cast<const QStyleOptionToolButton *>(option)) {
            // Split primary button: the menu part is a fixed column, so drawing and hit areas match
            const bool split = tool->subControls & SC_ToolButtonMenu;
            const QRect menu(r.right() - m.menuColumn + 1, r.top(), m.menuColumn, r.height());
            if (subControl == SC_ToolButtonMenu) {
                return split ? visualRect(option->direction, r, menu) : QRect();
            }
            if (subControl == SC_ToolButton) {
                return split ? visualRect(option->direction, r, QRect(r.left(), r.top(), r.width() - m.menuColumn, r.height())) : r;
            }
        }
    }
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
    const bool hover = enabled && (option->state & State_MouseOver);
    switch (element) {
    case PE_PanelButtonTool: {
        QColor fill = DesignPaint::wash(hover, option->state & State_Sunken);
        if (option->state & State_On) {
            fill = token("accent-soft");
        }
        if (!(option->state & State_AutoRaise) && fill.alpha() == 0) {
            // Stand alone tool buttons still read as buttons
            drawButton(painter, option, option->rect, false);
            return;
        }
        DesignPaint::panel(painter, option->rect, fill, Qt::transparent, radius("radius-md"));
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
            // The text field inside a spin box or combo box shows its parent's surface, so the control reads as one
            if (widget && (qobject_cast<const QAbstractSpinBox *>(widget->parentWidget()) || qobject_cast<const QComboBox *>(widget->parentWidget()))) {
                return;
            }
        }
        break;
    case PE_FrameLineEdit:
        DesignPaint::panel(painter, option->rect, Qt::transparent, enabled && (option->state & State_HasFocus) ? token("focus-ring") : token("border-control"),
                           radius("radius-md"));
        return;
    case PE_IndicatorCheckBox:
    case PE_IndicatorItemViewItemCheck: {
        const int side = qMin(option->rect.width(), option->rect.height());
        QRect box(0, 0, side, side);
        box.moveCenter(option->rect.center());
        const bool on = option->state & (State_On | State_NoChange);
        QColor border = on ? token("accent-fill") : (hover ? token("ink-secondary") : token("ink-tertiary"));
        QColor fill = on ? token("accent-fill") : token("surface-control");
        if (!enabled) {
            const qreal disabled = DesignTokens::opacity(QStringLiteral("opacity-disabled"));
            border = withAlpha(border, disabled);
            fill = withAlpha(fill, disabled);
        }
        DesignPaint::panel(painter, box, fill, border, radius("radius-xs"));
        if (on) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setPen(QPen(token("on-accent"), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
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
        QRectF circle(0, 0, side, side);
        circle.moveCenter(QRectF(option->rect).center());
        const bool on = option->state & State_On;
        QColor border = on ? token("accent-fill") : (hover ? token("ink-secondary") : token("ink-tertiary"));
        QColor fill = on ? token("accent-fill") : token("surface-control");
        if (!enabled) {
            const qreal disabled = DesignTokens::opacity(QStringLiteral("opacity-disabled"));
            border = withAlpha(border, disabled);
            fill = withAlpha(fill, disabled);
        }
        DesignPaint::panel(painter, circle, fill, border, side / 2.);
        if (on) {
            const qreal dot = side * 0.36;
            QRectF center(0, 0, dot, dot);
            center.moveCenter(circle.center());
            DesignPaint::pill(painter, center, token("on-accent"));
        }
        return;
    }
    case PE_FrameFocusRect:
        // Item views show focus through selection; elsewhere the focus ring token
        if (qobject_cast<const QAbstractItemView *>(widget)) {
            return;
        }
        DesignPaint::panel(painter, option->rect, Qt::transparent, token("focus-ring"), radius("radius-sm"));
        return;
    case PE_IndicatorToolBarSeparator: {
        const QRect r = option->rect;
        const int inset = DesignTokens::space(3);
        if (option->state & State_Horizontal) {
            painter->fillRect(QRect(r.center().x(), r.top() + inset, 1, r.height() - 2 * inset), token("separator"));
        } else {
            painter->fillRect(QRect(r.left() + inset, r.center().y(), r.width() - 2 * inset, 1), token("separator"));
        }
        return;
    }
    case PE_IndicatorToolBarHandle:
    case PE_PanelToolBar:
    case PE_IndicatorButtonDropDown:
        return;
    case PE_FrameTabBarBase:
        painter->fillRect(QRect(option->rect.left(), option->rect.bottom(), option->rect.width(), 1), token("separator"));
        return;
    case PE_FrameTabWidget:
    case PE_FrameGroupBox:
        DesignPaint::panel(painter, option->rect, Qt::transparent, token("separator"), radius("radius-lg"));
        return;
    case PE_PanelTipLabel:
        DesignPaint::panel(painter, option->rect, token("surface-raised"), token("border-control"), radius("radius-md"));
        return;
    case PE_PanelMenu:
        if (qobject_cast<const QMenu *>(widget)) {
            // Rounded popover panel; the window behind it is transparent
            DesignPaint::panel(painter, option->rect, token("surface-raised"), token("border-control"), radius("radius-lg"));
            return;
        }
        painter->fillRect(option->rect, token("surface-raised"));
        return;
    case PE_FrameMenu:
        // The border belongs to the rounded panel
        return;
    case PE_IndicatorArrowDown:
    case PE_IndicatorArrowUp:
    case PE_IndicatorArrowLeft:
    case PE_IndicatorArrowRight: {
        const Direction direction = element == PE_IndicatorArrowDown   ? Direction::Down
                                    : element == PE_IndicatorArrowUp   ? Direction::Up
                                    : element == PE_IndicatorArrowLeft ? Direction::Left
                                                                       : Direction::Right;
        DesignPaint::chevron(painter, option->rect, direction, enabled ? token("ink-secondary") : token("ink-tertiary"));
        return;
    }
    case PE_IndicatorTabTearLeft:
    case PE_IndicatorTabTearRight: {
        // Fade the cut tab into the header instead of clipping it hard
        const QRect r = option->rect;
        const QColor header = token("surface-sidebar");
        QColor clear = header;
        clear.setAlpha(0);
        QLinearGradient gradient(r.topLeft(), r.topRight());
        gradient.setColorAt(element == PE_IndicatorTabTearLeft ? 0 : 1, header);
        gradient.setColorAt(element == PE_IndicatorTabTearLeft ? 1 : 0, clear);
        painter->fillRect(r, gradient);
        return;
    }
    case PE_IndicatorTabClose: {
        const bool closeHover = enabled && (option->state & (State_MouseOver | State_Raised));
        const bool current = option->state & State_Selected;
        if (!closeHover && !current && widget) {
            // Only the current tab and the hovered one show their close button
            if (const auto *bar = qobject_cast<const QTabBar *>(widget->parentWidget())) {
                const int hovered = bar->tabAt(bar->mapFromGlobal(QCursor::pos()));
                if (hovered < 0 || (bar->tabButton(hovered, QTabBar::RightSide) != widget && bar->tabButton(hovered, QTabBar::LeftSide) != widget)) {
                    return;
                }
            }
        }
        const int side = size("indicator");
        QRect box(0, 0, side, side);
        box.moveCenter(option->rect.center());
        const QColor ink = current ? token("on-accent") : token("ink");
        if (closeHover) {
            DesignPaint::pill(painter, box, withAlpha(ink, option->state & State_Sunken ? 0.25 : 0.16));
        }
        const QColor color = withAlpha(ink, closeHover ? 1.0 : (current ? 0.75 : 0.45));
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
    const bool enabled = option->state & State_Enabled;
    const bool hover = enabled && (option->state & State_MouseOver);
    switch (element) {
    case CE_PushButtonBevel:
        if (const auto *button = qstyleoption_cast<const QStyleOptionButton *>(option)) {
            if (hasFlag(widget, "_kdenlive_pagebar")) {
                // Page bar: an accent underline marks the current page, hover gets a wash
                const QRect r = option->rect;
                if (hover) {
                    DesignPaint::panel(painter, r.adjusted(0, DesignTokens::space(1), 0, -DesignTokens::space(1)), DesignPaint::wash(true, false),
                                       Qt::transparent, radius("radius-md"));
                }
                if (option->state & State_On) {
                    const int inset = DesignTokens::space(3) + DesignTokens::space(1);
                    painter->fillRect(QRect(r.left() + inset, r.bottom() - 1, r.width() - 2 * inset, 2), token("accent"));
                }
                return;
            }
            if (hasFlag(widget, "_kdenlive_segmented")) {
                // Segmented control: the current segment is raised, the others only react to hover
                if (option->state & State_On) {
                    DesignPaint::panel(painter, option->rect, token("surface-raised"), token("border-control"), radius("radius-sm"));
                } else if (hover) {
                    DesignPaint::panel(painter, option->rect, DesignPaint::wash(true, false), Qt::transparent, radius("radius-sm"));
                }
                return;
            }
            const bool flat = button->features & QStyleOptionButton::Flat;
            if (!flat || (option->state & (State_Sunken | State_On | State_MouseOver))) {
                drawButton(painter, option, option->rect, isPrimaryButton(button, widget));
            }
            if (button->features & QStyleOptionButton::HasMenu) {
                const int indicator = proxy()->pixelMetric(PM_MenuButtonIndicator, option, widget);
                const QRect arrow(option->rect.right() - indicator - DesignTokens::space(2), option->rect.top(), indicator, option->rect.height());
                DesignPaint::chevron(painter, arrow, Direction::Down, isPrimaryButton(button, widget) ? token("on-accent") : textColor(option));
            }
            return;
        }
        break;
    case CE_PushButtonLabel:
        if (const auto *button = qstyleoption_cast<const QStyleOptionButton *>(option)) {
            QStyleOptionButton label(*button);
            if ((hasFlag(widget, "_kdenlive_segmented") || hasFlag(widget, "_kdenlive_pagebar")) && !(option->state & State_On)) {
                label.palette.setColor(QPalette::ButtonText, token("ink-secondary"));
            } else if (isPrimaryButton(button, widget) && enabled) {
                label.palette.setColor(QPalette::ButtonText, token("on-accent"));
                if (!button->icon.isNull()) {
                    // The icon follows the label onto the accent fill
                    const qreal dpr = widget ? widget->devicePixelRatioF() : 1.0;
                    label.icon = QIcon(DesignPaint::tinted(button->icon, button->iconSize, token("on-accent"), dpr));
                }
            } else {
                label.palette.setColor(QPalette::ButtonText, textColor(option));
            }
            QProxyStyle::drawControl(element, &label, painter, widget);
            return;
        }
        break;
    case CE_ToolBar:
        return;
    case CE_MenuBarEmptyArea:
        painter->fillRect(option->rect, token("surface-window"));
        return;
    case CE_MenuBarItem:
        if (const auto *item = qstyleoption_cast<const QStyleOptionMenuItem *>(option)) {
            QStyleOptionMenuItem plain(*item);
            const bool active = (option->state & State_Selected) && enabled;
            plain.state &= ~(State_Selected | State_Sunken);
            plain.palette.setColor(QPalette::ButtonText, token("ink"));
            plain.palette.setColor(QPalette::WindowText, token("ink"));
            painter->fillRect(option->rect, token("surface-window"));
            if (active) {
                DesignPaint::panel(painter, option->rect.adjusted(0, DesignTokens::space(1), 0, -DesignTokens::space(1)),
                                   DesignPaint::wash(true, option->state & State_Sunken), Qt::transparent, radius("radius-sm"));
            }
            QCommonStyle::drawControl(element, &plain, painter, widget);
            return;
        }
        break;
    case CE_ShapedFrame:
        if (const auto *frame = qstyleoption_cast<const QStyleOptionFrame *>(option)) {
            // Every QFrame line and box in the app: separator hairlines instead of Fusion's text colored or bevelled frames
            const QRect r = frame->rect;
            switch (frame->frameShape) {
            case QFrame::HLine:
                painter->fillRect(QRect(r.left(), r.center().y(), r.width(), 1), token("separator"));
                return;
            case QFrame::VLine:
                painter->fillRect(QRect(r.center().x(), r.top(), 1, r.height()), token("separator"));
                return;
            case QFrame::Box:
            case QFrame::Panel:
            case QFrame::StyledPanel:
            case QFrame::WinPanel:
                if (frame->lineWidth > 0) {
                    DesignPaint::panel(painter, r, Qt::transparent, token("separator"), radius("radius-sm"));
                }
                return;
            default:
                break;
            }
        }
        break;
    case CE_MenuEmptyArea:
        return;
    case CE_MenuItem:
        if (const auto *item = qstyleoption_cast<const QStyleOptionMenuItem *>(option)) {
            const MenuMetrics m;
            const QRect row = item->rect.adjusted(m.inset, 0, -m.inset, 0);
            if (item->menuItemType == QStyleOptionMenuItem::Separator) {
                if (item->text.isEmpty()) {
                    painter->fillRect(QRect(row.left() + m.padding, row.center().y(), row.width() - 2 * m.padding, 1), token("separator"));
                } else {
                    // Section title: caption in secondary ink
                    painter->save();
                    painter->setFont(DesignTokens::font(QStringLiteral("text-caption")));
                    painter->setPen(token("ink-secondary"));
                    painter->drawText(row.adjusted(m.padding, DesignTokens::space(2), -m.padding, 0), Qt::AlignLeft | Qt::AlignVCenter, item->text);
                    painter->restore();
                }
                return;
            }
            const bool selected = (item->state & State_Selected) && enabled;
            if (selected) {
                DesignPaint::panel(painter, row.adjusted(0, 1, 0, -1), token("accent-fill"), Qt::transparent, radius("radius-sm"));
            }
            const QColor ink = !enabled ? token("ink-tertiary") : (selected ? token("on-accent") : token("ink"));
            const QColor secondaryInk = !enabled ? token("ink-tertiary") : (selected ? token("on-accent") : token("ink-tertiary"));
            QRect content = row.adjusted(m.padding, 0, -m.padding, 0);

            // Icon column: the icon, or a check mark for checked items without one
            const int iconSize = DesignTokens::size(QStringLiteral("icon-md"));
            QRect iconRect(content.left(), content.top() + (content.height() - iconSize) / 2, iconSize, iconSize);
            const bool checked = item->checkType != QStyleOptionMenuItem::NotCheckable && item->checked;
            if (!item->icon.isNull()) {
                if (checked) {
                    DesignPaint::panel(painter, iconRect.adjusted(-2, -2, 2, 2),
                                       selected ? DesignPaint::withAlpha(token("on-accent"), 0.25) : token("accent-soft"), Qt::transparent,
                                       radius("radius-xs"));
                }
                if (selected) {
                    // White icon on the accent row, like the label beside it
                    const qreal dpr = widget ? widget->devicePixelRatioF() : 1.0;
                    painter->drawPixmap(iconRect, DesignPaint::tinted(item->icon, iconRect.size(), token("on-accent"), dpr));
                } else {
                    item->icon.paint(painter, iconRect, Qt::AlignCenter, enabled ? QIcon::Normal : QIcon::Disabled, checked ? QIcon::On : QIcon::Off);
                }
            } else if (checked) {
                painter->save();
                painter->setRenderHint(QPainter::Antialiasing, true);
                const QColor checkInk = selected ? token("on-accent") : (enabled ? token("accent") : token("ink-tertiary"));
                if (item->checkType == QStyleOptionMenuItem::Exclusive) {
                    QRectF dot(0, 0, 6, 6);
                    dot.moveCenter(QRectF(iconRect).center());
                    painter->setPen(Qt::NoPen);
                    painter->setBrush(checkInk);
                    painter->drawEllipse(dot);
                } else {
                    painter->setPen(QPen(checkInk, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                    const QRectF r(iconRect.adjusted(2, 2, -2, -2));
                    QPainterPath check;
                    check.moveTo(r.left() + r.width() * 0.15, r.top() + r.height() * 0.55);
                    check.lineTo(r.left() + r.width() * 0.4, r.top() + r.height() * 0.8);
                    check.lineTo(r.left() + r.width() * 0.88, r.top() + r.height() * 0.25);
                    painter->drawPath(check);
                }
                painter->restore();
            }
            content.setLeft(content.left() + m.iconColumn);

            // Submenu chevron on the right
            if (item->menuItemType == QStyleOptionMenuItem::SubMenu) {
                const QRect arrow(content.right() - m.arrowColumn + 1, content.top(), m.arrowColumn, content.height());
                DesignPaint::chevron(painter, arrow, Direction::Right, secondaryInk);
                content.setRight(arrow.left() - DesignTokens::space(2));
            }

            // Label on the left, shortcut right aligned in tertiary ink
            const QString label = item->text.section(QLatin1Char('\t'), 0, 0);
            const QString shortcut = item->text.section(QLatin1Char('\t'), 1);
            painter->save();
            painter->setFont(DesignTokens::font(QStringLiteral("text-body")));
            const int textFlags = Qt::AlignVCenter | Qt::TextSingleLine | Qt::TextHideMnemonic;
            if (!shortcut.isEmpty()) {
                painter->setPen(secondaryInk);
                painter->drawText(content, textFlags | Qt::AlignRight, shortcut);
                content.setRight(content.right() - painter->fontMetrics().horizontalAdvance(shortcut) - m.shortcutGap);
            }
            painter->setPen(ink);
            painter->drawText(content, textFlags | Qt::AlignLeft,
                              painter->fontMetrics().elidedText(label, Qt::ElideRight, content.width(), Qt::TextHideMnemonic));
            painter->restore();
            return;
        }
        break;
    case CE_HeaderSection: {
        const QRect r = option->rect;
        painter->fillRect(r, token("surface-sidebar"));
        painter->fillRect(QRect(r.left(), r.bottom(), r.width(), 1), token("separator"));
        painter->fillRect(QRect(r.right(), r.top() + DesignTokens::space(2), 1, r.height() - 2 * DesignTokens::space(2)), token("separator"));
        return;
    }
    case CE_HeaderLabel:
        if (const auto *header = qstyleoption_cast<const QStyleOptionHeader *>(option)) {
            QStyleOptionHeader label(*header);
            label.palette.setColor(QPalette::ButtonText, token("ink-secondary"));
            painter->save();
            painter->setFont(DesignTokens::font(QStringLiteral("text-caption")));
            QProxyStyle::drawControl(element, &label, painter, widget);
            painter->restore();
            return;
        }
        break;
    case CE_HeaderEmptyArea:
        DesignPaint::headerStrip(painter, option->rect);
        return;
    case CE_TabBarTabShape:
        if (const auto *tab = qstyleoption_cast<const QStyleOptionTab *>(option)) {
            // Pill tabs: the current one is filled with the accent, others only react to hover
            const QRect pill = pillRect(tab->rect);
            if (tab->state & State_Selected) {
                DesignPaint::pill(painter, pill, enabled ? token("accent-fill") : withAlpha(token("accent-fill"), 0.5));
            } else if (hover) {
                DesignPaint::pill(painter, pill, DesignPaint::wash(true, false));
            }
            return;
        }
        break;
    case CE_TabBarTabLabel:
        if (const auto *tab = qstyleoption_cast<const QStyleOptionTab *>(option)) {
            QStyleOptionTab label(*tab);
            const bool selected = tab->state & State_Selected;
            label.palette.setColor(QPalette::WindowText, selected ? token("on-accent") : token("ink-secondary"));
            painter->save();
            QFont font = painter->font();
            font.setWeight(selected ? QFont::DemiBold : QFont::Normal);
            painter->setFont(font);
            QProxyStyle::drawControl(element, &label, painter, widget);
            painter->restore();
            return;
        }
        break;
    case CE_Splitter:
        painter->fillRect(option->rect, token("surface-window"));
        if (option->state & State_Horizontal) {
            painter->fillRect(QRect(option->rect.center().x(), option->rect.top(), 1, option->rect.height()), token("separator"));
        } else {
            painter->fillRect(QRect(option->rect.left(), option->rect.center().y(), option->rect.width(), 1), token("separator"));
        }
        return;
    case CE_ProgressBarGroove:
        DesignPaint::pill(painter, option->rect, token("fill-track"));
        return;
    case CE_ProgressBarContents:
        if (const auto *bar = qstyleoption_cast<const QStyleOptionProgressBar *>(option)) {
            if (bar->maximum > bar->minimum) {
                QRect filled = bar->rect;
                filled.setWidth(int(qint64(filled.width()) * (bar->progress - bar->minimum) / (bar->maximum - bar->minimum)));
                DesignPaint::pill(painter, filled, token("accent-fill"));
            }
            return;
        }
        break;
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
            const bool hover = enabled && (tool->state & State_MouseOver);
            const bool pressed = tool->state & State_Sunken;
            if (widget && qobject_cast<const QTabBar *>(widget->parentWidget()) && tool->arrowType != Qt::NoArrow) {
                // Tab bar scroll buttons sit on top of the tabs: give them the header background and a bare chevron
                painter->fillRect(tool->rect, token("surface-sidebar"));
                if (hover || pressed) {
                    DesignPaint::panel(painter, tool->rect.adjusted(1, DesignTokens::space(2), -1, -DesignTokens::space(2)), DesignPaint::wash(hover, pressed),
                                       Qt::transparent, radius("radius-sm"));
                }
                const Direction direction = tool->arrowType == Qt::LeftArrow    ? Direction::Left
                                            : tool->arrowType == Qt::RightArrow ? Direction::Right
                                            : tool->arrowType == Qt::UpArrow    ? Direction::Up
                                                                                : Direction::Down;
                DesignPaint::chevron(painter, tool->rect, direction, enabled ? token("ink-secondary") : token("ink-tertiary"));
                return;
            }
            const QRect button = proxy()->subControlRect(control, tool, SC_ToolButton, widget);
            const QRect menu = proxy()->subControlRect(control, tool, SC_ToolButtonMenu, widget);
            const bool hasMenuPart = tool->subControls & SC_ToolButtonMenu;

            if (hasFlag(widget, "_kdenlive_primary")) {
                // The primary action of a bar: accent fill, bold label, the menu part behind a divider.
                // Hover and press only light the part under the pointer; an open menu keeps its part pressed.
                const QColor base = enabled ? token("accent-fill") : withAlpha(token("accent-fill"), DesignTokens::opacity(QStringLiteral("opacity-disabled")));
                DesignPaint::panel(painter, tool->rect, base, Qt::transparent, radius("radius-md"));
                const bool menuOpen = hasMenuPart && (tool->state & State_On);
                const bool overMenu = hasMenuPart && (tool->activeSubControls & SC_ToolButtonMenu);
                if (enabled && (hover || pressed || menuOpen)) {
                    const bool partPressed = pressed || menuOpen;
                    const QColor overlayColor = partPressed ? withAlpha(token("surface-viewer"), 0.22) : withAlpha(token("on-accent"), 0.12);
                    painter->save();
                    QPainterPath shape;
                    shape.addRoundedRect(QRectF(tool->rect), radius("radius-md"), radius("radius-md"));
                    painter->setClipPath(shape);
                    painter->fillRect(hasMenuPart ? ((overMenu || menuOpen) ? menu : button) : tool->rect, overlayColor);
                    painter->restore();
                }
                const QColor onAccent = token("on-accent");
                if (hasMenuPart) {
                    const int inset = DesignTokens::space(3) - DesignTokens::space(1);
                    painter->fillRect(QRect(menu.left(), menu.top() + inset, 1, menu.height() - 2 * inset), withAlpha(onAccent, 0.35));
                    DesignPaint::chevron(painter, menu, Direction::Down, onAccent);
                }
                QStyleOptionToolButton label(*tool);
                label.state &= ~(State_Sunken | State_On | State_MouseOver);
                label.toolButtonStyle = Qt::ToolButtonTextOnly;
                label.font = DesignTokens::font(QStringLiteral("text-body-strong"));
                label.palette.setColor(QPalette::ButtonText, onAccent);
                label.rect = button;
                proxy()->drawControl(CE_ToolButtonLabel, &label, painter, widget);
                return;
            }

            if (hasFlag(widget, "_kdenlive_panel_toggle")) {
                // Panel toggles, as in Final Cut: the visible pane shows an accent icon and a bright label;
                // hover only adds a wash, so the two states never look alike
                const bool on = tool->state & State_On;
                const QColor fill = DesignPaint::wash(hover, pressed);
                if (fill.alpha() > 0) {
                    DesignPaint::panel(painter, tool->rect, fill, Qt::transparent, radius("radius-md"));
                }
                QStyleOptionToolButton label(*tool);
                label.state &= ~(State_Sunken | State_On);
                label.palette.setColor(QPalette::ButtonText, on || hover ? token("ink") : token("ink-secondary"));
                if (on && !tool->icon.isNull()) {
                    const qreal dpr = widget ? widget->devicePixelRatioF() : 1.0;
                    label.icon = QIcon(DesignPaint::tinted(tool->icon, tool->iconSize, token("accent"), dpr));
                }
                // Center icon and text as one group, so equal width toggles stay balanced
                const int iconWidth = tool->icon.isNull() ? 0 : tool->iconSize.width() + DesignTokens::space(2);
                const int contentWidth = qMin(tool->rect.width(), iconWidth + tool->fontMetrics.horizontalAdvance(tool->text) + 2);
                label.rect = QRect(tool->rect.left() + (tool->rect.width() - contentWidth) / 2, tool->rect.top(), contentWidth, tool->rect.height());
                proxy()->drawControl(CE_ToolButtonLabel, &label, painter, widget);
                return;
            }

            State flags = tool->state & ~State_Sunken;
            if ((flags & State_AutoRaise) && (!(flags & State_MouseOver) || !enabled)) {
                flags &= ~State_Raised;
            }
            if (pressed && (tool->activeSubControls & (SC_ToolButton | SC_ToolButtonMenu))) {
                flags |= State_Sunken;
            }
            QStyleOption panel(*tool);
            // One panel for button and menu part, so split buttons read as a single control
            panel.rect = hasMenuPart ? tool->rect : button;
            panel.state = flags;
            proxy()->drawPrimitive(PE_PanelButtonTool, &panel, painter, widget);
            if (hasMenuPart) {
                DesignPaint::chevron(painter, menu, Direction::Down, enabled ? token("ink-secondary") : token("ink-tertiary"));
            } else if (tool->features & QStyleOptionToolButton::HasMenu) {
                const int indicator = DesignTokens::space(3) - DesignTokens::space(1);
                const QRect arrow(tool->rect.right() - indicator - 1, tool->rect.bottom() - indicator - 1, indicator, indicator);
                DesignPaint::chevron(painter, arrow, Direction::Down, token("ink-secondary"));
            }
            QStyleOptionToolButton label(*tool);
            label.state = flags & ~State_Sunken;
            label.palette.setColor(QPalette::ButtonText, textColor(option));
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
            DesignPaint::chevron(painter, arrow, Direction::Down, enabled ? token("ink-secondary") : token("ink-tertiary"));
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
                for (const auto &[rect, sub, stepEnabled] : {std::tuple{up, SC_SpinBoxUp, bool(spin->stepEnabled & QAbstractSpinBox::StepUpEnabled)},
                                                             std::tuple{down, SC_SpinBoxDown, bool(spin->stepEnabled & QAbstractSpinBox::StepDownEnabled)}}) {
                    if (enabled && stepEnabled && (spin->activeSubControls & sub) && (spin->state & (State_MouseOver | State_Sunken))) {
                        DesignPaint::panel(painter, rect.adjusted(1, 1, -1, -1), DesignPaint::wash(true, spin->state & State_Sunken), Qt::transparent,
                                           radius("radius-xs"));
                    }
                    const QColor color = stepEnabled && enabled ? token("ink-secondary") : token("ink-tertiary");
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
                        DesignPaint::chevron(painter, rect, sub == SC_SpinBoxUp ? Direction::Up : Direction::Down, color);
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
                const bool active = (bar->activeSubControls & SC_ScrollBarSlider) && (bar->state & (State_Sunken | State_MouseOver));
                const int inset = DesignTokens::space(1);
                const QRect handle = bar->orientation == Qt::Horizontal ? slider.adjusted(1, inset, -1, -inset) : slider.adjusted(inset, 1, -inset, -1);
                QColor fill = active ? token("fill-thumb-hover") : token("fill-thumb");
                if (!enabled) {
                    fill = withAlpha(fill, DesignTokens::opacity(QStringLiteral("opacity-disabled")));
                }
                DesignPaint::pill(painter, handle, fill);
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
            if (slider->subControls & SC_SliderGroove) {
                const qreal thickness = size("slider-track");
                const QRectF track = horizontal ? QRectF(groove.left(), groove.center().y() - thickness / 2 + 0.5, groove.width(), thickness)
                                                : QRectF(groove.center().x() - thickness / 2 + 0.5, groove.top(), thickness, groove.height());
                DesignPaint::pill(painter, track, token("fill-track"));
                QRectF filled = track;
                const QPointF handleCenter = QRectF(handle).center();
                if (horizontal) {
                    (slider->upsideDown ? filled.setLeft(handleCenter.x()) : filled.setRight(handleCenter.x()));
                } else {
                    (slider->upsideDown ? filled.setBottom(handleCenter.y()) : filled.setTop(handleCenter.y()));
                }
                DesignPaint::pill(painter, filled, enabled ? token("accent-fill") : token("fill-thumb"));
            }
            if (slider->subControls & SC_SliderHandle) {
                const qreal diameter = qMin<qreal>(qMin(handle.width(), handle.height()), size("slider-thumb"));
                QRectF knob(0, 0, diameter, diameter);
                knob.moveCenter(QRectF(handle).center());
                DesignPaint::panel(painter, knob, enabled ? token("ink") : token("ink-tertiary"), token("border-control"), diameter / 2.);
            }
            return;
        }
        break;
    default:
        break;
    }
    QProxyStyle::drawComplexControl(control, option, painter, widget);
}
