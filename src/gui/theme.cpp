#include "gui/theme.h"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>
#include <QStyleHints>

namespace iso {

namespace {

QString tokenReplace(QString tpl, const Palette& p)
{
    const auto hex = [](const QColor& c) { return c.name(QColor::HexRgb); };
    tpl.replace(QStringLiteral("@bg"), hex(p.background));
    tpl.replace(QStringLiteral("@surfaceAlt"), hex(p.surfaceAlt));
    tpl.replace(QStringLiteral("@surface"), hex(p.surface));
    tpl.replace(QStringLiteral("@border"), hex(p.border));
    tpl.replace(QStringLiteral("@mutedText"), hex(p.mutedText));
    tpl.replace(QStringLiteral("@text"), hex(p.text));
    tpl.replace(QStringLiteral("@accentHover"), hex(p.accentHover));
    tpl.replace(QStringLiteral("@accentPressed"), hex(p.accentPressed));
    tpl.replace(QStringLiteral("@accentText"), hex(p.accentText));
    tpl.replace(QStringLiteral("@accent"), hex(p.accent));
    tpl.replace(
        QStringLiteral("@chevron"),
        p.isDark ? QStringLiteral(":/icons/chevron-down-dark.svg") : QStringLiteral(":/icons/chevron-down-light.svg"));
    return tpl;
}

} // namespace

Palette lightPalette()
{
    Palette p;
    p.background = QColor("#f4f6fb");
    p.surface = QColor("#ffffff");
    p.surfaceAlt = QColor("#eef2f9");
    p.border = QColor("#d5dce8");
    p.text = QColor("#1f2937");
    p.mutedText = QColor("#64748b");
    p.accent = QColor("#2563eb");
    p.accentHover = QColor("#1d4ed8");
    p.accentPressed = QColor("#1e40af");
    p.accentText = QColor("#ffffff");
    p.statusMatch = QColor("#15803d");
    p.statusMismatch = QColor("#c2410c");
    p.statusError = QColor("#b91c1c");
    p.statusInfo = QColor("#2563eb");
    p.statusCancelled = QColor("#64748b");
    p.isDark = false;
    return p;
}

Palette darkPalette()
{
    Palette p;
    p.background = QColor("#0f141b");
    p.surface = QColor("#191f29");
    p.surfaceAlt = QColor("#232b38");
    p.border = QColor("#2d3848");
    p.text = QColor("#e7ecf4");
    p.mutedText = QColor("#94a3b8");
    p.accent = QColor("#3b82f6");
    p.accentHover = QColor("#60a5fa");
    p.accentPressed = QColor("#2563eb");
    p.accentText = QColor("#ffffff");
    p.statusMatch = QColor("#4ade80");
    p.statusMismatch = QColor("#fb923c");
    p.statusError = QColor("#f87171");
    p.statusInfo = QColor("#60a5fa");
    p.statusCancelled = QColor("#94a3b8");
    p.isDark = true;
    return p;
}

const Palette& paletteFor(ColorScheme scheme)
{
    static const Palette light = lightPalette();
    static const Palette dark = darkPalette();
    return scheme == ColorScheme::Dark ? dark : light;
}

ColorScheme resolveColorScheme(Theme theme)
{
    switch (theme) {
    case Theme::Light:
        return ColorScheme::Light;
    case Theme::Dark:
        return ColorScheme::Dark;
    case Theme::System:
        break;
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (auto* hints = QApplication::styleHints()) {
        if (hints->colorScheme() == Qt::ColorScheme::Dark) {
            return ColorScheme::Dark;
        }
    }
    return ColorScheme::Light;
#else
    const QColor windowColor = QApplication::palette().color(QPalette::Window);
    return windowColor.lightness() < 128 ? ColorScheme::Dark : ColorScheme::Light;
#endif
}

QString buildStyleSheet(const Palette& palette)
{
    static const QString tpl = QStringLiteral(R"(
QWidget {
    font-family: 'Segoe UI', 'Inter', sans-serif;
    font-size: 10pt;
    background: @bg;
    color: @text;
}

QGroupBox#card {
    background: @surface;
    border: 1px solid @border;
    border-radius: 12px;
    margin-top: 14px;
    padding: 18px 16px 16px 16px;
}
QGroupBox#card::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    left: 14px;
    padding: 0 6px;
    color: @mutedText;
    font-weight: 600;
}

QLabel { background: transparent; }
QLabel#title { font-size: 22pt; font-weight: 700; color: @text; }
QLabel#subtitle { color: @mutedText; }
QLabel#fieldLabel { color: @mutedText; font-weight: 600; }
QLabel#footnote { color: @mutedText; }

QTextEdit#mismatchDetail {
    background: transparent;
    color: @mutedText;
    border: none;
    padding: 0;
}

QLineEdit {
    background: @surfaceAlt;
    border: 1px solid @border;
    border-radius: 8px;
    padding: 8px 10px;
    selection-background-color: @accent;
    selection-color: @accentText;
}
QLineEdit:focus { border: 1px solid @accent; background: @surface; }
QLineEdit:read-only { color: @mutedText; }
QLineEdit:disabled { color: @mutedText; }

QComboBox {
    background: @surfaceAlt;
    border: 1px solid @border;
    border-radius: 8px;
    padding: 7px 10px;
    min-height: 20px;
    combobox-popup: 0;
}
QComboBox:hover { border: 1px solid @accent; }
QComboBox:focus { border: 1px solid @accent; }
QComboBox::drop-down {
    subcontrol-origin: padding;
    subcontrol-position: center right;
    width: 30px;
    border: none;
}
QComboBox::down-arrow {
    image: url(@chevron);
    width: 14px;
    height: 14px;
    margin-right: 10px;
}
QComboBox QAbstractItemView {
    background: @surface;
    border: 1px solid @border;
    border-radius: 8px;
    padding: 4px;
    outline: none;
    selection-background-color: transparent;
    selection-color: @accentText;
}
QComboBox QAbstractItemView::item {
    padding: 0px 10px;
    min-height: 30px;
    border: none;
    border-radius: 6px;
    color: @text;
}
QComboBox QAbstractItemView::item:hover {
    background: @surfaceAlt;
    color: @text;
}
QComboBox QAbstractItemView::item:selected {
    background: @accent;
    color: @accentText;
}

QPushButton {
    background: @surfaceAlt;
    color: @text;
    border: 1px solid @border;
    border-radius: 8px;
    padding: 8px 16px;
    font-weight: 600;
}
QPushButton:hover { border: 1px solid @accent; color: @accent; }
QPushButton:pressed { background: @border; }
QPushButton:disabled { color: @mutedText; border: 1px solid @border; background: @surfaceAlt; }

QPushButton[variant="primary"] {
    background: @accent;
    color: @accentText;
    border: 1px solid @accent;
    padding: 10px 22px;
}
QPushButton[variant="primary"]:hover { background: @accentHover; border: 1px solid @accentHover; color: @accentText; }
QPushButton[variant="primary"]:pressed { background: @accentPressed; border: 1px solid @accentPressed; }
QPushButton[variant="primary"]:disabled { background: @surfaceAlt; color: @mutedText; border: 1px solid @border; }

QPushButton[variant="secondary"] {
    background: @surfaceAlt;
    color: @text;
    border: 1px solid @border;
}
QPushButton[variant="secondary"]:hover { border: 1px solid @accent; color: @accent; }
QPushButton[variant="secondary"]:pressed { background: @border; }

QPushButton[variant="text"] {
    background: transparent;
    color: @mutedText;
    border: 1px solid transparent;
    padding: 6px 12px;
    font-weight: 600;
}
QPushButton[variant="text"]:hover { color: @accent; background: @surfaceAlt; }
QPushButton[variant="text"]:pressed { background: @border; }

QProgressBar {
    background: @surfaceAlt;
    border: 1px solid @border;
    border-radius: 7px;
    min-height: 28px;
    text-align: center;
    color: @text;
}
QProgressBar::chunk {
    background: @accent;
    border-radius: 6px;
}

QLabel#statusBadge {
    border-radius: 8px;
    padding: 8px 12px;
    font-weight: 700;
}

