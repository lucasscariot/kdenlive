/*
    SPDX-FileCopyrightText: 2020 Jean-Baptiste Mardelle <jb@kdenlive.org>
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

import QtQuick 2.15
import QtQuick.Shapes 1.15
import org.kde.kdenlive as K

Shape {
    id: mixShape
    asynchronous: true
    opacity: 0.4

    ShapePath {
        fillColor: K.Design.alpha("on-accent", 0.67)
        strokeColor: "transparent"
        PathLine {x: 0; y: 0}
        PathLine {x: mixShape.width; y: mixShape.height}
        PathLine {x: 0; y: mixShape.height}
        PathLine {x: 0; y: 0}
    }
    /*ShapePath {
        fillColor: K.Design.alpha("on-accent", 0.4)
        strokeColor: "transparent"
        PathLine {x: mixShape.width; y: 0}
        PathLine {x: mixShape.width; y: mixShape.height}
        PathLine {x: 0; y: mixShape.height}
        PathLine {x: mixShape.width; y: 0}
    }*/
}
