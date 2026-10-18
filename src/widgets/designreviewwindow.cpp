/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "designreviewwindow.h"
#include "utils/designaudit.h"
#include "utils/designpaint.h"
#include "utils/designtokens.h"
#include "utils/flowlayout.h"
#include "utils/timecode.h"
#include "widgets/precisionspinbox.h"
#include "widgets/progressbutton.h"
#include "widgets/timecodedisplay.h"

#include <KAboutData>
#include <KColorScheme>
#include <KLocalizedString>
#include <KMessageWidget>
#include <KSharedConfig>

#include <kddockwidgets/core/DockRegistry.h>
#include <kddockwidgets/core/DockWidget.h>
#include <kddockwidgets/qtwidgets/views/DockWidget.h>

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>

namespace {

QColor token(const QString &name)
{
    return DesignTokens::color(name);
}

/** @brief A painted swatch: fill (over the window surface when translucent), hairline border, radius and optional text */
class Sample : public QWidget
{
public:
    Sample(const QString &fillToken, const QSize &size, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_fill(fillToken)
    {
        setFixedSize(size);
    }
    void setText(const QString &text, const QString &inkToken, const QFont &font)
    {
        m_text = text;
        m_ink = inkToken;
        m_font = font;
        update();
    }
    void setRadius(int radius)
    {
        m_radius = radius;
        update();
    }
    void setBorder(bool border)
    {
        m_border = border;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor fill = token(m_fill);
        if (fill.alpha() < 255) {
            DesignPaint::panel(&painter, rect(), token(QStringLiteral("surface-window")), Qt::transparent, m_radius);
        }
        DesignPaint::panel(&painter, rect(), fill, m_border ? token(QStringLiteral("border-control")) : Qt::transparent, m_radius);
        if (!m_text.isEmpty()) {
            painter.setFont(m_font);
            painter.setPen(token(m_ink));
            painter.drawText(rect(), Qt::AlignCenter, m_text);
        }
    }

private:
    QString m_fill;
    QString m_text;
    QString m_ink{QStringLiteral("ink")};
    QFont m_font;
    int m_radius{-1};
    bool m_border{true};
};

QLabel *label(const QString &text, const QString &style = QStringLiteral("text-body"), const QString &inkToken = QString())
{
    auto *result = new QLabel(text);
    result->setFont(DesignTokens::font(style));
    result->setWordWrap(true);
    result->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (!inkToken.isEmpty()) {
        QPalette palette = result->palette();
        palette.setColor(QPalette::WindowText, token(inkToken));
        result->setPalette(palette);
        // Follow theme switches
        QObject::connect(DesignTokens::instance(), &DesignTokens::themeChanged, result, [result, inkToken]() {
            QPalette updated = result->palette();
            updated.setColor(QPalette::WindowText, token(inkToken));
            result->setPalette(updated);
        });
    }
    return result;
}

QLabel *caption(const QString &text)
{
    return label(text, QStringLiteral("text-caption"), QStringLiteral("ink-secondary"));
}

QLabel *mono(const QString &text)
{
    return label(text, QStringLiteral("text-timecode"), QStringLiteral("ink-secondary"));
}

/** @brief A raised section with a headline, its body laid out vertically */
QWidget *section(const QString &title, const QString &note, QVBoxLayout *&body)
{
    auto *frame = new QWidget;
    DesignPaint::setBackground(frame, QStringLiteral("surface-raised"));
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(DesignTokens::space(4), DesignTokens::space(4), DesignTokens::space(4), DesignTokens::space(4));
    layout->setSpacing(DesignTokens::space(3));
    layout->addWidget(label(title, QStringLiteral("text-headline")));
    if (!note.isEmpty()) {
        layout->addWidget(caption(note));
    }
    body = layout;
    return frame;
}

QWidget *pageContent(const QString &title, const QString &intro, QVBoxLayout *&body)
{
    auto *content = new QWidget;
    DesignPaint::setBackground(content, QStringLiteral("surface-panel"));
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(DesignTokens::space(5), DesignTokens::space(4), DesignTokens::space(5), DesignTokens::space(5));
    layout->setSpacing(DesignTokens::space(4));
    layout->addWidget(label(title, QStringLiteral("text-title3")));
    if (!intro.isEmpty()) {
        layout->addWidget(label(intro, QStringLiteral("text-body"), QStringLiteral("ink-secondary")));
    }
    body = layout;
    return content;
}

QString ratioText(qreal ratio)
{
    return QString::number(ratio, 'f', 1) + QStringLiteral(":1");
}

QString geometryText(const QJsonArray &geometry)
{
    return QStringLiteral("%1,%2 %3×%4").arg(geometry.at(0).toInt()).arg(geometry.at(1).toInt()).arg(geometry.at(2).toInt()).arg(geometry.at(3).toInt());
}

QString fontText(const QJsonObject &font)
{
    if (font.isEmpty()) {
        return QString();
    }
    return QStringLiteral("%1 %2px %3")
        .arg(font.value(QStringLiteral("family")).toString())
        .arg(font.value(QStringLiteral("px")).toInt())
        .arg(font.value(QStringLiteral("weight")).toInt());
}

QString layoutText(const QJsonObject &layout)
{
    if (layout.isEmpty()) {
        return QString();
    }
    const QJsonArray margins = layout.value(QStringLiteral("margins")).toArray();
    const QJsonValue spacing = layout.value(QStringLiteral("spacing"));
    const QString gap = spacing.isArray() ? QStringLiteral("%1/%2").arg(spacing.toArray().at(0).toInt()).arg(spacing.toArray().at(1).toInt())
                                          : QString::number(spacing.toInt());
    return QStringLiteral("m %1 %2 %3 %4 · s %5")
        .arg(margins.at(0).toInt())
        .arg(margins.at(1).toInt())
        .arg(margins.at(2).toInt())
        .arg(margins.at(3).toInt())
        .arg(gap);
}

QStringList samples(const QJsonObject &entry)
{
    QStringList result;
    for (const QJsonValue &value : entry.value(QStringLiteral("samples")).toArray()) {
        result.append(value.toString());
    }
    return result;
}

QJsonObject failure(const QString &code, const QString &message)
{
    return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}}}};
}

QStringList keys(const QJsonObject &object)
{
    return object.keys();
}

KDDockWidgets::Core::DockWidget *dockByName(const QString &name)
{
    return KDDockWidgets::DockRegistry::self()->dockByName(name);
}

QWidget *dockWidget(KDDockWidgets::Core::DockWidget *dock)
{
    return dock ? dynamic_cast<KDDockWidgets::QtWidgets::DockWidget *>(dock->view()) : nullptr;
}

/** @brief The theme of a palette, by comparing its window ground to its text */
QString themeOf(const QPalette &palette)
{
    return palette.color(QPalette::Window).lightness() < palette.color(QPalette::WindowText).lightness() ? QStringLiteral("dark") : QStringLiteral("light");
}

/** @brief Runs a capture under @p theme and restores the previous palette afterwards */
struct ThemeScope
{
    explicit ThemeScope(const QString &theme)
    {
        if (theme.isEmpty() || theme == DesignReviewWindow::currentTheme()) {
            return;
        }
        previous = qApp->palette();
        switched = DesignReviewWindow::applyTheme(theme);
        QCoreApplication::processEvents();
    }
    ~ThemeScope()
    {
        if (switched) {
            qApp->setPalette(previous);
            QCoreApplication::processEvents();
        }
    }
    QPalette previous;
    bool switched{false};
};

} // namespace

