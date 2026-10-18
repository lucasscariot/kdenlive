/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

class QWidget;

/** @brief Read back the design system and measure the running interface against it.

    Everything here is data: the tokens as the app resolved them, the contrast of every ink on
    every surface, the type styles, the icons the theme covers, and a walk over a widget tree
    that lists fonts, layout margins, control heights, icons, style sheets and texts that do not
    come from the tokens. The design review window, the command line export and the MCP tools
    all read the same report.
 */
namespace DesignAudit {

/** @brief WCAG 2 contrast ratio of @p foreground over @p background, 1 to 21; translucent colors are composited first */
qreal contrastRatio(QColor foreground, QColor background);

/** @brief The tokens.json document as shipped in the binary */
QJsonObject tokensDocument();
/** @brief The theme ids declared in tokens.json, first one first ("dark", "light") */
QStringList themeIds();
/** @brief Color tokens in file order: {name, usage, values: {theme: "#rrggbbaa"}} with aliases resolved */
QJsonArray colorTokens();
/** @brief Type styles in file order: {name, group, family, px, pt, weight, lineHeight, letterSpacing, sample, usage} */
QJsonArray typeStyles();
/** @brief Spacing, radius, size and opacity tokens: {family: [{name, value, usage}]} */
QJsonObject lengthTokens();
/** @brief The ink tokens checked for contrast, and the surfaces they are checked on */
QStringList inkTokens();
QStringList surfaceTokens();
/** @brief Contrast of every ink on every surface for @p theme: {ink, surface, ratio, aa (4.5), large (3)} */
QJsonArray contrastMatrix(const QString &theme);
/** @brief Pairs below 4.5:1 for @p theme, except ink-tertiary which only has to reach 3:1 */
QJsonArray contrastFailures(const QString &theme);

/** @brief Icon names drawn by the Kdenlive PRO icon theme (the same set in both themes) */
QStringList themeIcons();
/** @brief Icon names a widget tree asks for, by name: {name, count, inTheme} sorted by count */
QJsonArray iconUsage(QWidget *root);

/** @brief The widget tree under @p root: {class, name, visible, geometry, font, layout, text, children} */
QJsonObject inventory(QWidget *root, bool includeHidden = true, int maxDepth = 64);
/** @brief What the live interface does outside the tokens: fonts, spacing, sizes, icons, styleSheets, palettes, texts */
QJsonObject findings(QWidget *root, bool includeHidden = true);
/** @brief The full audit of @p root: tokens, contrast per theme, type styles, findings and, when asked, the inventory */
QJsonObject report(QWidget *root, const QStringList &sections = {});
/** @brief Every section name report() knows */
QStringList reportSections();

} // namespace DesignAudit
