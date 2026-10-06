/*
SPDX-FileCopyrightText: 2012 Jean-Baptiste Mardelle <jb@kdenlive.org>
SPDX-FileCopyrightText: 2014 Till Theato <root@ttill.de>
SPDX-FileCopyrightText: 2020 Julius Künzel <julius.kuenzel@kde.org>
SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "layouts/layoutswitcher.h"
#include "utils/designtokens.h"
#include <QIcon>
#include <QMap>
#include <QObject>
#include <QPushButton>

LayoutSwitcher::LayoutSwitcher(QWidget *parent)
    : QWidget(parent)
    , m_buttonGroup(new QButtonGroup(this))
    , m_layout(new QHBoxLayout)
{
    m_layout->setSpacing(DesignTokens::space(2));
    m_layout->setContentsMargins(0, 0, 0, 0);
    setLayout(m_layout);
    m_buttonGroup->setExclusive(true);
    connect(m_buttonGroup, QOverload<QAbstractButton *>::of(&QButtonGroup::buttonClicked), this, &LayoutSwitcher::buttonClicked);
}

void LayoutSwitcher::buttonClicked(QAbstractButton *button)
{
    if (button) {
        m_currentLayoutId = button->property("layoutid").toString();
        qDebug() << ":::: LAYOUT BUTTON CLICKES: " << m_currentLayoutId;
        Q_EMIT layoutSelected(m_currentLayoutId);
    }
}

void LayoutSwitcher::setLayouts(const QList<QPair<QString, QString>> &layouts, const QString &currentLayout)
{
    // Remove old buttons
    while (QLayoutItem *item = m_layout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            m_buttonGroup->removeButton(qobject_cast<QAbstractButton *>(w));
            delete w;
        }
        delete item;
    }
    // Add new buttons
    for (const auto &pair : layouts) {
        const QString &internalId = pair.first;
        const QString &label = pair.second;
        auto *btn = new QPushButton(label, this);
        btn->setProperty("layoutid", internalId);
        // Default layouts show as icons in the page bar, custom ones keep their name
        static const QMap<QString, QString> pageIcons = {
            {QStringLiteral("logging"), QStringLiteral("view-media-playlist")}, {QStringLiteral("editing"), QStringLiteral("edit-cut")},
            {QStringLiteral("editing_vertical"), QStringLiteral("edit-cut")},   {QStringLiteral("audio"), QStringLiteral("view-media-equalizer")},
            {QStringLiteral("effects"), QStringLiteral("tools-wizard")},        {QStringLiteral("color"), QStringLiteral("color-management")}};
        const QIcon pageIcon = QIcon::fromTheme(pageIcons.value(internalId));
        if (!pageIcon.isNull()) {
            btn->setIcon(pageIcon);
            btn->setIconSize(QSize(DesignTokens::size(QStringLiteral("icon-lg")), DesignTokens::size(QStringLiteral("icon-lg"))));
            btn->setText(QString());
            btn->setMinimumWidth(DesignTokens::space(5) + DesignTokens::space(4));
        }
        btn->setToolTip(label);
        btn->setCheckable(true);
        btn->setFlat(true);
        btn->setFocusPolicy(Qt::NoFocus);
        // Lets the application style draw the buttons as one segmented control
        btn->setProperty("_kdenlive_pagebar", true);
        btn->setFont(DesignTokens::font(QStringLiteral("text-caption")));
        m_buttonGroup->addButton(btn);
        m_layout->addWidget(btn);
        if (!currentLayout.isEmpty() && internalId == currentLayout) {
            btn->setChecked(true);
            m_currentLayoutId = internalId;
        }
    }
}

QString LayoutSwitcher::currentLayout() const
{
    QAbstractButton *checked = m_buttonGroup->checkedButton();
    if (checked) {
        return checked->property("layoutid").toString();
    }
    return QString();
}

void LayoutSwitcher::setCurrentLayout(const QString &layoutId)
{
    const QList<QAbstractButton *> buttons = m_buttonGroup->buttons();
    for (QAbstractButton *btn : buttons) {
        if (btn->property("layoutid").toString() == layoutId) {
            QSignalBlocker blocker(m_buttonGroup);
            btn->setChecked(true);
            m_currentLayoutId = layoutId;
            break;
        }
    }
}
