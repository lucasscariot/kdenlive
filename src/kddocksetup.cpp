/*
    SPDX-FileCopyrightText: 2025 Jean-Baptiste Mardelle <jb@kdenlive.org>

SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "kddocksetup.h"
#include "core.h"
#include "kdenlivesettings.h"
#include "utils/kdenlivestyle.h"

#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QHoverEvent>
#include <QMimeData>
#include <QObject>
#include <QTabBar>

#include <QPainter>
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
    p.fillRect(rect, KdenliveStyle::headerColor(widget->palette()));
    p.fillRect(QRect(rect.left(), rect.bottom(), rect.width(), 1), KdenliveStyle::overlay(widget->palette(), 0.1));
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
        setFont(KdenliveStyle::chromeFont(qApp->font()));

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

protected:
    void paintEvent(QPaintEvent *event) override
    {
        paintHeaderStrip(this, rect());
        KDDockWidgets::QtWidgets::TabBar::paintEvent(event);
    }

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

class KdenliveDockTitleBar : public KDDockWidgets::QtWidgets::TitleBar
{
public:
    explicit KdenliveDockTitleBar(KDDockWidgets::Core::TitleBar *controller, KDDockWidgets::Core::View *parent = nullptr)
        : KDDockWidgets::QtWidgets::TitleBar(controller, parent)
        , m_controller(controller)
    {
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
    void paintEvent(QPaintEvent *) override
    {
        paintHeaderStrip(this, rect());
        QPainter p(this);
        QFont font = KdenliveStyle::chromeFont(qApp->font());
        font.setWeight(QFont::DemiBold);
        p.setFont(font);
        p.setPen(KdenliveStyle::overlay(palette(), 0.85));
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
        p.fillRect(QWidget::rect(), palette().window());
        // A hairline at rest, an accent bar while hovered so the drag target is obvious
        const QColor color = hovered ? palette().highlight().color() : KdenliveStyle::overlay(palette(), 0.1);
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
