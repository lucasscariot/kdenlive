/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

class LiveBridge;

namespace McpTools {
QJsonArray definitions();
QJsonObject call(LiveBridge &engine, const QString &name, const QJsonObject &arguments, const QString &additionalMediaRoot);
} // namespace McpTools