DesignReviewWindow::DesignReviewWindow(QWidget *subject)
    : QWidget(subject, Qt::Window)
    , m_subject(subject)
{
    setWindowTitle(i18n("Design Review"));
    setObjectName(QStringLiteral("designReviewWindow"));
    resize(1380, 900);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Navigation
    auto *side = new QWidget(this);
    DesignPaint::setBackground(side, QStringLiteral("surface-sidebar"));
    side->setFixedWidth(180);
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(DesignTokens::space(3), DesignTokens::space(4), DesignTokens::space(3), DesignTokens::space(4));
    sideLayout->setSpacing(DesignTokens::space(3));
    sideLayout->addWidget(label(i18n("Design Review"), QStringLiteral("text-headline")));
    m_nav = new QListWidget(side);
    m_nav->setFrameShape(QFrame::NoFrame);
    m_nav->setFont(DesignTokens::font(QStringLiteral("text-body")));
    for (const QString &id : pageIds()) {
        auto *item = new QListWidgetItem(pageTitle(id), m_nav);
        item->setData(Qt::UserRole, id);
    }
    sideLayout->addWidget(m_nav, 1);

    auto *themeRow = new QWidget(side);
    auto *themeLayout = new QHBoxLayout(themeRow);
    themeLayout->setContentsMargins(0, 0, 0, 0);
    themeLayout->setSpacing(DesignTokens::space(1));
    for (const QString &theme : DesignAudit::themeIds()) {
        auto *button = new QPushButton(theme == QLatin1String("dark") ? i18n("Dark") : theme == QLatin1String("light") ? i18n("Light") : theme, themeRow);
        button->setCheckable(true);
        button->setProperty("_kdenlive_segmented", true);
        button->setChecked(theme == currentTheme());
        connect(button, &QPushButton::clicked, this, [theme]() { applyTheme(theme); });
        connect(DesignTokens::instance(), &DesignTokens::themeChanged, button, [button, theme]() { button->setChecked(theme == currentTheme()); });
        themeLayout->addWidget(button);
    }
    sideLayout->addWidget(caption(i18n("Theme")));
    sideLayout->addWidget(themeRow);

    auto *exportButton = new QPushButton(i18n("Export…"), side);
    exportButton->setToolTip(i18n("Write every page, the main window and its panels as PNG files with review.json into a folder"));
    connect(exportButton, &QPushButton::clicked, this, [this]() {
        const QString folder = QFileDialog::getExistingDirectory(this, i18n("Export Design Review"));
        if (!folder.isEmpty()) {
            exportReview(folder, DesignAudit::themeIds(), devicePixelRatioF(), false);
        }
    });
    sideLayout->addWidget(exportButton);
    layout->addWidget(side);

    m_stack = new QStackedWidget(this);
    layout->addWidget(m_stack, 1);

    addPage(QStringLiteral("colors"), buildColorsPage());
    addPage(QStringLiteral("type"), buildTypePage());
    addPage(QStringLiteral("spacing"), buildSpacingPage());
    addPage(QStringLiteral("icons"), buildIconsPage());
    addPage(QStringLiteral("components"), buildComponentsPage());
    addPage(QStringLiteral("inventory"), buildInventoryPage());

    connect(m_nav, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0) {
            const QString id = m_nav->item(row)->data(Qt::UserRole).toString();
            if (id == QLatin1String("inventory") && !m_liveBuilt) {
                refreshLive();
            }
            m_stack->setCurrentWidget(m_pages.value(id));
        }
    });
    m_nav->setCurrentRow(0);
}

DesignReviewWindow::~DesignReviewWindow() = default;

DesignReviewWindow *DesignReviewWindow::instance(QWidget *subject)
{
    static QPointer<DesignReviewWindow> window;
    if (!window) {
        window = new DesignReviewWindow(subject);
        window->setAttribute(Qt::WA_DeleteOnClose, false);
    }
    return window;
}

QStringList DesignReviewWindow::pageIds()
{
    return {QStringLiteral("colors"), QStringLiteral("type"),       QStringLiteral("spacing"),
            QStringLiteral("icons"),  QStringLiteral("components"), QStringLiteral("inventory")};
}

QString DesignReviewWindow::pageTitle(const QString &id)
{
    if (id == QLatin1String("colors")) return i18n("Colors");
    if (id == QLatin1String("type")) return i18n("Type");
    if (id == QLatin1String("spacing")) return i18n("Spacing & Sizes");
    if (id == QLatin1String("icons")) return i18n("Icons");
    if (id == QLatin1String("components")) return i18n("Components");
    if (id == QLatin1String("inventory")) return i18n("Live Inventory");
    return id;
}

QString DesignReviewWindow::currentTheme()
{
    return themeOf(qApp->palette());
}

bool DesignReviewWindow::applyTheme(const QString &theme)
{
    const QString file = theme == QLatin1String("light")  ? QStringLiteral("KdenliveLight")
                         : theme == QLatin1String("dark") ? QStringLiteral("KdenliveDark")
                                                          : QString();
    if (file.isEmpty()) {
        return false;
    }
    const QString path = QStringLiteral(":/design/color-schemes/%1.colors").arg(file);
    if (!QFile::exists(path)) {
        return false;
    }
    qApp->setPalette(KColorScheme::createApplicationPalette(KSharedConfig::openConfig(path, KConfig::SimpleConfig)));
    return true;
}

void DesignReviewWindow::addPage(const QString &id, QWidget *content)
{
    auto *area = new QScrollArea(m_stack);
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setWidget(content);
    m_pages.insert(id, area);
    m_stack->addWidget(area);
}

void DesignReviewWindow::showPage(const QString &id)
{
    for (int row = 0; row < m_nav->count(); ++row) {
        if (m_nav->item(row)->data(Qt::UserRole).toString() == id) {
            m_nav->setCurrentRow(row);
            return;
        }
    }
}

// Pages

QWidget *DesignReviewWindow::buildColorsPage()
{
    QVBoxLayout *body = nullptr;
    QWidget *content =
        pageContent(i18n("Colors"),
                    i18n("Every color token from tokens.json as the running app resolves it. Swatches follow the active theme; both values are listed."), body);

    QVBoxLayout *tokensBody = nullptr;
    body->addWidget(section(i18n("Tokens"), QString(), tokensBody));
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(DesignTokens::space(4));
    grid->setVerticalSpacing(DesignTokens::space(2));
    grid->setColumnStretch(3, 1);
    int row = 0;
    for (const QJsonValue &entry : DesignAudit::colorTokens()) {
        const QJsonObject color = entry.toObject();
        const QString name = color.value(QStringLiteral("name")).toString();
        auto *swatch = new Sample(name, QSize(72, 36));
        swatch->setRadius(DesignTokens::radius(QStringLiteral("radius-sm")));
        grid->addWidget(swatch, row, 0);
        QString title = name;
        if (color.contains(QStringLiteral("alias"))) {
            title += QStringLiteral(" → {%1}").arg(color.value(QStringLiteral("alias")).toString());
        }
        grid->addWidget(label(title, QStringLiteral("text-body-strong")), row, 1);
        QStringList values;
        const QJsonObject perTheme = color.value(QStringLiteral("values")).toObject();
        for (const QString &theme : DesignAudit::themeIds()) {
            values.append(QStringLiteral("%1 %2").arg(theme, perTheme.value(theme).toString()));
        }
        grid->addWidget(mono(values.join(QStringLiteral("   "))), row, 2);
        grid->addWidget(caption(color.value(QStringLiteral("usage")).toString()), row, 3);
        row++;
    }
    tokensBody->addLayout(grid);

    QVBoxLayout *contrastBody = nullptr;
    body->addWidget(section(i18n("Contrast"),
                            i18n("WCAG ratio of each ink on each surface in the active theme. Text needs 4.5:1; icons, placeholders and ink-tertiary 3:1. Red "
                                 "cells fail the level their usage asks for."),
                            contrastBody));
    auto *matrix = new QGridLayout;
    matrix->setHorizontalSpacing(DesignTokens::space(1));
    matrix->setVerticalSpacing(DesignTokens::space(1));
    const QStringList inks = DesignAudit::inkTokens();
    const QStringList surfaces = DesignAudit::surfaceTokens();
    for (int column = 0; column < surfaces.size(); ++column) {
        auto *head = caption(surfaces.at(column));
        head->setAlignment(Qt::AlignCenter);
        matrix->addWidget(head, 0, column + 1);
    }
    for (int line = 0; line < inks.size(); ++line) {
        matrix->addWidget(caption(inks.at(line)), line + 1, 0);
        for (int column = 0; column < surfaces.size(); ++column) {
            const QString ink = inks.at(line);
            const QString surface = surfaces.at(column);
            auto *cell = new Sample(surface, QSize(96, 32));
            cell->setRadius(DesignTokens::radius(QStringLiteral("radius-xs")));
            const auto refresh = [cell, ink, surface]() {
                const qreal ratio = DesignAudit::contrastRatio(token(ink), token(surface));
                const bool needsLarge = ink == QLatin1String("ink-tertiary");
                const bool passes = ratio >= (needsLarge ? 3.0 : 4.5);
                cell->setText(ratioText(ratio), ink, DesignTokens::font(QStringLiteral("text-timecode")));
                cell->setToolTip(QStringLiteral("%1 on %2: %3 %4").arg(ink, surface, ratioText(ratio), passes ? QStringLiteral("✓") : QStringLiteral("✗")));
                cell->setProperty("fails", !passes);
                cell->setBorder(!passes);
            };
            refresh();
            connect(DesignTokens::instance(), &DesignTokens::themeChanged, cell, refresh);
            matrix->addWidget(cell, line + 1, column + 1);
        }
    }
    contrastBody->addLayout(matrix);
    body->addStretch(1);
    return content;
}

