/*
    SPDX-FileCopyrightText: 2025 Jean-Baptiste Mardelle <jb@kdenlive.org>

SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "kddocksetup.h"
#include "core.h"
#include "kdenlivesettings.h"
#include "utils/designpaint.h"
#include "utils/designtokens.h"
#include "utils/kdenlivestyle.h"

#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QHoverEvent>
#include <QMimeData>
#include <QObject>
#include <QTabBar>

#include <QAbstractButton>
#include <QChildEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleOptionTab>

#include <functional>

class DockTabDragHandler : public QObject
{
public:
    explicit DockTabDragHandler(KDDockWidgets::Core::DockWidget *coreDock,
                                KDDockWidgets::Core::TabBar *coreTabBar,
                                KDDockWidgets::QtWidgets::TabBar *qtTabBar,
                                std::function<void()> onDragEnteredTab = {},
                                std::function<bool(const QMimeData*)> mimeHandler = {},
                                QObject *parent = nullptr)
        : QObject(parent)
        , m_coreDock(coreDock)
        , m_coreTabBar(coreTabBar)
        , m_qtTabBar(qtTabBar)
        , m_onDragEnteredTab(std::move(onDragEnteredTab))
        , m_mimeHandler(std::move(mimeHandler))
    {
        qtTabBar->setAcceptDrops(true);
        qtTabBar->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *obj, QEvent *event) override
    {
        if (obj != m_qtTabBar || !m_coreTabBar) {
            return QObject::eventFilter(obj, event);
        }

        switch (event->type()) {
            case QEvent::DragEnter: {
                auto *e = static_cast<QDragEnterEvent *>(event);
                e->accept();
                return true;
            }
            case QEvent::DragMove: {
                auto *e = static_cast<QDragMoveEvent *>(event);
    const int idx = m_qtTabBar->tabAt(e->position().toPoint());
                if (idx >= 0) {
                    if (m_mimeHandler && m_mimeHandler(e->mimeData()) && m_coreTabBar->dockWidgetAt(idx) == m_coreDock && m_onDragEnteredTab) {
                        m_onDragEnteredTab();
                    }
                    e->ignore();
                }
                return true;
                break;
            }
            default:
                break;
        }

        return QObject::eventFilter(obj, event);
    }

private:
    KDDockWidgets::Core::DockWidget *m_coreDock = nullptr;
    KDDockWidgets::Core::TabBar *m_coreTabBar = nullptr;
    KDDockWidgets::QtWidgets::TabBar *m_qtTabBar = nullptr;
    std::function<void()> m_onDragEnteredTab;
    std::function<bool(const QMimeData*)> m_mimeHandler;
};

/** @brief Disables animations on dock tab bars; drawing comes from the application style */
class DockTabStyle : public QProxyStyle
{
public:
    using QProxyStyle::QProxyStyle;

    int styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget, QStyleHintReturn *returnData) const override
    {
        // Same as the KDDockWidgets style we replace: animations glitch while dragging tabs
        if (hint == QStyle::SH_Widget_Animation_Duration) {
            return 0;
        }
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }
};

/** @brief Fill the panel header strip behind tabs and title bars */
static void paintHeaderStrip(QWidget *widget, const QRect &rect)
{
    QPainter p(widget);
    DesignPaint::headerStrip(&p, rect);
}

