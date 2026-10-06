/*
    SPDX-FileCopyrightText: 2022 Jean-Baptiste Mardelle <jb@kdenlive.org>
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

pragma ComponentBehavior: Bound

import QtQuick.Controls 2.15
import QtQuick 2.15
import org.kde.kdenlive as K

Rectangle {
    id: counter
    property int countdown: 3
    property int subcount: 0
    anchors.fill: parent
    property int size: Math.min(width, height)
    signal stopCountdown()
    color: K.Design.colors["scrim"]
    Timer {
        id: countdownTimer
        interval: 100
        running: counter.countdown > 0
        repeat: true
        onTriggered: {
            counter.subcount += 1
            if (counter.subcount % 10 == 0) {
                counter.subcount = 0
                counter.countdown--
                if (counter.countdown == 0) {
                    counter.stopCountdown()
                }
            }
        }
    }
    Rectangle {
        width: counter.size * 0.6
        height: width
        color: K.Design.colors["surface-viewer"]
        border.color: K.Design.colors["on-accent"]
        border.width: 4
        radius: width*0.5
        opacity: 0.5
        anchors.centerIn: parent
    }
    Repeater {
        model: 4
        anchors.fill: parent
        delegate: Label {
            required property int index
            required property int modelData
            anchors.centerIn: parent
            visible: counter.countdown <= index
            opacity: counter.countdown == index ? 1 : 0.4
            scale: counter.countdown >= index ? 1.0 : 0.0
            text: modelData
            color: index < 2 ? 'red' : 'white'
            font.pixelSize: counter.size * 0.5
            Behavior on opacity { NumberAnimation {} }
            //horizontalAlignment: Text.AlignHCenter
        }
    }
    Rectangle {
        color: K.Design.colors["on-accent"]
        height: 5
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 10
        width: parent.width - (counter.subcount * parent.width / 10)
    }
}