QWidget *DesignReviewWindow::buildTypePage()
{
    QVBoxLayout *body = nullptr;
    QWidget *content = pageContent(i18n("Type"),
                                   i18n("The type styles, rendered with the bundled fonts at their design size. Below, every font the live interface sets "
                                        "explicitly, matched against these styles."),
                                   body);

    QString group;
    QVBoxLayout *groupBody = nullptr;
    for (const QJsonValue &entry : DesignAudit::typeStyles()) {
        const QJsonObject style = entry.toObject();
        if (style.value(QStringLiteral("group")).toString() != group) {
            group = style.value(QStringLiteral("group")).toString();
            body->addWidget(section(group, QString(), groupBody));
        }
        const QString name = style.value(QStringLiteral("name")).toString();
        auto *row = new QWidget;
        auto *rowLayout = new QGridLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setHorizontalSpacing(DesignTokens::space(4));
        rowLayout->setVerticalSpacing(DesignTokens::space(1));
        rowLayout->setColumnMinimumWidth(0, 180);
        rowLayout->addWidget(label(name, QStringLiteral("text-body-strong")), 0, 0);
        rowLayout->addWidget(caption(style.value(QStringLiteral("usage")).toString()), 1, 0);
        auto *sample = label(style.value(QStringLiteral("sample")).toString(), name);
        rowLayout->addWidget(sample, 0, 1, 2, 1);
        rowLayout->addWidget(mono(QStringLiteral("%1 · %2px / %3px · %4 · %5")
                                      .arg(style.value(QStringLiteral("resolvedFamily")).toString())
                                      .arg(style.value(QStringLiteral("px")).toInt())
                                      .arg(style.value(QStringLiteral("lineHeight")).toInt())
                                      .arg(style.value(QStringLiteral("weight")).toInt())
                                      .arg(style.value(QStringLiteral("letterSpacing")).toString())),
                             0, 2, 2, 1);
        rowLayout->setColumnStretch(1, 1);
        groupBody->addWidget(row);
    }

    QVBoxLayout *liveBody = nullptr;
    body->addWidget(section(i18n("Fonts in use"),
                            i18n("Fonts set on widgets of the main window. A row without a type style is a size or weight the design does not define."),
                            liveBody));
    m_fontsInUse = new QTreeWidget;
    m_fontsInUse->setHeaderLabels({i18n("Family"), i18n("Size"), i18n("Weight"), i18n("Type style"), i18n("Widgets"), i18n("Samples")});
    m_fontsInUse->setRootIsDecorated(false);
    m_fontsInUse->setMinimumHeight(220);
    m_fontsInUse->header()->setStretchLastSection(true);
    liveBody->addWidget(m_fontsInUse);
    auto *refresh = new QPushButton(i18n("Refresh"));
    connect(refresh, &QPushButton::clicked, this, &DesignReviewWindow::refreshLive);
    liveBody->addWidget(refresh, 0, Qt::AlignLeft);
    body->addStretch(1);
    return content;
}

QWidget *DesignReviewWindow::buildSpacingPage()
{
    QVBoxLayout *body = nullptr;
    QWidget *content =
        pageContent(i18n("Spacing & Sizes"), i18n("The doubling spacing scale, the radii and the fixed sizes, drawn at their real pixel values."), body);
    const QJsonObject lengths = DesignAudit::lengthTokens();

    QVBoxLayout *spacingBody = nullptr;
    body->addWidget(section(i18n("Spacing"),
                            lengths.value(QStringLiteral("spacing")).toArray().isEmpty()
                                ? QString()
                                : DesignAudit::tokensDocument().value(QStringLiteral("spacing")).toObject().value(QStringLiteral("note")).toString(),
                            spacingBody));
    auto *spacingGrid = new QGridLayout;
    spacingGrid->setHorizontalSpacing(DesignTokens::space(4));
    spacingGrid->setVerticalSpacing(DesignTokens::space(2));
    spacingGrid->setColumnStretch(3, 1);
    int row = 0;
    for (const QJsonValue &entry : lengths.value(QStringLiteral("spacing")).toArray()) {
        const QJsonObject tokenObject = entry.toObject();
        const int value = tokenObject.value(QStringLiteral("value")).toString().chopped(2).toInt();
        spacingGrid->addWidget(label(tokenObject.value(QStringLiteral("name")).toString(), QStringLiteral("text-body-strong")), row, 0);
        auto *bar = new Sample(QStringLiteral("accent-fill"), QSize(qMax(value, 1), 16));
        bar->setBorder(false);
        bar->setRadius(0);
        spacingGrid->addWidget(bar, row, 1, Qt::AlignLeft);
        spacingGrid->addWidget(mono(tokenObject.value(QStringLiteral("value")).toString()), row, 2);
        spacingGrid->addWidget(caption(tokenObject.value(QStringLiteral("usage")).toString()), row, 3);
        row++;
    }
    spacingBody->addLayout(spacingGrid);

    QVBoxLayout *radiusBody = nullptr;
    body->addWidget(section(i18n("Radii"), QString(), radiusBody));
    auto *radiusRow = new QHBoxLayout;
    radiusRow->setSpacing(DesignTokens::space(4));
    for (const QJsonValue &entry : lengths.value(QStringLiteral("radius")).toArray()) {
        const QJsonObject tokenObject = entry.toObject();
        const QString name = tokenObject.value(QStringLiteral("name")).toString();
        auto *cell = new QWidget;
        auto *cellLayout = new QVBoxLayout(cell);
        cellLayout->setContentsMargins(0, 0, 0, 0);
        cellLayout->setSpacing(DesignTokens::space(1));
        auto *shape = new Sample(QStringLiteral("surface-control"), QSize(72, 48));
        shape->setRadius(qMin(DesignTokens::radius(name), 24));
        cellLayout->addWidget(shape);
        cellLayout->addWidget(label(name, QStringLiteral("text-caption")));
        cellLayout->addWidget(mono(tokenObject.value(QStringLiteral("value")).toString()));
        auto *usage = caption(tokenObject.value(QStringLiteral("usage")).toString());
        usage->setMaximumWidth(140);
        cellLayout->addWidget(usage);
        cellLayout->addStretch(1);
        radiusRow->addWidget(cell);
    }
    radiusRow->addStretch(1);
    radiusBody->addLayout(radiusRow);

    QVBoxLayout *sizeBody = nullptr;
    body->addWidget(
        section(i18n("Sizes"), DesignAudit::tokensDocument().value(QStringLiteral("size")).toObject().value(QStringLiteral("note")).toString(), sizeBody));
    auto *sizeGrid = new QGridLayout;
    sizeGrid->setHorizontalSpacing(DesignTokens::space(4));
    sizeGrid->setVerticalSpacing(DesignTokens::space(3));
    sizeGrid->setColumnStretch(3, 1);
    row = 0;
    for (const QJsonValue &entry : lengths.value(QStringLiteral("size")).toArray()) {
        const QJsonObject tokenObject = entry.toObject();
        const QString name = tokenObject.value(QStringLiteral("name")).toString();
        const int value = DesignTokens::size(name);
        sizeGrid->addWidget(label(name, QStringLiteral("text-body-strong")), row, 0);
        QWidget *demo = nullptr;
        if (name.startsWith(QLatin1String("control-"))) {
            auto *button = new QPushButton(i18n("Button"));
            button->setFixedHeight(value);
            demo = button;
        } else if (name.startsWith(QLatin1String("icon-"))) {
            auto *icon = new QLabel;
            icon->setPixmap(QIcon::fromTheme(QStringLiteral("edit-cut")).pixmap(QSize(value, value)));
            connect(DesignTokens::instance(), &DesignTokens::themeChanged, icon,
                    [icon, value]() { icon->setPixmap(QIcon::fromTheme(QStringLiteral("edit-cut")).pixmap(QSize(value, value))); });
            demo = icon;
        } else if (name.startsWith(QLatin1String("lane-"))) {
            auto *lane = new Sample(name == QLatin1String("lane-video") ? QStringLiteral("role-video") : QStringLiteral("role-music"), QSize(220, value));
            lane->setRadius(DesignTokens::radius(QStringLiteral("radius-sm")));
            demo = lane;
        } else if (name == QLatin1String("scrollbar")) {
            auto *bar = new QScrollBar(Qt::Horizontal);
            bar->setRange(0, 100);
            bar->setValue(30);
            bar->setFixedWidth(220);
            demo = bar;
        } else if (name == QLatin1String("indicator")) {
            auto *box = new QCheckBox(i18n("Option"));
            box->setChecked(true);
            demo = box;
        } else if (name.startsWith(QLatin1String("slider-"))) {
            auto *slider = new QSlider(Qt::Horizontal);
            slider->setRange(0, 100);
            slider->setValue(40);
            slider->setFixedWidth(220);
            demo = slider;
        } else {
            auto *box = new Sample(QStringLiteral("surface-control"), QSize(value, value));
            demo = box;
        }
        sizeGrid->addWidget(demo, row, 1, Qt::AlignLeft | Qt::AlignVCenter);
        sizeGrid->addWidget(mono(tokenObject.value(QStringLiteral("value")).toString()), row, 2);
        sizeGrid->addWidget(caption(tokenObject.value(QStringLiteral("usage")).toString()), row, 3);
        row++;
    }
    sizeBody->addLayout(sizeGrid);

    QVBoxLayout *opacityBody = nullptr;
    body->addWidget(section(i18n("Opacity"), QString(), opacityBody));
    for (const QJsonValue &entry : lengths.value(QStringLiteral("opacity")).toArray()) {
        const QJsonObject tokenObject = entry.toObject();
        auto *line = new QHBoxLayout;
        line->setSpacing(DesignTokens::space(4));
        line->addWidget(label(tokenObject.value(QStringLiteral("name")).toString(), QStringLiteral("text-body-strong")));
        line->addWidget(mono(tokenObject.value(QStringLiteral("value")).toString()));
        line->addWidget(caption(tokenObject.value(QStringLiteral("usage")).toString()), 1);
        opacityBody->addLayout(line);
    }
    body->addStretch(1);
    return content;
}