class KdenliveDockTabBar : public KDDockWidgets::QtWidgets::TabBar
{
public:
    explicit KdenliveDockTabBar(KDDockWidgets::Core::TabBar *controller, KDDockWidgets::Core::View *parent = nullptr)
        : KDDockWidgets::QtWidgets::TabBar(controller, KDDockWidgets::QtCommon::View_qt::asQWidget(parent))
    {
        auto parentWidget = KDDockWidgets::QtCommon::View_qt::asQWidget(parent);
        setProperty("_breeze_force_frame", false);
        setDocumentMode(true);
        parentWidget->setProperty("_breeze_force_frame", false);
        setContextMenuPolicy(Qt::CustomContextMenu);
        // The constructor of KDDockWidgets::QtWidgets::TabBar makes a QProxyStyle
        // that ends up taking ownership of the style for the entire application!
        if (QProxyStyle *proxy_style = qobject_cast<QProxyStyle *>(style())) {
            proxy_style->baseStyle()->setParent(qApp);
            proxy_style->setBaseStyle(KdenliveStyle::cloneApplicationStyle());
        }
        auto *tabStyle = new DockTabStyle(KdenliveStyle::cloneApplicationStyle());
        tabStyle->setParent(this);
        setStyle(tabStyle);
        setPalette(qApp->palette());
        setMaximumHeight(0);
        setFont(DesignTokens::font(QStringLiteral("text-caption")));

        connect(this, &QWidget::customContextMenuRequested, []() { Q_EMIT pCore.get()->switchTitleBars(); });
        connect(this, &KDDockWidgets::QtWidgets::TabBar::countChanged, [&]() {
            if (!KdenliveSettings::showtitlebars()) {
                pCore->startHideBarsTimer();
            }
        });
        connect(this, &KDDockWidgets::QtWidgets::TabBar::dockWidgetInserted, [&, this, controller](int index) {
            auto dock = controller->dockWidgetAt(index);
            if (dock && dock->view() && dock->uniqueName() == QStringLiteral("project_bin")) {
                KDDockWidgets::QtWidgets::DockWidget *qtDock = dynamic_cast<KDDockWidgets::QtWidgets::DockWidget *>(dock->view());
                new DockTabDragHandler(dock, controller, this, [qtDock]() {
                    qtDock->open();
                    qtDock->setAsCurrentTab();
                    qtDock->setFocus(Qt::OtherFocusReason);
                }, 
                [](const QMimeData *mime)
                {
                    return mime && (mime->hasFormat("application/x-kdenlive-clip") || mime->hasUrls() || mime->hasText());
                },
                this);
            }
        });
    }

    // Panes have no visible tabs: switching happens from the panel top bar
    QSize sizeHint() const override { return {KDDockWidgets::QtWidgets::TabBar::sizeHint().width(), 0}; }
    QSize minimumSizeHint() const override { return {0, 0}; }

protected:
    void paintEvent(QPaintEvent *) override {}

    bool event(QEvent *event) override
    {
        // Close buttons only show on the current and hovered tabs, repaint them when the hovered tab changes
        if (event->type() == QEvent::HoverMove || event->type() == QEvent::HoverLeave) {
            const int hovered = event->type() == QEvent::HoverLeave ? -1 : tabAt(static_cast<QHoverEvent *>(event)->position().toPoint());
            if (hovered != m_hoveredTab) {
                m_hoveredTab = hovered;
                for (int i = 0; i < count(); ++i) {
                    if (QWidget *button = tabButton(i, QTabBar::RightSide)) {
                        button->update();
                    }
                }
            }
        }
        return KDDockWidgets::QtWidgets::TabBar::event(event);
    }

private:
    int m_hoveredTab{-1};
};

class KdenliveDockGroup : public KDDockWidgets::QtWidgets::Group
{
public:
    explicit KdenliveDockGroup(KDDockWidgets::Core::Group *controller, KDDockWidgets::Core::View *parent = nullptr)
        : KDDockWidgets::QtWidgets::Group(controller, KDDockWidgets::QtCommon::View_qt::asQWidget(parent))
        , m_controller(controller)
    {
    }
    void paintEvent(QPaintEvent *) override
    {
        // The tab bar may be narrower than the panel, extend its header strip to the full width
        if (!m_controller->tabBar() || !m_controller->tabBar()->view()) {
            return;
        }
        QWidget *tabBar = KDDockWidgets::QtCommon::View_qt::asQWidget(m_controller->tabBar()->view());
        if (tabBar && tabBar->isVisible()) {
            const QRect bar(tabBar->mapTo(this, QPoint(0, 0)), tabBar->size());
            paintHeaderStrip(this, QRect(0, bar.top(), width(), bar.height()));
        }
    }

private:
    KDDockWidgets::Core::Group *const m_controller;
};

class KdenliveDockStack : public KDDockWidgets::QtWidgets::Stack
{
public:
    explicit KdenliveDockStack(KDDockWidgets::Core::Stack *controller, KDDockWidgets::Core::View *parent = nullptr)
        : KDDockWidgets::QtWidgets::Stack(controller, KDDockWidgets::QtCommon::View_qt::asQWidget(parent))
    {
    }
    void paintEvent(QPaintEvent *) override {}
};