QFrame#divider { background: @border; max-height: 1px; border: none; }
)");

    return tokenReplace(tpl, palette);
}

void applyTheme(QApplication& app, Theme theme)
{
    if (auto* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        app.setStyle(fusion);
    }

    const ColorScheme scheme = resolveColorScheme(theme);
    const Palette& p = paletteFor(scheme);

    QPalette qpal;
    qpal.setColor(QPalette::Window, p.background);
    qpal.setColor(QPalette::WindowText, p.text);
    qpal.setColor(QPalette::Base, p.surfaceAlt);
    qpal.setColor(QPalette::AlternateBase, p.surface);
    qpal.setColor(QPalette::Text, p.text);
    qpal.setColor(QPalette::Button, p.surfaceAlt);
    qpal.setColor(QPalette::ButtonText, p.text);
    qpal.setColor(QPalette::Highlight, p.accent);
    qpal.setColor(QPalette::HighlightedText, p.accentText);
    qpal.setColor(QPalette::ToolTipBase, p.surface);
    qpal.setColor(QPalette::ToolTipText, p.text);
    qpal.setColor(QPalette::PlaceholderText, p.mutedText);
    qpal.setColor(QPalette::Disabled, QPalette::Text, p.mutedText);
    qpal.setColor(QPalette::Disabled, QPalette::ButtonText, p.mutedText);
    app.setPalette(qpal);

    app.setStyleSheet(buildStyleSheet(p));
}