QWidget *DesignReviewWindow::buildIconsPage()
{
    QVBoxLayout *body = nullptr;
    QWidget *content = pageContent(i18n("Icons"),
                                   i18n("The Kdenlive PRO icon theme, generated from Lucide. Names the theme does not cover fall back to Breeze; the live list "
                                        "below shows which ones the main window asks for."),
                                   body);

    QVBoxLayout *themeBody = nullptr;
    const QStringList names = DesignAudit::themeIcons();
    body->addWidget(section(i18n("Theme icons (%1)", names.size()), QString(), themeBody));
    auto *filter = new QLineEdit;
    filter->setPlaceholderText(i18n("Filter icons…"));
    filter->setClearButtonEnabled(true);
    filter->setMaximumWidth(320);
    themeBody->addWidget(filter);
    auto *cells = new QWidget;
    auto *flow = new FlowLayout(cells, 0, DesignTokens::space(2), DesignTokens::space(2));
    QList<QWidget *> cellWidgets;
    for (const QString &name : names) {
        auto *cell = new QWidget;
        cell->setObjectName(name);
        cell->setFixedWidth(112);
        auto *cellLayout = new QVBoxLayout(cell);
        cellLayout->setContentsMargins(DesignTokens::space(2), DesignTokens::space(2), DesignTokens::space(2), DesignTokens::space(2));
        cellLayout->setSpacing(DesignTokens::space(1));
        auto *picture = new QLabel;
        picture->setAlignment(Qt::AlignCenter);
        const int side = DesignTokens::size(QStringLiteral("icon-lg"));
        const auto paint = [picture, name, side]() { picture->setPixmap(QIcon::fromTheme(name).pixmap(QSize(side, side))); };
        paint();
        connect(DesignTokens::instance(), &DesignTokens::themeChanged, picture, paint);
        cellLayout->addWidget(picture);
        auto *text = caption(name);
        text->setAlignment(Qt::AlignCenter);
        text->setWordWrap(true);
        cellLayout->addWidget(text);
        flow->addWidget(cell);
        cellWidgets.append(cell);
    }
    connect(filter, &QLineEdit::textChanged, cells, [cellWidgets](const QString &text) {
        for (QWidget *cell : cellWidgets) {
            cell->setVisible(text.isEmpty() || cell->objectName().contains(text, Qt::CaseInsensitive));
        }
    });
    themeBody->addWidget(cells);

    QVBoxLayout *usageBody = nullptr;
    body->addWidget(section(i18n("Icons outside the theme"),
                            i18n("Icon names used by actions and buttons of the main window that the theme does not draw. Each is drawn here by the fallback "
                                 "theme: add it to generate-icon-theme.py to bring it into the design."),
                            usageBody));
    m_iconUsage = new QWidget;
    new FlowLayout(m_iconUsage, 0, DesignTokens::space(2), DesignTokens::space(2));
    usageBody->addWidget(m_iconUsage);
    auto *refresh = new QPushButton(i18n("Refresh"));
    connect(refresh, &QPushButton::clicked, this, &DesignReviewWindow::refreshLive);
    usageBody->addWidget(refresh, 0, Qt::AlignLeft);
    body->addStretch(1);
    return content;
}

