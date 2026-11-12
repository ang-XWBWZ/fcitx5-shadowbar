#pragma once

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QString>

namespace shadowbar::floating_ui {

struct ThemeTokens {
    bool dark = true;
    QColor surface;
    QColor surface_elevated;
    QColor surface_input;
    QColor border;
    QColor text_primary;
    QColor text_secondary;
    QColor accent;
    QColor accent_text;
    QColor accent_soft;
    QColor hover_bg;
    QColor pill_bg;
    QColor orb_bg;
    QColor orb_border;
};

inline ThemeTokens resolveThemeTokens(const QPalette &pal = QApplication::palette()) {
    ThemeTokens t;
    const QColor win = pal.color(QPalette::Window);
    const QColor hl = pal.color(QPalette::Highlight);
    t.dark = win.lightness() < 128;

    t.accent = hl.isValid() ? hl : QColor(t.dark ? "#3daee9" : "#2980b9");
    t.accent_text = QColor("#ffffff");
    t.accent_soft = QColor(t.accent.red(), t.accent.green(), t.accent.blue(),
                           t.dark ? 42 : 28);

    if (t.dark) {
        t.surface = QColor(34, 37, 43, 242);
        t.surface_elevated = QColor(42, 46, 54, 248);
        t.surface_input = QColor(24, 27, 32, 245);
        t.border = QColor(255, 255, 255, 32);
        t.text_primary = QColor(239, 241, 245);
        t.text_secondary = QColor(166, 173, 186);
        t.hover_bg = QColor(255, 255, 255, 22);
        t.pill_bg = QColor(255, 255, 255, 16);
        t.orb_bg = QColor(32, 36, 44, 224);
        t.orb_border = QColor(t.accent.red(), t.accent.green(), t.accent.blue(), 170);
    } else {
        t.surface = QColor(250, 251, 253, 246);
        t.surface_elevated = QColor(255, 255, 255, 252);
        t.surface_input = QColor(243, 245, 248, 250);
        t.border = QColor(0, 0, 0, 28);
        t.text_primary = QColor(30, 34, 41);
        t.text_secondary = QColor(102, 110, 125);
        t.hover_bg = QColor(0, 0, 0, 14);
        t.pill_bg = QColor(0, 0, 0, 10);
        t.orb_bg = QColor(252, 253, 255, 232);
        t.orb_border = QColor(t.accent.red(), t.accent.green(), t.accent.blue(), 180);
    }
    return t;
}

inline QString cssRgba(const QColor &c) {
    return QString("rgba(%1,%2,%3,%4)")
        .arg(c.red())
        .arg(c.green())
        .arg(c.blue())
        .arg(c.alpha());
}

} // namespace shadowbar::floating_ui