Theme themeFromSettings(int value)
{
    switch (value) {
    case 1:
        return Theme::Light;
    case 2:
        return Theme::Dark;
    default:
        return Theme::System;
    }
}

int themeToSettings(Theme theme)
{
    switch (theme) {
    case Theme::Light:
        return 1;
    case Theme::Dark:
        return 2;
    case Theme::System:
        return 0;
    }
    return 0;
}

QString statusBadgePrefix(VerificationStatus status)
{
    switch (status) {
    case VerificationStatus::Match:
        return QStringLiteral("\u2713 ");
    case VerificationStatus::Mismatch:
        return QStringLiteral("\u2717 ");
    case VerificationStatus::Error:
        return QStringLiteral("\u26A0 ");
    case VerificationStatus::Cancelled:
        return QStringLiteral("\u2014 ");
    case VerificationStatus::Generated:
        return {};
    }
    return {};
}

QColor statusBadgeBackground(VerificationStatus status, const Palette& palette)
{
    QColor base;
    switch (status) {
    case VerificationStatus::Match:
        base = palette.statusMatch;
        break;
    case VerificationStatus::Mismatch:
        base = palette.statusMismatch;
        break;
    case VerificationStatus::Error:
        base = palette.statusError;
        break;
    case VerificationStatus::Cancelled:
        base = palette.statusCancelled;
        break;
    case VerificationStatus::Generated:
        base = palette.statusInfo;
        break;
    }

    if (palette.isDark) {
        QColor tint = base;
        tint.setAlpha(48);
        return tint;
    }
    QColor tint = base;
    tint.setAlpha(28);
    return tint;
}

QColor statusBadgeText(VerificationStatus status, const Palette& palette)
{
    switch (status) {
    case VerificationStatus::Match:
        return palette.statusMatch;
    case VerificationStatus::Mismatch:
        return palette.statusMismatch;
    case VerificationStatus::Error:
        return palette.statusError;
    case VerificationStatus::Cancelled:
        return palette.statusCancelled;
    case VerificationStatus::Generated:
        return palette.statusInfo;
    }
    return palette.text;
}

} // namespace iso