QWidget *DesignReviewWindow::buildComponentsPage()
{
    QVBoxLayout *body = nullptr;
    QWidget *content = pageContent(
        i18n("Components"),
        i18n("Every control the Kdenlive style paints, in its states. Hover is simulated, pressed buttons are held down, the last column is disabled."), body);

    enum State { Normal, Hover, Pressed, Disabled };
    const QStringList columns{i18n("Normal"), i18n("Hover"), i18n("Pressed / checked"), i18n("Disabled")};
    const auto applyState = [](QWidget *widget, State state) {
        if (state == Hover) {
            widget->setAttribute(Qt::WA_UnderMouse, true);
            const QList<QWidget *> children = widget->findChildren<QWidget *>();
            for (QWidget *child : children) {
                child->setAttribute(Qt::WA_UnderMouse, true);
            }
        } else if (state == Pressed) {
            if (auto *button = qobject_cast<QAbstractButton *>(widget)) {
                if (button->isCheckable()) {
                    button->setChecked(true);
                } else {
                    button->setDown(true);
                }
            }
        } else if (state == Disabled) {
            widget->setEnabled(false);
        }
    };

    QVBoxLayout *groupBody = nullptr;
    QGridLayout *grid = nullptr;
    int row = 0;
    const auto startGroup = [&](const QString &title) {
        body->addWidget(section(title, QString(), groupBody));
        grid = new QGridLayout;
        grid->setHorizontalSpacing(DesignTokens::space(4));
        grid->setVerticalSpacing(DesignTokens::space(3));
        grid->setColumnMinimumWidth(0, 170);
        for (int column = 0; column < columns.size(); ++column) {
            grid->addWidget(caption(columns.at(column)), 0, column + 1);
            grid->setColumnStretch(column + 1, 1);
        }
        groupBody->addLayout(grid);
        row = 1;
    };
    // A control in all four states, @p states says which columns apply
    const auto addControl = [&](const QString &name, const std::function<QWidget *()> &make, const QList<State> &states = {Normal, Hover, Pressed, Disabled}) {
        grid->addWidget(label(name, QStringLiteral("text-body-strong")), row, 0);
        for (State state : states) {
            QWidget *widget = make();
            applyState(widget, state);
            grid->addWidget(widget, row, int(state) + 1, Qt::AlignLeft | Qt::AlignVCenter);
        }
        row++;
    };
    const auto flagged = [](QWidget *widget, const char *flag) {
        widget->setProperty(flag, true);
        return widget;
    };

    startGroup(i18n("Buttons"));
    addControl(i18n("Push button"), []() { return new QPushButton(i18n("Button")); });
    addControl(i18n("Push button with icon"), []() { return new QPushButton(QIcon::fromTheme(QStringLiteral("document-save")), i18n("Save")); });
    addControl(i18n("Primary button"), [flagged]() { return flagged(new QPushButton(i18n("Render")), "_kdenlive_primary"); });
    addControl(i18n("Checkable push button"), []() {
        auto *button = new QPushButton(i18n("Toggle"));
        button->setCheckable(true);
        return button;
    });
    addControl(i18n("Segmented control"),
               [flagged]() {
                   auto *box = new QWidget;
                   auto *layout = new QHBoxLayout(box);
                   layout->setContentsMargins(0, 0, 0, 0);
                   layout->setSpacing(DesignTokens::space(1));
                   int index = 0;
                   for (const QString &text : {i18n("Clip"), i18n("Track"), i18n("Master")}) {
                       auto *segment = new QPushButton(text);
                       segment->setCheckable(true);
                       segment->setChecked(index == 1);
                       layout->addWidget(flagged(segment, "_kdenlive_segmented"));
                       index++;
                   }
                   return box;
               },
               {Normal, Hover, Disabled});
    addControl(i18n("Page bar"), [flagged]() {
        auto *page = new QPushButton(i18n("Edit"));
        page->setCheckable(true);
        return flagged(page, "_kdenlive_pagebar");
    });
    addControl(i18n("Tool button"), []() {
        auto *button = new QToolButton;
        button->setIcon(QIcon::fromTheme(QStringLiteral("edit-cut")));
        button->setAutoRaise(true);
        return button;
    });
    addControl(i18n("Tool button with label"), []() {
        auto *button = new QToolButton;
        button->setIcon(QIcon::fromTheme(QStringLiteral("edit-cut")));
        button->setText(i18n("Cut"));
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setAutoRaise(true);
        return button;
    });
    addControl(i18n("Tool button with menu"), []() {
        auto *button = new QToolButton;
        button->setIcon(QIcon::fromTheme(QStringLiteral("configure")));
        button->setText(i18n("Options"));
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setPopupMode(QToolButton::MenuButtonPopup);
        auto *menu = new QMenu(button);
        menu->addAction(i18n("First"));
        menu->addAction(i18n("Second"));
        button->setMenu(menu);
        return button;
    });
    addControl(i18n("Panel toggle"), [flagged]() {
        auto *button = new QToolButton;
        button->setIcon(QIcon::fromTheme(QStringLiteral("folder-videos")));
        button->setText(i18n("Project Bin"));
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setAutoRaise(true);
        button->setCheckable(true);
        button->setFont(DesignTokens::font(QStringLiteral("text-caption")));
        return flagged(button, "_kdenlive_panel_toggle");
    });
    addControl(i18n("Primary tool button"), [flagged]() {
        auto *button = new QToolButton;
        button->setIcon(QIcon::fromTheme(QStringLiteral("media-record")));
        button->setText(i18n("Render"));
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        return flagged(button, "_kdenlive_primary");
    });
    addControl(i18n("Progress button"),
               []() {
                   auto *button = new ProgressButton(i18n("Preview"), 100);
                   auto *action = new QAction(QIcon::fromTheme(QStringLiteral("media-playback-start")), i18n("Preview"), button);
                   auto *inProgress = new QAction(QIcon::fromTheme(QStringLiteral("process-stop")), i18n("Stop"), button);
                   button->defineDefaultAction(action, inProgress);
                   button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
                   button->setProgress(45);
                   return button;
               },
               {Normal, Hover, Disabled});

    startGroup(i18n("Selection"));
    addControl(i18n("Check box"), []() { return new QCheckBox(i18n("Option")); });
    addControl(i18n("Check box, tristate"),
               []() {
                   auto *box = new QCheckBox(i18n("Some selected"));
                   box->setTristate(true);
                   box->setCheckState(Qt::PartiallyChecked);
                   return box;
               },
               {Normal, Hover, Disabled});
    addControl(i18n("Radio button"), []() { return new QRadioButton(i18n("Choice")); });

    startGroup(i18n("Fields"));
    addControl(i18n("Line edit, empty"),
               []() {
                   auto *edit = new QLineEdit;
                   edit->setPlaceholderText(i18n("Search…"));
                   edit->setClearButtonEnabled(true);
                   return edit;
               },
               {Normal, Hover, Disabled});
    addControl(i18n("Line edit, text"), []() { return new QLineEdit(i18n("Interview take 3")); }, {Normal, Hover, Disabled});
    addControl(i18n("Combo box"),
               []() {
                   auto *combo = new QComboBox;
                   combo->addItems({i18n("Normal"), i18n("Overwrite"), i18n("Insert")});
                   return combo;
               },
               {Normal, Hover, Disabled});
    addControl(i18n("Editable combo box"),
               []() {
                   auto *combo = new QComboBox;
                   combo->setEditable(true);
                   combo->addItems({QStringLiteral("1920x1080"), QStringLiteral("1080x1920")});
                   return combo;
               },
               {Normal, Hover, Disabled});
    addControl(i18n("Spin box"),
               []() {
                   auto *spin = new QSpinBox;
                   spin->setRange(0, 999);
                   spin->setValue(25);
                   spin->setSuffix(QStringLiteral(" fps"));
                   return spin;
               },
               {Normal, Hover, Disabled});
    addControl(i18n("Double spin box"),
               []() {
                   auto *spin = new QDoubleSpinBox;
                   spin->setRange(-60, 12);
                   spin->setValue(-6.5);
                   spin->setSuffix(QStringLiteral(" dB"));
                   return spin;
               },
               {Normal, Hover, Disabled});
    addControl(i18n("Precision spin box"), []() { return new PrecisionSpinBox(nullptr, 0, 100, 3, 42.5); }, {Normal, Hover, Disabled});
    addControl(i18n("Timecode"),
               []() {
                   auto *timecode = new TimecodeDisplay(nullptr, Timecode(Timecode::HH_MM_SS_FF, 25));
                   timecode->setValue(1234);
                   return timecode;
               },
               {Normal, Hover, Disabled});
    addControl(i18n("Text edit"),
               []() {
                   auto *edit = new QPlainTextEdit;
                   edit->setPlainText(i18n("A short note.\nSecond line."));
                   edit->setFixedSize(220, 64);
                   return edit;
               },
               {Normal, Hover, Disabled});

    startGroup(i18n("Sliders, progress and scrolling"));
    addControl(i18n("Slider"),
               []() {
                   auto *slider = new QSlider(Qt::Horizontal);
                   slider->setRange(0, 100);
                   slider->setValue(35);
                   slider->setFixedWidth(200);
                   return slider;
               },
               {Normal, Hover, Disabled});
    addControl(i18n("Progress bar"),
               []() {
                   auto *bar = new QProgressBar;
                   bar->setRange(0, 100);
                   bar->setValue(60);
                   bar->setFixedWidth(200);
                   return bar;
               },
               {Normal, Disabled});
    addControl(i18n("Progress bar, busy"),
               []() {
                   auto *bar = new QProgressBar;
                   bar->setRange(0, 0);
                   bar->setFixedWidth(200);
                   return bar;
               },
               {Normal});
    addControl(i18n("Scroll bar"),
               []() {
                   auto *bar = new QScrollBar(Qt::Horizontal);
                   bar->setRange(0, 100);
                   bar->setValue(20);
                   bar->setFixedWidth(200);
                   return bar;
               },
               {Normal, Hover, Disabled});

    startGroup(i18n("Containers"));
    addControl(i18n("Tab bar"),
               []() {
                   auto *tabs = new QTabBar;
                   tabs->addTab(i18n("Project Bin"));
                   tabs->addTab(i18n("Effects"));
                   tabs->addTab(i18n("Compositions"));
                   tabs->setCurrentIndex(1);
                   return tabs;
               },
               {Normal, Hover, Disabled});
    addControl(i18n("Document tab bar"),
               []() {
                   auto *tabs = new QTabBar;
                   tabs->setDocumentMode(true);
                   tabs->setTabsClosable(true);
                   tabs->addTab(i18n("Sequence 1"));
                   tabs->addTab(i18n("Sequence 2"));
                   return tabs;
               },
               {Normal, Hover, Disabled});
    addControl(i18n("Group box"),
               []() {
                   auto *box = new QGroupBox(i18n("Audio"));
                   auto *layout = new QVBoxLayout(box);
                   layout->addWidget(new QCheckBox(i18n("Normalize")));
                   layout->addWidget(new QCheckBox(i18n("Mono")));
                   return box;
               },
               {Normal, Disabled});
    addControl(i18n("Menu bar"),
               []() {
                   auto *bar = new QMenuBar;
                   bar->setNativeMenuBar(false);
                   bar->addMenu(i18n("File"));
                   bar->addMenu(i18n("Edit"));
                   bar->addMenu(i18n("View"));
                   bar->setFixedWidth(200);
                   return bar;
               },
               {Normal, Disabled});

    startGroup(i18n("Lists"));
    addControl(i18n("Tree"),
               []() {
                   auto *tree = new QTreeWidget;
                   tree->setHeaderLabels({i18n("Name"), i18n("Duration")});
                   tree->setFixedSize(280, 150);
                   for (int index = 0; index < 4; ++index) {
                       auto *item = new QTreeWidgetItem(tree, {i18n("Clip %1", index + 1), QStringLiteral("00:00:1%1:00").arg(index)});
                       item->setIcon(0, QIcon::fromTheme(index % 2 ? QStringLiteral("audio-x-generic") : QStringLiteral("video-x-generic")));
                       if (index == 1) {
                           item->setSelected(true);
                       }
                   }
                   return tree;
               },
               {Normal, Disabled});
    addControl(i18n("List"),
               []() {
                   auto *list = new QListWidget;
                   list->setFixedSize(220, 120);
                   list->addItems({i18n("Blur"), i18n("Color correction"), i18n("Transform"), i18n("Volume")});
                   list->setCurrentRow(2);
                   return list;
               },
               {Normal, Disabled});

    startGroup(i18n("Text and badges"));
    addControl(i18n("Body and caption"),
               []() {
                   auto *box = new QWidget;
                   auto *layout = new QVBoxLayout(box);
                   layout->setContentsMargins(0, 0, 0, 0);
                   layout->setSpacing(DesignTokens::space(1));
                   layout->addWidget(label(i18n("Opacity, Scale, Position")));
                   layout->addWidget(caption(i18n("Pane tabs, clip names, help text")));
                   return box;
               },
               {Normal, Disabled});
    addControl(i18n("Badges"),
               []() {
                   auto *box = new QWidget;
                   auto *layout = new QHBoxLayout(box);
                   layout->setContentsMargins(0, 0, 0, 0);
                   layout->setSpacing(DesignTokens::space(2));
                   const QList<QPair<QString, QString>> badges{{i18n("Rendered"), QStringLiteral("success-fill")},
                                                               {i18n("Missing proxy"), QStringLiteral("warning")},
                                                               {i18n("Offline"), QStringLiteral("danger-fill")},
                                                               {i18n("4K"), QStringLiteral("surface-control")}};
                   for (const auto &[text, fill] : badges) {
                       auto *badge = new QLabel(text);
                       const QString ink = fill == QLatin1String("surface-control") ? QStringLiteral("ink") : QStringLiteral("on-accent");
                       badge->setStyleSheet(DesignPaint::badgeStyleSheet(QStringLiteral("QLabel"), fill, ink));
                       badge->setFont(DesignTokens::font(QStringLiteral("text-caption2")));
                       layout->addWidget(badge);
                   }
                   return box;
               },
               {Normal, Disabled});
    addControl(i18n("Inline message"),
               []() {
                   auto *message = new KMessageWidget(i18n("The proxy clips are being generated."));
                   message->setMessageType(KMessageWidget::Information);
                   message->setCloseButtonVisible(true);
                   message->setWordWrap(true);
                   message->setFixedWidth(260);
                   return message;
               },
               {Normal, Disabled});
    addControl(i18n("Warning message"),
               []() {
                   auto *message = new KMessageWidget(i18n("Some clips are missing."));
                   message->setMessageType(KMessageWidget::Warning);
                   message->setWordWrap(true);
                   message->setFixedWidth(260);
                   return message;
               },
               {Normal});

    body->addStretch(1);
    return content;
}