/** @brief Repaints KDDockWidgets title bar buttons as flat tool buttons: bare icon, rounded hover background */
class TitleButtonPainter : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() != QEvent::Paint) {
            return QObject::eventFilter(watched, event);
        }
        auto *button = qobject_cast<QAbstractButton *>(watched);
        if (!button) {
            return QObject::eventFilter(watched, event);
        }
        QPainter p(button);
        if (button->isEnabled() && (button->underMouse() || button->isDown())) {
            DesignPaint::panel(&p, QRectF(button->rect()).adjusted(1, 1, -1, -1), DesignPaint::wash(true, button->isDown()), Qt::transparent,
                               DesignTokens::radius(QStringLiteral("radius-sm")));
        }
        const int iconSize = DesignTokens::size(QStringLiteral("icon-md"));
        QRect iconRect(0, 0, iconSize, iconSize);
        iconRect.moveCenter(button->rect().center());
        button->icon().paint(&p, iconRect, Qt::AlignCenter, button->isEnabled() ? QIcon::Normal : QIcon::Disabled);
        return true;
    }
};

class KdenliveDockTitleBar : public KDDockWidgets::QtWidgets::TitleBar
{
public:
    explicit KdenliveDockTitleBar(KDDockWidgets::Core::TitleBar *controller, KDDockWidgets::Core::View *parent = nullptr)
        : KDDockWidgets::QtWidgets::TitleBar(controller, parent)
        , m_controller(controller)
    {
        // Panes have no title bar; panes cannot float either, since dragging is disabled
        setMaximumHeight(0);
        connect(pCore.get(), &Core::hideBars, this, [this](bool hide) {
            if (hide) {
#if defined(Q_OS_WIN)
                auto parentWidget = m_controller->view()->parentView();
                if (parentWidget && parentWidget->asFloatingWindowController() != nullptr) {
                    // Floating window, don't show as we already have the widget titlebar
                    return;
                }
#endif
            } else {
                if (m_controller->dockWidgets().size() > 1) {
                    // Don't show title bar when there are tabbed widgets
                    return;
                }
                auto parentWidget = m_controller->view()->parentView();
                if (parentWidget && parentWidget->asFloatingWindowController() != nullptr) {
                    // Floating window, don't show as we already have the widget titlebar
                    return;
                }
            }
            setVisible(!hide);
        });
    }

protected:
    void childEvent(QChildEvent *event) override
    {
        if (event->type() == QEvent::ChildPolished) {
            if (auto *button = qobject_cast<QAbstractButton *>(event->child())) {
                button->setAttribute(Qt::WA_Hover, true);
                button->installEventFilter(&m_buttonPainter);
            }
        }
        KDDockWidgets::QtWidgets::TitleBar::childEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        paintHeaderStrip(this, rect());
        QPainter p(this);
        QFont font = DesignTokens::font(QStringLiteral("text-caption"));
        font.setWeight(QFont::DemiBold);
        p.setFont(font);
        p.setPen(DesignTokens::color(QStringLiteral("ink")));
        // Leave room for the buttons laid out on the right
        int right = width();
        for (QWidget *child : findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
            if (child->isVisible() && child->x() > width() / 2) {
                right = qMin(right, child->x());
            }
        }
        const QRect textRect(10, 0, right - 14, height());
        p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(m_controller->title(), Qt::ElideRight, textRect.width()));
    }

private:
    KDDockWidgets::Core::TitleBar *const m_controller;
    TitleButtonPainter m_buttonPainter;
};

class KdenliveDockSeparator : public KDDockWidgets::QtWidgets::Separator
{
public:
    explicit KdenliveDockSeparator(KDDockWidgets::Core::Separator *controller, KDDockWidgets::Core::View *parent)
        : KDDockWidgets::QtWidgets::Separator(controller, parent)
        , m_controller(controller)
    {
    }

    ~KdenliveDockSeparator() override;

    void enterEvent(KDDockWidgets::Qt5Qt6Compat::QEnterEvent *event) override
    {
        hovered = true;
        KDDockWidgets::QtWidgets::Separator::enterEvent(event);
        update();
    }

    void leaveEvent(QEvent *event) override
    {
        hovered = false;
        KDDockWidgets::QtWidgets::Separator::leaveEvent(event);
        update();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(QWidget::rect(), DesignTokens::color(QStringLiteral("surface-window")));
        // A hairline at rest, an accent bar while hovered so the drag target is obvious
        const QColor color = DesignTokens::color(hovered ? QStringLiteral("accent") : QStringLiteral("separator"));
        const int thickness = hovered ? 2 : 1;
        const QRect r = QWidget::rect();
        if (m_controller->isVertical()) {
            p.fillRect(QRect(r.x(), r.y() + (r.height() - thickness) / 2, r.width(), thickness), color);
        } else {
            p.fillRect(QRect(r.x() + (r.width() - thickness) / 2, r.y(), thickness, r.height()), color);
        }
    }

private:
    KDDockWidgets::Core::Separator *const m_controller;
    bool hovered{false};
};

