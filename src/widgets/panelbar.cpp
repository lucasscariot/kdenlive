/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "panelbar.h"

#include "core.h"
#include "utils/designtokens.h"
#include <algorithm>

#include <KLocalizedString>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QRegularExpression>
#include <QTimer>
#include <QToolButton>
#include <kddockwidgets/core/DockRegistry.h>
#include <kddockwidgets/core/DockWidget.h>
#include <kddockwidgets/qtwidgets/views/DockWidget.h>

PanelBar::PanelBar(QWidget *window, QWidget *parent)
    : QWidget(parent)
    , m_window(window)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(DesignTokens::space(2), DesignTokens::space(1), DesignTokens::space(3), DesignTokens::space(1));
    layout->setSpacing(DesignTokens::space(1));
    m_left = new QHBoxLayout;
    m_left->setSpacing(DesignTokens::space(1));
    m_right = new QHBoxLayout;
    m_right->setSpacing(DesignTokens::space(1));
    layout->addLayout(m_left);
    layout->addStretch(1);
    layout->addLayout(m_right);

    // The project name floats in the exact center, independent of how wide the side groups are
    m_titleBox = new QWidget(this);
    auto *titleLayout = new QHBoxLayout(m_titleBox);
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(DesignTokens::space(3));
    m_title = new QLabel(m_titleBox);
    m_title->setFont(DesignTokens::font(QStringLiteral("text-body-strong")));
    m_state = new QLabel(m_titleBox);
    m_state->setFont(DesignTokens::font(QStringLiteral("text-caption")));
    auto applyColors = [this]() {
        QPalette dim = m_state->palette();
        dim.setColor(QPalette::WindowText, DesignTokens::color(QStringLiteral("ink-secondary")));
        m_state->setPalette(dim);
    };
    applyColors();
    connect(DesignTokens::instance(), &DesignTokens::themeChanged, this, applyColors);
    titleLayout->addWidget(m_title);
    titleLayout->addWidget(m_state);
    m_titleBox->setAttribute(Qt::WA_TransparentForMouseEvents);

    m_window->installEventFilter(this);
    updateTitle();
    // Layout restores do not report which stacked pane became visible, they all end with hideBars
    connect(pCore.get(), &Core::hideBars, this, [this]() { QTimer::singleShot(0, this, &PanelBar::refreshToggles); });
}