QWidget *DesignReviewWindow::buildInventoryPage()
{
    QVBoxLayout *body = nullptr;
    QWidget *content =
        pageContent(i18n("Live Inventory"),
                    i18n("What the main window really does: findings group the fonts, margins, control heights, icon sizes, fallback icons, style sheets, "
                         "custom palettes and text slips outside the tokens; the inventory lists every widget with its geometry, font and layout."),
                    body);

    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(DesignTokens::space(3));
    auto *refresh = new QPushButton(i18n("Refresh"));
    connect(refresh, &QPushButton::clicked, this, &DesignReviewWindow::refreshLive);
    toolbar->addWidget(refresh);
    m_includeHidden = new QCheckBox(i18n("Include hidden widgets"));
    m_includeHidden->setChecked(true);
    connect(m_includeHidden, &QCheckBox::toggled, this, &DesignReviewWindow::refreshLive);
    toolbar->addWidget(m_includeHidden);
    m_liveSummary = caption(QString());
    toolbar->addWidget(m_liveSummary, 1);
    body->addLayout(toolbar);

    auto *splitter = new QSplitter(Qt::Vertical);
    splitter->setChildrenCollapsible(false);

    m_findings = new QTreeWidget;
    m_findings->setHeaderLabels({i18n("Finding"), i18n("Value"), i18n("Count"), i18n("Where")});
    m_findings->header()->setStretchLastSection(true);
    m_findings->setMinimumHeight(260);
    splitter->addWidget(m_findings);

    auto *inventoryBox = new QWidget;
    auto *inventoryLayout = new QVBoxLayout(inventoryBox);
    inventoryLayout->setContentsMargins(0, 0, 0, 0);
    inventoryLayout->setSpacing(DesignTokens::space(2));
    m_inventoryFilter = new QLineEdit;
    m_inventoryFilter->setPlaceholderText(i18n("Filter by class, object name or text…"));
    m_inventoryFilter->setClearButtonEnabled(true);
    inventoryLayout->addWidget(m_inventoryFilter);
    m_inventory = new QTreeWidget;
    m_inventory->setHeaderLabels({i18n("Widget"), i18n("Name"), i18n("Geometry"), i18n("Font"), i18n("Layout"), i18n("Text")});
    m_inventory->header()->setStretchLastSection(true);
    m_inventory->setMinimumHeight(320);
    m_inventory->setUniformRowHeights(true);
    inventoryLayout->addWidget(m_inventory);
    splitter->addWidget(inventoryBox);
    body->addWidget(splitter, 1);

    connect(m_inventoryFilter, &QLineEdit::textChanged, this, [this](const QString &text) {
        std::function<bool(QTreeWidgetItem *)> apply = [&](QTreeWidgetItem *item) {
            bool match = text.isEmpty();
            for (int column = 0; column < item->columnCount() && !match; ++column) {
                match = item->text(column).contains(text, Qt::CaseInsensitive);
            }
            bool childMatch = false;
            for (int index = 0; index < item->childCount(); ++index) {
                childMatch = apply(item->child(index)) || childMatch;
            }
            item->setHidden(!(match || childMatch));
            if (childMatch && !text.isEmpty()) {
                item->setExpanded(true);
            }
            return match || childMatch;
        };
        for (int index = 0; index < m_inventory->topLevelItemCount(); ++index) {
            apply(m_inventory->topLevelItem(index));
        }
    });
    return content;
}

// Live data

void DesignReviewWindow::refreshLive()
{
    m_liveBuilt = true;
    fillFindings();
    fillInventory();
    fillIconUsage();
    fillFontsInUse();
}

void DesignReviewWindow::fillFindings()
{
    m_findings->clear();
    if (!m_subject) {
        return;
    }
    const QJsonObject findings = DesignAudit::findings(m_subject, m_includeHidden->isChecked());
    const auto group = [this, &findings](const QString &key, const QString &title, const QString &valueKey,
                                         const std::function<QString(const QJsonObject &)> &value) {
        const QJsonArray entries = findings.value(key).toArray();
        auto *top = new QTreeWidgetItem(m_findings, {QStringLiteral("%1 (%2)").arg(title).arg(entries.size())});
        top->setFont(0, DesignTokens::font(QStringLiteral("text-body-strong")));
        for (const QJsonValue &entry : entries) {
            const QJsonObject object = entry.toObject();
            new QTreeWidgetItem(top, {valueKey.isEmpty() ? QString() : object.value(valueKey).toString(), value(object),
                                      QString::number(object.value(QStringLiteral("count")).toInt()), samples(object).join(QStringLiteral(", "))});
        }
        top->setExpanded(entries.size() <= 12);
    };
    group(QStringLiteral("fonts"), i18n("Fonts set on widgets"), QString(), [](const QJsonObject &entry) {
        const QString style = entry.value(QStringLiteral("style")).toString();
        return QStringLiteral("%1 %2px %3 → %4")
            .arg(entry.value(QStringLiteral("family")).toString())
            .arg(entry.value(QStringLiteral("px")).toInt())
            .arg(entry.value(QStringLiteral("weight")).toInt())
            .arg(style.isEmpty() ? i18n("no type style") : style);
    });
    group(QStringLiteral("spacing"), i18n("Margins and spacings off the scale"), QString(),
          [](const QJsonObject &entry) { return i18n("%1 px", entry.value(QStringLiteral("px")).toInt()); });
    group(QStringLiteral("sizes"), i18n("Control heights off the sizes"), QString(),
          [](const QJsonObject &entry) { return i18n("%1 px", entry.value(QStringLiteral("px")).toInt()); });
    group(QStringLiteral("iconSizes"), i18n("Icon sizes off the sizes"), QString(),
          [](const QJsonObject &entry) { return i18n("%1 px", entry.value(QStringLiteral("px")).toInt()); });
    group(QStringLiteral("icons"), i18n("Icons outside the theme"), QStringLiteral("name"), [](const QJsonObject &) { return i18n("falls back to Breeze"); });
    group(QStringLiteral("styleSheets"), i18n("Style sheets"), QString(),
          [](const QJsonObject &entry) { return entry.value(QStringLiteral("styleSheet")).toString(); });
    group(QStringLiteral("palettes"), i18n("Custom palettes"), QStringLiteral("widget"), [](const QJsonObject &) { return QString(); });
    group(QStringLiteral("texts"), i18n("Text slips"), QStringLiteral("text"), [](const QJsonObject &entry) {
        QStringList issues;
        for (const QJsonValue &issue : entry.value(QStringLiteral("issues")).toArray()) {
            issues.append(issue.toString());
        }
        return issues.join(QStringLiteral(", "));
    });
    for (int column = 0; column < 3; ++column) {
        m_findings->resizeColumnToContents(column);
    }
    m_liveSummary->setText(i18n("%1 widgets inspected", findings.value(QStringLiteral("widgets")).toInt()));
}

