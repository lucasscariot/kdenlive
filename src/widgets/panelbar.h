/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <QWidget>

class QHBoxLayout;
class QLabel;
class QToolButton;

/** @class PanelBar
    @brief Top bar with panel toggles on both sides and the project name centered.

    Each toggle raises its dock when it is hidden behind another tab, opens it when closed,
    and closes it when it is already the visible tab.
 */
class PanelBar : public QWidget
{
    Q_OBJECT
public:
    explicit PanelBar(QWidget *window, QWidget *parent = nullptr);
    /** @brief Add a toggle for the dock with unique name @p dockName, on the left or right side */
    void addPanelToggle(const QString &dockName, const QString &label, const QIcon &icon, bool rightSide);
    /** @brief Add a widget after the right side toggles, like the render button */
    void addTrailingWidget(QWidget *widget);

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QWidget *m_window;
    QHBoxLayout *m_left;
    QHBoxLayout *m_right;
    QWidget *m_titleBox;
    QLabel *m_title;
    QLabel *m_state;
    void updateTitle();
    void placeTitle();
};
