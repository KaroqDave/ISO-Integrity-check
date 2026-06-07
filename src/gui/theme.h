#pragma once

#include "core/verifier.h"

#include <QColor>
#include <QString>

class QApplication;

namespace iso {

enum class Theme {
    System,
    Light,
    Dark,
};

enum class ColorScheme {
    Light,
    Dark,
};

struct Palette {
    QColor background;
    QColor surface;
    QColor surfaceAlt;
    QColor border;
    QColor text;
    QColor mutedText;
    QColor accent;
    QColor accentHover;
    QColor accentPressed;
    QColor accentText;
    QColor statusMatch;
    QColor statusError;
    QColor statusInfo;
    bool isDark = false;
};

Palette lightPalette();
Palette darkPalette();

ColorScheme resolveColorScheme(Theme theme);
const Palette& paletteFor(ColorScheme scheme);

QString buildStyleSheet(const Palette& palette);

// Applies the Fusion base style, a matching QPalette, and the generated
// stylesheet for the resolved color scheme.
void applyTheme(QApplication& app, Theme theme);

// Returns the badge colors (background, text) for a verification status under
// the supplied palette.
QColor statusBadgeBackground(VerificationStatus status, const Palette& palette);
QColor statusBadgeText(VerificationStatus status, const Palette& palette);

} // namespace iso
