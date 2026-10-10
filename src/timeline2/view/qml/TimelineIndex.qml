/*
    SPDX-FileCopyrightText: 2026 Lucas Scariot <lucas@scariot.fr>
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import org.kde.ki18n

import org.kde.kdenlive as K

// Final Cut style timeline index: the timeline clips in order, and the tracks grouped by role
Rectangle {
    id: indexRoot
    required property K.TimelineController timeline
    required property K.TimelineItemModel controller

    signal handBackFocus()

    color: K.Design.colors["surface-panel"]

    property int page: K.KdenliveSettings.timelineIndexTab
    property var clips: []

    function refreshClips() {
        if (visible && page === 0) {
            clips = indexRoot.timeline.indexClips(searchField.text)
        }
    }

    onVisibleChanged: refreshClips()
    onPageChanged: {
        K.KdenliveSettings.timelineIndexTab = page
        refreshClips()
    }
    Component.onCompleted: refreshClips()

    Connections {
        target: indexRoot.timeline
        function onIndexClipsChanged() { indexRoot.refreshClips() }
    }

    // Separator from the timeline
    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: K.Design.colors["border-control"]
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.rightMargin: 1
        spacing: 0

        // Pages, as pill tabs
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: K.Design.space3
            spacing: K.Design.space1
            Repeater {
                model: [KI18n.i18n("Clips"), KI18n.i18n("Roles")]
                delegate: Rectangle {
                    id: tab
                    required property string modelData
                    required property int index
                    readonly property bool current: indexRoot.page === index
                    Layout.fillWidth: true
                    implicitHeight: K.Design.controlSm
                    radius: height / 2
                    color: current ? K.Design.colors["accent-fill"] : (tabMouse.containsMouse ? K.Design.alpha("ink", 0.08) : "transparent")
                    Label {
                        anchors.centerIn: parent
                        text: tab.modelData
                        font: K.Design.fonts["text-caption"]
                        color: tab.current ? K.Design.colors["on-accent"] : K.Design.colors["ink-secondary"]
                    }
                    MouseArea {
                        id: tabMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: indexRoot.page = tab.index
                    }
                }
            }
        }

        // Clips page
        ColumnLayout {
            visible: indexRoot.page === 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0
            TextField {
                id: searchField
                Layout.fillWidth: true
                Layout.leftMargin: K.Design.space3
                Layout.rightMargin: K.Design.space3
                Layout.bottomMargin: K.Design.space2
                placeholderText: KI18n.i18n("Search")
                font: K.Design.fonts["text-caption"]
                onTextChanged: indexRoot.refreshClips()
                Keys.onEscapePressed: {
                    text = ''
                    indexRoot.handBackFocus()
                }
            }
            ListView {
                id: clipList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: indexRoot.clips
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                delegate: Rectangle {
                    id: clipRow
                    required property var modelData
                    width: ListView.view.width
                    height: K.Design.controlSm
                    color: clipMouse.containsMouse ? K.Design.alpha("ink", 0.06) : "transparent"
                    Rectangle {
                        id: swatch
                        // Connected clips are indented under the storyline clips
                        x: clipRow.modelData.storyline ? K.Design.space3 : K.Design.space3 + K.Design.space4
                        anchors.verticalCenter: parent.verticalCenter
                        width: K.Design.space3
                        height: K.Design.space3
                        radius: K.Design.radiusXs
                        color: clipRow.modelData.color
                    }
                    Label {
                        anchors.left: swatch.right
                        anchors.leftMargin: K.Design.space3
                        anchors.right: timecode.left
                        anchors.rightMargin: K.Design.space3
                        anchors.verticalCenter: parent.verticalCenter
                        text: clipRow.modelData.name
                        elide: Text.ElideRight
                        font: K.Design.fonts["text-caption"]
                        color: K.Design.colors["ink"]
                    }
                    Label {
                        id: timecode
                        anchors.right: parent.right
                        anchors.rightMargin: K.Design.space3
                        anchors.verticalCenter: parent.verticalCenter
                        text: clipRow.modelData.timecode
                        font: K.Design.fonts["text-caption2"]
                        color: K.Design.colors["ink-tertiary"]
                    }
                    MouseArea {
                        id: clipMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: indexRoot.timeline.revealClip(clipRow.modelData.id)
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                Layout.margins: K.Design.space3
                horizontalAlignment: Text.AlignHCenter
                text: KI18n.i18np("%1 clip", "%1 clips", indexRoot.clips.length)
                font: K.Design.fonts["text-caption"]
                color: K.Design.colors["ink-tertiary"]
            }
        }

        // Roles page
        ListView {
            id: roleList
            visible: indexRoot.page === 1
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: K.Design.space2
            model: indexRoot.timeline.roleLanes
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            delegate: Column {
                id: roleItem
                required property var modelData
                width: ListView.view.width
                // The role, with its checkbox to play or silence all its lanes
                Rectangle {
                    width: parent.width
                    height: K.Design.controlMd
                    color: roleMouse.containsMouse ? K.Design.alpha("ink", 0.06) : "transparent"
                    MouseArea {
                        id: roleMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.NoButton
                    }
                    CheckBox {
                        id: roleCheck
                        x: K.Design.space2
                        anchors.verticalCenter: parent.verticalCenter
                        checked: roleItem.modelData.enabled
                        onToggled: indexRoot.timeline.setRoleEnabled(roleItem.modelData.role, checked)
                        ToolTip.visible: hovered
                        ToolTip.delay: 1000
                        ToolTip.text: roleItem.modelData.audio ? KI18n.i18n("Mute or unmute the %1 lanes", roleItem.modelData.label)
                                                               : KI18n.i18n("Show or hide the %1 lanes", roleItem.modelData.label)
                    }
                    Rectangle {
                        id: roleSwatch
                        anchors.left: roleCheck.right
                        anchors.leftMargin: K.Design.space2
                        anchors.verticalCenter: parent.verticalCenter
                        width: K.Design.space4
                        height: K.Design.iconSm
                        radius: K.Design.radiusXs
                        color: roleItem.modelData.color
                        opacity: roleItem.modelData.enabled ? 1 : K.Design.opacityDisabled
                    }
                    Label {
                        anchors.left: roleSwatch.right
                        anchors.leftMargin: K.Design.space3
                        anchors.right: soloButton.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: roleItem.modelData.label
                        elide: Text.ElideRight
                        font: K.Design.fonts["text-body-strong"]
                        color: roleItem.modelData.enabled ? K.Design.colors["ink"] : K.Design.colors["ink-tertiary"]
                    }
                    ToolButton {
                        id: soloButton
                        anchors.right: parent.right
                        anchors.rightMargin: K.Design.space2
                        anchors.verticalCenter: parent.verticalCenter
                        width: K.Design.controlSm
                        height: K.Design.controlSm
                        icon.name: 'visibility'
                        icon.width: K.Design.iconMd
                        icon.height: K.Design.iconMd
                        focusPolicy: Qt.NoFocus
                        onClicked: indexRoot.timeline.soloRole(roleItem.modelData.role)
                        ToolTip.visible: hovered
                        ToolTip.delay: 1000
                        ToolTip.text: KI18n.i18n("Play only this role, click again to play all roles")
                    }
                }
                // Its lanes
                Repeater {
                    model: roleItem.modelData.lanes
                    delegate: Rectangle {
                        id: laneRow
                        required property var modelData
                        readonly property bool active: modelData.trackId === indexRoot.timeline.activeTrack
                        width: roleItem.width
                        height: K.Design.controlSm
                        color: active ? K.Design.alpha("accent", 0.16) : (laneMouse.containsMouse ? K.Design.alpha("ink", 0.06) : "transparent")
                        MouseArea {
                            id: laneMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onClicked: mouse => {
                                indexRoot.timeline.activeTrack = laneRow.modelData.trackId
                                if (mouse.button === Qt.RightButton) {
                                    laneMenu.popup()
                                }
                            }
                            onDoubleClicked: {
                                laneName.visible = false
                                laneEdit.text = laneRow.modelData.name
                                laneEdit.visible = true
                                laneEdit.forceActiveFocus()
                                laneEdit.selectAll()
                            }
                        }
                        Label {
                            id: laneName
                            anchors.left: parent.left
                            anchors.leftMargin: K.Design.space5 + K.Design.space3
                            anchors.right: laneButtons.left
                            anchors.rightMargin: K.Design.space2
                            anchors.verticalCenter: parent.verticalCenter
                            text: laneRow.modelData.name.length > 0 ? laneRow.modelData.name : laneRow.modelData.tag
                            elide: Text.ElideRight
                            font: K.Design.fonts["text-caption"]
                            color: laneRow.modelData.disabled ? K.Design.colors["ink-tertiary"] : K.Design.colors["ink-secondary"]
                        }
                        TextField {
                            id: laneEdit
                            visible: false
                            anchors.left: parent.left
                            anchors.leftMargin: K.Design.space5 + K.Design.space2
                            anchors.right: laneButtons.left
                            anchors.verticalCenter: parent.verticalCenter
                            height: K.Design.controlSm
                            font: K.Design.fonts["text-caption"]
                            onEditingFinished: {
                                if (visible) {
                                    indexRoot.controller.setTrackName(laneRow.modelData.trackId, text)
                                }
                                visible = false
                                laneName.visible = true
                            }
                            Keys.onEscapePressed: {
                                visible = false
                                laneName.visible = true
                            }
                        }
                        Row {
                            id: laneButtons
                            anchors.right: parent.right
                            anchors.rightMargin: K.Design.space2
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: K.Design.space1
                            Rectangle {
                                // The primary storyline of the magnetic timeline
                                visible: laneRow.modelData.storyline && indexRoot.timeline.magnetic
                                anchors.verticalCenter: parent.verticalCenter
                                width: storylineLabel.implicitWidth + K.Design.space3
                                height: K.Design.space4
                                radius: K.Design.radiusXs
                                color: K.Design.alpha("accent", 0.2)
                                Label {
                                    id: storylineLabel
                                    anchors.centerIn: parent
                                    text: KI18n.i18n("Storyline")
                                    font: K.Design.fonts["text-caption2"]
                                    color: K.Design.colors["accent"]
                                }
                            }
                            ToolButton {
                                width: K.Design.controlSm
                                height: K.Design.controlSm
                                focusPolicy: Qt.NoFocus
                                icon.width: K.Design.iconMd
                                icon.height: K.Design.iconMd
                                icon.name: roleItem.modelData.audio ? (laneRow.modelData.disabled ? 'audio-off' : 'audio-volume-high')
                                                                    : (laneRow.modelData.disabled ? 'kdenlive-hide-video' : 'kdenlive-show-video')
                                opacity: laneRow.modelData.disabled ? 1 : 0.6
                                onClicked: indexRoot.timeline.hideTrack(laneRow.modelData.trackId, laneRow.modelData.disabled)
                                ToolTip.visible: hovered
                                ToolTip.delay: 1000
                                ToolTip.text: roleItem.modelData.audio ? KI18n.i18n("Mute lane") : KI18n.i18n("Hide lane")
                            }
                            ToolButton {
                                width: K.Design.controlSm
                                height: K.Design.controlSm
                                focusPolicy: Qt.NoFocus
                                icon.width: K.Design.iconMd
                                icon.height: K.Design.iconMd
                                icon.name: laneRow.modelData.locked ? 'lock' : 'unlock'
                                opacity: laneRow.modelData.locked ? 1 : 0.6
                                onClicked: indexRoot.controller.setTrackLockedState(laneRow.modelData.trackId, !laneRow.modelData.locked)
                                ToolTip.visible: hovered
                                ToolTip.delay: 1000
                                ToolTip.text: KI18n.i18n("Lock lane")
                            }
                        }
                        Menu {
                            id: laneMenu
                            Repeater {
                                model: indexRoot.timeline.trackRoles(laneRow.modelData.trackId)
                                delegate: MenuItem {
                                    required property string modelData
                                    text: indexRoot.timeline.roleLabel(modelData)
                                    checkable: true
                                    checked: modelData === roleItem.modelData.role
                                    onTriggered: indexRoot.timeline.setTrackRole(laneRow.modelData.trackId, modelData)
                                }
                            }
                            MenuSeparator {
                                visible: !roleItem.modelData.audio
                            }
                            MenuItem {
                                visible: !roleItem.modelData.audio
                                height: visible ? implicitHeight : 0
                                text: KI18n.i18n("Use as Primary Storyline")
                                enabled: !laneRow.modelData.storyline
                                onTriggered: indexRoot.timeline.setPrimaryStoryline(laneRow.modelData.trackId)
                            }
                        }
                    }
                }
            }
        }
    }
}