void PanelBar::addPanelToggle(const QString &dockName, const QString &label, const QIcon &icon, bool rightSide)
{
    KDDockWidgets::Core::DockWidget *dock = KDDockWidgets::DockRegistry::self()->dockByName(dockName);
    if (!dock) {
        return;
    }
    auto *view = dynamic_cast<KDDockWidgets::QtWidgets::DockWidget *>(dock->view());
    auto *button = new QToolButton(this);
    button->setText(label);
    button->setIcon(icon);
    button->setToolTip(dock->title());
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setAutoRaise(true);
    button->setCheckable(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setFont(DesignTokens::font(QStringLiteral("text-caption")));
    // Drawn as a quiet toggle by the Kdenlive style: brighter label when on, no accent fill
    button->setProperty("_kdenlive_panel_toggle", true);
    auto refresh = [button, dock]() {
        QSignalBlocker blocker(button);
        button->setChecked(dock->isOpen() && dock->isCurrentTab());
    };
    m_refreshers.append(refresh);
    connect(button, &QToolButton::clicked, this, [this, dock]() {
        if (!dock->isOpen()) {
            dock->open();
            dock->setAsCurrentTab();
        } else if (!dock->isCurrentTab()) {
            dock->setAsCurrentTab();
        } else if (!dock->isTabbed()) {
            // A pane of its own hides to give its space back, like the Inspector in Resolve;
            // stacked panes have no empty state, so the active one stays
            dock->close();
        }
        // Raising one pane hides the other panes stacked with it
        refreshToggles();
    });
    if (view) {
        connect(view, &KDDockWidgets::QtWidgets::DockWidget::isOpenChanged, button, refresh);
        connect(view, &KDDockWidgets::QtWidgets::DockWidget::isCurrentTabChanged, button, refresh);
    }
    refresh();
    (rightSide ? m_right : m_left)->addWidget(button);
    m_toggles.append(button);
    equalizeToggles();
}

void PanelBar::equalizeToggles()
{
    // Prefer equal width labelled toggles, then natural widths, then icons only, whichever fits the bar
    QList<int> natural;
    for (QToolButton *toggle : std::as_const(m_toggles)) {
        QToolButton probe;
        probe.setFont(toggle->font());
        probe.setText(toggle->text());
        probe.setIcon(toggle->icon());
        probe.setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        probe.setProperty("_kdenlive_panel_toggle", true);
        natural.append(probe.sizeHint().width());
    }
    int fixedWidth = m_titleBox->sizeHint().width() + 2 * DesignTokens::space(4) + 2 * DesignTokens::space(3);
    for (int i = 0; i < m_right->count(); ++i) {
        if (QWidget *w = m_right->itemAt(i)->widget(); w && !m_toggles.contains(qobject_cast<QToolButton *>(w))) {
            fixedWidth += w->sizeHint().width() + DesignTokens::space(3);
        }
    }
    const int gap = DesignTokens::space(1);
    const int widest = natural.isEmpty() ? 0 : *std::max_element(natural.cbegin(), natural.cend());
    int naturalTotal = 0;
    for (int w : std::as_const(natural)) {
        naturalTotal += w + gap;
    }
    const int available = width() > 0 ? width() : QWIDGETSIZE_MAX;
    const bool equal = fixedWidth + m_toggles.size() * (widest + gap) <= available;
    const bool labelled = equal || fixedWidth + naturalTotal <= available;
    const int iconOnly = DesignTokens::size(QStringLiteral("control-md")) + DesignTokens::space(2);
    for (int i = 0; i < m_toggles.size(); ++i) {
        QToolButton *toggle = m_toggles.at(i);
        toggle->setToolButtonStyle(labelled ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
        toggle->setFixedWidth(equal ? widest : (labelled ? natural.at(i) : iconOnly));
    }
    m_compact = !labelled;
}

void PanelBar::refreshToggles()
{
    for (const auto &refresh : std::as_const(m_refreshers)) {
        refresh();
    }
}

void PanelBar::addTrailingWidget(QWidget *widget)
{
    m_right->addSpacing(DesignTokens::space(3));
    m_right->addWidget(widget);
    widget->show();
}

QSize PanelBar::minimumSizeHint() const
{
    int width = 2 * DesignTokens::space(3);
    const int iconOnly = DesignTokens::size(QStringLiteral("control-md")) + DesignTokens::space(2);
    width += m_toggles.size() * (iconOnly + DesignTokens::space(1));
    for (int i = 0; i < m_right->count(); ++i) {
        if (QWidget *w = m_right->itemAt(i)->widget(); w && !m_toggles.contains(qobject_cast<QToolButton *>(w))) {
            width += w->sizeHint().width() + DesignTokens::space(3);
        }
    }
    return {width, QWidget::minimumSizeHint().height()};
}

void PanelBar::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    equalizeToggles();
    placeTitle();
}

bool PanelBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_window && (event->type() == QEvent::WindowTitleChange || event->type() == QEvent::ModifiedChange)) {
        updateTitle();
    }
    return QWidget::eventFilter(watched, event);
}

void PanelBar::updateTitle()
{
    // Window titles look like "name[*] / profile — Kdenlive", the bar only shows the project name
    QString title = m_window->windowTitle();
    title.remove(QStringLiteral("[*]"));
    static const QRegularExpression separators(QStringLiteral("\\s+[/\u2014\u2013-]\\s+"), QRegularExpression::UseUnicodePropertiesOption);
    title = title.split(separators).value(0);
    m_title->setText(title.trimmed());
    m_state->setText(m_window->isWindowModified() ? i18nc("@info:status the project has unsaved changes", "Edited") : QString());
    m_state->setVisible(m_window->isWindowModified());
    placeTitle();
}

void PanelBar::placeTitle()
{
    m_titleBox->adjustSize();
    QRect box(QPoint(0, 0), m_titleBox->sizeHint());
    box.moveCenter(rect().center());
    // Hide rather than overlap the toggles when the window is narrow
    const int leftEdge = m_left->geometry().right() + DesignTokens::space(4);
    const int rightEdge = m_right->geometry().left() - DesignTokens::space(4);
    m_titleBox->setVisible(box.left() > leftEdge && box.right() < rightEdge);
    m_titleBox->setGeometry(box);
}