void DesignReviewWindow::fillInventory()
{
    m_inventory->clear();
    if (!m_subject) {
        return;
    }
    const QJsonObject tree = DesignAudit::inventory(m_subject, m_includeHidden->isChecked());
    std::function<void(QTreeWidgetItem *, const QJsonObject &)> add = [&](QTreeWidgetItem *parent, const QJsonObject &node) {
        const QStringList columns{node.value(QStringLiteral("class")).toString(),
                                  node.value(QStringLiteral("name")).toString(),
                                  geometryText(node.value(QStringLiteral("geometry")).toArray()),
                                  fontText(node.value(QStringLiteral("font")).toObject()),
                                  layoutText(node.value(QStringLiteral("layout")).toObject()),
                                  node.value(QStringLiteral("text")).toString().simplified().left(60)};
        auto *item = parent ? new QTreeWidgetItem(parent, columns) : new QTreeWidgetItem(m_inventory, columns);
        if (!node.value(QStringLiteral("visible")).toBool()) {
            item->setForeground(0, token(QStringLiteral("ink-tertiary")));
        }
        for (const QJsonValue &child : node.value(QStringLiteral("children")).toArray()) {
            add(item, child.toObject());
        }
    };
    add(nullptr, tree);
    if (m_inventory->topLevelItemCount() > 0) {
        m_inventory->topLevelItem(0)->setExpanded(true);
    }
    for (int column = 0; column < 5; ++column) {
        m_inventory->resizeColumnToContents(column);
    }
    if (!m_inventoryFilter->text().isEmpty()) {
        Q_EMIT m_inventoryFilter->textChanged(m_inventoryFilter->text());
    }
}

void DesignReviewWindow::fillIconUsage()
{
    QLayout *flow = m_iconUsage->layout();
    while (QLayoutItem *item = flow->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (!m_subject) {
        return;
    }
    for (const QJsonValue &entry : DesignAudit::iconUsage(m_subject)) {
        const QJsonObject icon = entry.toObject();
        if (icon.value(QStringLiteral("inTheme")).toBool()) {
            continue;
        }
        const QString name = icon.value(QStringLiteral("name")).toString();
        auto *cell = new QWidget;
        cell->setFixedWidth(112);
        auto *cellLayout = new QVBoxLayout(cell);
        cellLayout->setContentsMargins(DesignTokens::space(2), DesignTokens::space(2), DesignTokens::space(2), DesignTokens::space(2));
        cellLayout->setSpacing(DesignTokens::space(1));
        auto *picture = new QLabel;
        picture->setAlignment(Qt::AlignCenter);
        const int side = DesignTokens::size(QStringLiteral("icon-lg"));
        picture->setPixmap(QIcon::fromTheme(name).pixmap(QSize(side, side)));
        cellLayout->addWidget(picture);
        auto *text = caption(QStringLiteral("%1 ×%2").arg(name).arg(icon.value(QStringLiteral("count")).toInt()));
        text->setAlignment(Qt::AlignCenter);
        cellLayout->addWidget(text);
        flow->addWidget(cell);
    }
}

void DesignReviewWindow::fillFontsInUse()
{
    m_fontsInUse->clear();
    if (!m_subject) {
        return;
    }
    const QJsonObject findings = DesignAudit::findings(m_subject, m_includeHidden->isChecked());
    for (const QJsonValue &entry : findings.value(QStringLiteral("fonts")).toArray()) {
        const QJsonObject font = entry.toObject();
        const QString style = font.value(QStringLiteral("style")).toString();
        auto *item =
            new QTreeWidgetItem(m_fontsInUse, {font.value(QStringLiteral("family")).toString(), i18n("%1 px", font.value(QStringLiteral("px")).toInt()),
                                               QString::number(font.value(QStringLiteral("weight")).toInt()), style.isEmpty() ? i18n("none") : style,
                                               QString::number(font.value(QStringLiteral("count")).toInt()), samples(font).join(QStringLiteral(", "))});
        if (style.isEmpty()) {
            item->setForeground(3, token(QStringLiteral("danger")));
        }
    }
    for (int column = 0; column < 5; ++column) {
        m_fontsInUse->resizeColumnToContents(column);
    }
}

// Rendering

QPixmap DesignReviewWindow::renderWidget(QWidget *widget, qreal scale)
{
    if (!widget) {
        return QPixmap();
    }
    QPixmap pixmap(widget->size() * scale);
    pixmap.setDevicePixelRatio(scale);
    pixmap.fill(Qt::transparent);
    widget->render(&pixmap, QPoint(), QRegion(), QWidget::DrawWindowBackground | QWidget::DrawChildren);
    return pixmap;
}

QByteArray DesignReviewWindow::toPng(const QPixmap &pixmap)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    pixmap.save(&buffer, "PNG");
    return bytes;
}

QPixmap DesignReviewWindow::renderPage(const QString &id, int width, qreal scale)
{
    QScrollArea *area = m_pages.value(id);
    if (!area) {
        return QPixmap();
    }
    if (id == QLatin1String("inventory") && !m_liveBuilt) {
        refreshLive();
    }
    // Lay the window out without showing it on screen, at a width that gives the page @p width
    const bool offScreen = !isVisible();
    if (offScreen) {
        setAttribute(Qt::WA_DontShowOnScreen, true);
        resize(width + 180 + area->verticalScrollBar()->sizeHint().width(), 900);
        show();
    }
    showPage(id);
    QCoreApplication::processEvents();
    QWidget *content = area->widget();
    content->ensurePolished();
    const QPixmap pixmap = renderWidget(content, scale);
    if (offScreen) {
        hide();
        setAttribute(Qt::WA_DontShowOnScreen, false);
    }
    return pixmap;
}

QJsonArray DesignReviewWindow::panels()
{
    QJsonArray result;
    const QVector<KDDockWidgets::Core::DockWidget *> docks = KDDockWidgets::DockRegistry::self()->dockwidgets();
    for (KDDockWidgets::Core::DockWidget *dock : docks) {
        result.append(QJsonObject{{QStringLiteral("name"), dock->uniqueName()},
                                  {QStringLiteral("title"), dock->title()},
                                  {QStringLiteral("open"), dock->isOpen()},
                                  {QStringLiteral("current"), dock->isOpen() && dock->isCurrentTab()},
                                  {QStringLiteral("floating"), dock->isFloating()}});
    }
    return result;
}