KdenliveDockSeparator::~KdenliveDockSeparator() = default;

KDDockWidgets::Core::View *CustomWidgetFactory::createTitleBar(KDDockWidgets::Core::TitleBar *controller, KDDockWidgets::Core::View *parent) const
{
    return new KdenliveDockTitleBar(controller, parent);
}

KDDockWidgets::Core::View *CustomWidgetFactory::createGroup(KDDockWidgets::Core::Group *controller, KDDockWidgets::Core::View *parent) const
{
    return new KdenliveDockGroup(controller, parent);
}

KDDockWidgets::Core::View *CustomWidgetFactory::createStack(KDDockWidgets::Core::Stack *controller, KDDockWidgets::Core::View *parent) const
{
    return new KdenliveDockStack(controller, parent);
}

KDDockWidgets::Core::View *CustomWidgetFactory::createSeparator(KDDockWidgets::Core::Separator *controller, KDDockWidgets::Core::View *parent) const
{
    return new KdenliveDockSeparator(controller, parent);
}

KDDockWidgets::Core::View *CustomWidgetFactory::createTabBar(KDDockWidgets::Core::TabBar *controller, KDDockWidgets::Core::View *parent) const
{
    return new KdenliveDockTabBar(controller, parent);
}

QIcon CustomWidgetFactory::iconForButtonType(KDDockWidgets::TitleBarButtonType type, qreal dpr) const
{
    // Line icons in the same weight as the style's chevrons, drawn on a 16px grid
    const int size = DesignTokens::size(QStringLiteral("icon-md"));
    QPixmap pixmap(QSize(size, size) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(DesignTokens::color(QStringLiteral("ink-secondary")), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    auto arrow = [&p](QPointF from, QPointF to, QPointF corner1, QPointF corner2) {
        p.drawLine(from, to);
        QPainterPath head;
        head.moveTo(corner1);
        head.lineTo(to);
        head.lineTo(corner2);
        p.drawPath(head);
    };
    switch (type) {
    case KDDockWidgets::TitleBarButtonType::Close:
        p.drawLine(QPointF(4.5, 4.5), QPointF(11.5, 11.5));
        p.drawLine(QPointF(11.5, 4.5), QPointF(4.5, 11.5));
        break;
    case KDDockWidgets::TitleBarButtonType::Float:
        // Pop out: frame open at the top right with an arrow leaving it
        p.drawPolyline(QPolygonF({QPointF(7, 3.5), QPointF(3.5, 3.5), QPointF(3.5, 12.5), QPointF(12.5, 12.5), QPointF(12.5, 9)}));
        arrow(QPointF(7.5, 8.5), QPointF(12.5, 3.5), QPointF(9, 3.5), QPointF(12.5, 7));
        break;
    case KDDockWidgets::TitleBarButtonType::Normal:
        // Dock back: arrow entering the frame
        p.drawPolyline(QPolygonF({QPointF(7, 3.5), QPointF(3.5, 3.5), QPointF(3.5, 12.5), QPointF(12.5, 12.5), QPointF(12.5, 9)}));
        arrow(QPointF(12.5, 3.5), QPointF(7.5, 8.5), QPointF(7.5, 5), QPointF(11, 8.5));
        break;
    case KDDockWidgets::TitleBarButtonType::Maximize:
        p.drawRoundedRect(QRectF(3.5, 3.5, 9, 9), 1.5, 1.5);
        break;
    case KDDockWidgets::TitleBarButtonType::Minimize:
        p.drawLine(QPointF(4, 11.5), QPointF(12, 11.5));
        break;
    case KDDockWidgets::TitleBarButtonType::AutoHide:
    case KDDockWidgets::TitleBarButtonType::UnautoHide: {
        // Pin, tilted once the panel auto hides
        if (type == KDDockWidgets::TitleBarButtonType::UnautoHide) {
            p.translate(8, 8);
            p.rotate(45);
            p.translate(-8, -8);
        }
        p.drawRoundedRect(QRectF(6, 2.5, 4, 5), 1, 1);
        p.drawLine(QPointF(4, 7.5), QPointF(12, 7.5));
        p.drawLine(QPointF(8, 7.5), QPointF(8, 13.5));
        break;
    }
    default:
        break;
    }
    p.end();
    return QIcon(pixmap);
}
