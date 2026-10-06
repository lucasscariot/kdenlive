/*
    SPDX-FileCopyrightText: 2025 Kdenlive Contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

import QtQuick 2.15
import org.kde.kdenlive as K

Item {
    id: overlayLabel
    property alias text: label.text
    property color backgroundColor: K.Design.colors["scrim"]
    property color textColor: K.Design.colors["on-accent"]
    property int padding: 6
    property alias font: label.font
    property bool flipText: false
    width: label.width + 2 * padding
    height: label.height + padding

    Rectangle {
        anchors.fill: parent
        color: overlayLabel.backgroundColor
        radius: 4
    }
    Text {
        id: label
        anchors.centerIn: parent
        color: overlayLabel.textColor
        font.bold: true
        text: ""
        rotation: overlayLabel.flipText ? 180 : 0
    }
} 