QJsonObject DesignReviewWindow::exportReview(const QString &directory, const QStringList &themes, qreal scale, bool openClosedPanels)
{
    const QDir root(directory);
    if (!root.mkpath(QStringLiteral("."))) {
        return failure(QStringLiteral("INVALID_PATH"), i18n("Cannot create %1", directory));
    }
    const QString initialTheme = currentTheme();
    QJsonObject manifest{{QStringLiteral("generatedAt"), QDateTime::currentDateTime().toString(Qt::ISODate)},
                         {QStringLiteral("version"), KAboutData::applicationData().version()},
                         {QStringLiteral("scale"), scale},
                         {QStringLiteral("pages"), QJsonArray::fromStringList(pageIds())},
                         {QStringLiteral("themes"), QJsonArray::fromStringList(themes)}};
    QJsonObject screens;
    const auto save = [&](const QString &relative, const QPixmap &pixmap) {
        root.mkpath(QFileInfo(relative).path());
        pixmap.save(root.filePath(relative), "PNG");
        return QJsonObject{{QStringLiteral("file"), relative},
                           {QStringLiteral("width"), int(pixmap.width() / scale)},
                           {QStringLiteral("height"), int(pixmap.height() / scale)}};
    };
    for (const QString &theme : themes) {
        applyTheme(theme);
        QCoreApplication::processEvents();
        refreshLive();
        QJsonObject perTheme;
        QJsonObject pages;
        for (const QString &id : pageIds()) {
            pages.insert(id, save(QStringLiteral("%1/page-%2.png").arg(theme, id), renderPage(id, 1200, scale)));
        }
        perTheme.insert(QStringLiteral("pages"), pages);
        if (m_subject) {
            QCoreApplication::processEvents();
            perTheme.insert(QStringLiteral("window"), save(QStringLiteral("%1/window.png").arg(theme), renderWidget(m_subject, scale)));
        }
        screens.insert(theme, perTheme);
    }
    // Panels last: raising every panel for its picture rearranges the layout the window pictures show
    if (m_subject) {
        const QVector<KDDockWidgets::Core::DockWidget *> docks = KDDockWidgets::DockRegistry::self()->dockwidgets();
        for (KDDockWidgets::Core::DockWidget *dock : docks) {
            if (!dock->isOpen() && !openClosedPanels) {
                continue;
            }
            if (openClosedPanels) {
                if (!dock->isOpen()) {
                    dock->open();
                }
                dock->setAsCurrentTab();
                QCoreApplication::processEvents();
            }
            QWidget *view = dockWidget(dock);
            if (!view || view->size().isEmpty()) {
                continue;
            }
            for (const QString &theme : themes) {
                applyTheme(theme);
                QCoreApplication::processEvents();
                QJsonObject perTheme = screens.value(theme).toObject();
                QJsonArray panelFiles = perTheme.value(QStringLiteral("panels")).toArray();
                QJsonObject panel = save(QStringLiteral("%1/panel-%2.png").arg(theme, dock->uniqueName()), renderWidget(view, scale));
                panel.insert(QStringLiteral("name"), dock->uniqueName());
                panel.insert(QStringLiteral("title"), dock->title());
                panelFiles.append(panel);
                perTheme.insert(QStringLiteral("panels"), panelFiles);
                screens.insert(theme, perTheme);
            }
        }
    }
    applyTheme(initialTheme);
    QCoreApplication::processEvents();
    refreshLive();
    manifest.insert(QStringLiteral("screens"), screens);
    manifest.insert(QStringLiteral("panels"), panels());
    manifest.insert(QStringLiteral("report"), DesignAudit::report(m_subject, DesignAudit::reportSections()));
    QFile file(root.filePath(QStringLiteral("review.json")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return failure(QStringLiteral("WRITE_FAILED"), i18n("Cannot write %1", file.fileName()));
    }
    file.write(QJsonDocument(manifest).toJson());
    manifest.insert(QStringLiteral("ok"), true);
    manifest.insert(QStringLiteral("directory"), root.absolutePath());
    return manifest;
}

// MCP tools

QJsonObject DesignReviewWindow::captureTool(QWidget *subject, const QJsonObject &arguments)
{
    static const QStringList allowed{QStringLiteral("target"), QStringLiteral("panel"), QStringLiteral("page"),
                                     QStringLiteral("theme"),  QStringLiteral("scale"), QStringLiteral("width")};
    for (const QString &key : keys(arguments)) {
        if (!allowed.contains(key)) {
            return failure(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("Unknown argument %1.").arg(key));
        }
    }
    const QString target = arguments.value(QStringLiteral("target")).toString();
    const QString theme = arguments.value(QStringLiteral("theme")).toString();
    const qreal scale = arguments.value(QStringLiteral("scale")).toDouble(1.0);
    const int width = arguments.value(QStringLiteral("width")).toInt(1200);
    if (!theme.isEmpty() && !DesignAudit::themeIds().contains(theme)) {
        return failure(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("theme must be one of %1.").arg(DesignAudit::themeIds().join(QStringLiteral(", "))));
    }
    if (scale < 1 || scale > 3 || width < 480 || width > 3840) {
        return failure(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("scale must be 1 to 3 and width 480 to 3840."));
    }
    if (!subject) {
        return failure(QStringLiteral("NO_WINDOW"), QStringLiteral("The editor window is not available."));
    }
    QPixmap pixmap;
    QJsonObject result{{QStringLiteral("ok"), true}, {QStringLiteral("target"), target}, {QStringLiteral("scale"), scale}};
    if (target == QLatin1String("window")) {
        ThemeScope scope(theme);
        pixmap = renderWidget(subject, scale);
    } else if (target == QLatin1String("panel")) {
        const QString name = arguments.value(QStringLiteral("panel")).toString();
        KDDockWidgets::Core::DockWidget *dock = dockByName(name);
        if (!dock) {
            return failure(QStringLiteral("UNKNOWN_PANEL"), QStringLiteral("No panel named %1; desktop_design_audit lists them.").arg(name));
        }
        if (!dock->isOpen()) {
            return failure(QStringLiteral("PANEL_CLOSED"), QStringLiteral("Panel %1 is closed; the user can open it from the panel bar.").arg(name));
        }
        if (!dock->isCurrentTab()) {
            dock->setAsCurrentTab();
            QCoreApplication::processEvents();
        }
        ThemeScope scope(theme);
        pixmap = renderWidget(dockWidget(dock), scale);
        result.insert(QStringLiteral("panel"), name);
        result.insert(QStringLiteral("title"), dock->title());
    } else if (target == QLatin1String("page")) {
        const QString page = arguments.value(QStringLiteral("page")).toString();
        if (!pageIds().contains(page)) {
            return failure(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("page must be one of %1.").arg(pageIds().join(QStringLiteral(", "))));
        }
        ThemeScope scope(theme);
        DesignReviewWindow *window = instance(subject);
        if (page == QLatin1String("inventory") || page == QLatin1String("icons") || page == QLatin1String("type")) {
            window->refreshLive();
        }
        pixmap = window->renderPage(page, width, scale);
        result.insert(QStringLiteral("page"), page);
    } else {
        return failure(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("target must be window, panel or page."));
    }
    if (pixmap.isNull()) {
        return failure(QStringLiteral("CAPTURE_FAILED"), QStringLiteral("Nothing was rendered."));
    }
    result.insert(QStringLiteral("theme"), theme.isEmpty() ? currentTheme() : theme);
    result.insert(QStringLiteral("width"), pixmap.width());
    result.insert(QStringLiteral("height"), pixmap.height());
    result.insert(QStringLiteral("image"), QJsonObject{{QStringLiteral("data"), QString::fromLatin1(toPng(pixmap).toBase64())},
                                                       {QStringLiteral("mimeType"), QStringLiteral("image/png")}});
    return result;
}

QJsonObject DesignReviewWindow::auditTool(QWidget *subject, const QJsonObject &arguments)
{
    static const QStringList allowed{QStringLiteral("include"), QStringLiteral("panel")};
    for (const QString &key : keys(arguments)) {
        if (!allowed.contains(key)) {
            return failure(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("Unknown argument %1.").arg(key));
        }
    }
    QStringList sections;
    for (const QJsonValue &value : arguments.value(QStringLiteral("include")).toArray()) {
        if (!DesignAudit::reportSections().contains(value.toString())) {
            return failure(QStringLiteral("INVALID_ARGUMENTS"),
                           QStringLiteral("include entries must be among %1.").arg(DesignAudit::reportSections().join(QStringLiteral(", "))));
        }
        sections.append(value.toString());
    }
    QWidget *root = subject;
    if (arguments.contains(QStringLiteral("panel"))) {
        KDDockWidgets::Core::DockWidget *dock = dockByName(arguments.value(QStringLiteral("panel")).toString());
        root = dockWidget(dock);
        if (!root) {
            return failure(QStringLiteral("UNKNOWN_PANEL"), QStringLiteral("No panel named %1.").arg(arguments.value(QStringLiteral("panel")).toString()));
        }
    }
    QJsonObject result = DesignAudit::report(root, sections);
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("panels"), panels());
    result.insert(QStringLiteral("pages"), QJsonArray::fromStringList(pageIds()));
    return result;
}
