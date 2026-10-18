/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <QHash>
#include <QJsonObject>
#include <QPixmap>
#include <QWidget>

class QCheckBox;
class QLineEdit;
class QListWidget;
class QScrollArea;
class QStackedWidget;
class QTreeWidget;
class QLabel;

/** @class DesignReviewWindow
    @brief One window to review the design system against the running interface.

    Pages show every token as it is rendered (colors with their contrast, type styles, the
    spacing scale, radii and sizes, the icon theme), every control the Kdenlive style paints in
    its states, and a live inventory of the main window with the fonts, margins, sizes, icons,
    style sheets and texts that bypass the tokens (DesignAudit). The same pages are rendered to
    PNG by `kdenlive --design-review <folder>` and by the MCP tool desktop_ui_capture, so a
    design conversation can start from pictures and numbers rather than from a running screen.
 */
class DesignReviewWindow : public QWidget
{
    Q_OBJECT
public:
    /** @param subject the interface under review, usually the main window; it is also the parent */
    explicit DesignReviewWindow(QWidget *subject);
    ~DesignReviewWindow() override;

    /** @brief The window reviewing @p subject, created on first use */
    static DesignReviewWindow *instance(QWidget *subject);

    /** @brief Page ids in display order: colors, type, spacing, icons, components, inventory */
    static QStringList pageIds();
    static QString pageTitle(const QString &id);

    /** @brief The active theme id, "dark" or "light" */
    static QString currentTheme();
    /** @brief Switch the application palette to the generated color scheme of @p theme; false when unknown */
    static bool applyTheme(const QString &theme);

    void showPage(const QString &id);
    /** @brief Rebuild the live pages (icons in use, inventory and findings) from the subject */
    void refreshLive();

    /** @brief Render one page at @p width logical pixels and @p scale, without showing the window on screen */
    QPixmap renderPage(const QString &id, int width = 1200, qreal scale = 1.0);
    /** @brief Render any widget at @p scale, like QWidget::grab with a device pixel ratio */
    static QPixmap renderWidget(QWidget *widget, qreal scale = 1.0);
    static QByteArray toPng(const QPixmap &pixmap);

    /** @brief Write every page, the subject window and its panels for @p themes as PNG files into @p directory,
        with review.json holding the audit and a manifest of the files. Returns the manifest, or an error.
        @p openClosedPanels raises every panel for its picture, for throwaway instances only */
    QJsonObject exportReview(const QString &directory, const QStringList &themes, qreal scale, bool openClosedPanels);

    /** @brief MCP desktop_ui_capture: a PNG of the subject window, one panel or one page */
    static QJsonObject captureTool(QWidget *subject, const QJsonObject &arguments);
    /** @brief MCP desktop_design_audit: the DesignAudit report with the panel list */
    static QJsonObject auditTool(QWidget *subject, const QJsonObject &arguments);
    /** @brief The dock panels of the application: {name, title, open, current, floating} */
    static QJsonArray panels();

private:
    QWidget *buildColorsPage();
    QWidget *buildTypePage();
    QWidget *buildSpacingPage();
    QWidget *buildIconsPage();
    QWidget *buildComponentsPage();
    QWidget *buildInventoryPage();
    void addPage(const QString &id, QWidget *content);
    void fillFindings();
    void fillInventory();
    void fillIconUsage();
    void fillFontsInUse();

    QWidget *m_subject;
    QListWidget *m_nav{nullptr};
    QStackedWidget *m_stack{nullptr};
    QHash<QString, QScrollArea *> m_pages;
    QTreeWidget *m_findings{nullptr};
    QTreeWidget *m_inventory{nullptr};
    QLineEdit *m_inventoryFilter{nullptr};
    QCheckBox *m_includeHidden{nullptr};
    QWidget *m_iconUsage{nullptr};
    QTreeWidget *m_fontsInUse{nullptr};
    QLabel *m_liveSummary{nullptr};
    bool m_liveBuilt{false};
};
