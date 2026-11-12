#pragma once

#include <QColor>
#include <QGuiApplication>
#include <QPalette>
#include <QString>
#include <QStyleHints>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <optional>

namespace shadowbar::floating_ui {

struct ThemeTokens {
    bool dark = true;
    QColor surface;
    QColor surface_elevated;
    QColor surface_input;
    QColor card_strip;
    QColor border;
    QColor divider;
    QColor text_primary;
    QColor text_secondary;
    QColor accent;
    QColor accent_hover;
    QColor accent_text;
    QColor accent_soft;
    QColor hover_bg;
    QColor pill_bg;
    QColor orb_bg;
    QColor orb_border;
    // Always 100% opaque (alpha == 255) for QComboBox popups, QMenu & tooltips
    QColor popup_bg;
    QColor popup_border;
    QColor popup_hover;
};

namespace detail {

inline QColor blendColor(const QColor &a, const QColor &b, qreal t) {
    const qreal clamped = std::clamp(t, 0.0, 1.0);
    const int r = qRound(a.red() * (1.0 - clamped) + b.red() * clamped);
    const int g = qRound(a.green() * (1.0 - clamped) + b.green() * clamped);
    const int bl = qRound(a.blue() * (1.0 - clamped) + b.blue() * clamped);
    return QColor(r, g, bl);
}

inline QColor withAlpha(QColor c, int alpha) {
    c.setAlpha(std::clamp(alpha, 0, 255));
    return c;
}

inline qreal relativeLuminance(const QColor &c) {
    auto lin = [](qreal ch) {
        return (ch <= 0.03928) ? (ch / 12.92)
                               : std::pow((ch + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(c.redF()) + 0.7152 * lin(c.greenF()) +
           0.0722 * lin(c.blueF());
}

} // namespace detail

inline bool detectSystemDarkTheme(
    const QPalette &pal = QGuiApplication::palette(),
    std::optional<bool> dark_override = std::nullopt) {
    if (dark_override.has_value()) {
        return *dark_override;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (QGuiApplication::instance() && QGuiApplication::styleHints()) {
        const Qt::ColorScheme scheme =
            QGuiApplication::styleHints()->colorScheme();
        if (scheme == Qt::ColorScheme::Dark) {
            return true;
        }
        if (scheme == Qt::ColorScheme::Light) {
            return false;
        }
    }
#endif
    const QColor win = pal.color(QPalette::Window);
    return !win.isValid() || win.lightness() < 128;
}

inline ThemeTokens resolveThemeTokens(
    const QPalette &pal = QGuiApplication::palette(),
    std::optional<bool> dark_override = std::nullopt) {
    ThemeTokens t;
    t.dark = detectSystemDarkTheme(pal, dark_override);

    const QColor sys_win = pal.color(QPalette::Window);
    const QColor sys_base = pal.color(QPalette::Base);
    const QColor sys_alt = pal.color(QPalette::AlternateBase);
    const QColor sys_btn = pal.color(QPalette::Button);
    const QColor sys_txt = pal.color(QPalette::WindowText);
    const QColor sys_sub = pal.color(QPalette::PlaceholderText);
    const QColor sys_hl = pal.color(QPalette::Highlight);

    const bool win_matches =
        sys_win.isValid() &&
        ((t.dark && sys_win.lightness() < 125) ||
         (!t.dark && sys_win.lightness() > 130));
    const bool base_matches =
        sys_base.isValid() &&
        ((t.dark && sys_base.lightness() < 120) ||
         (!t.dark && sys_base.lightness() > 135));

    const QColor default_accent(t.dark ? "#4ea1ff" : "#2563eb");
    const bool usable_hl = sys_hl.isValid() && sys_hl.alpha() > 0 &&
                           (sys_hl.saturation() > 12 ||
                            (sys_hl.lightness() > 45 && sys_hl.lightness() < 215));
    t.accent = usable_hl ? QColor(sys_hl.rgb()) : default_accent;
    // Keep accent distinguishable against dark or light surfaces.
    if (t.dark && t.accent.lightness() < 95) {
        t.accent = t.accent.lighter(132);
    } else if (!t.dark && t.accent.lightness() > 175) {
        t.accent = t.accent.darker(125);
    }
    t.accent_hover =
        t.dark ? t.accent.lighter(114) : t.accent.darker(110);
    t.accent_text = detail::relativeLuminance(t.accent) > 0.45
                        ? QColor("#111827")
                        : QColor("#ffffff");
    t.accent_soft = detail::withAlpha(t.accent, t.dark ? 44 : 28);

    const QColor opaque_surface =
        win_matches ? QColor(sys_win.rgb())
                    : (t.dark ? QColor(30, 34, 42) : QColor(246, 248, 252));
    const QColor opaque_input =
        base_matches ? QColor(sys_base.rgb())
                     : (t.dark ? QColor(22, 25, 31) : QColor(255, 255, 255));
    QColor opaque_elevated =
        (sys_alt.isValid() &&
         ((t.dark && sys_alt.lightness() < 130) ||
          (!t.dark && sys_alt.lightness() > 130)))
            ? QColor(sys_alt.rgb())
            : detail::blendColor(opaque_surface,
                                 t.dark ? QColor(255, 255, 255) : QColor(255, 255, 255),
                                 t.dark ? 0.05 : 0.65);
    if (!t.dark && opaque_elevated.lightness() < opaque_surface.lightness()) {
        opaque_elevated = QColor(255, 255, 255);
    }
    if (t.dark && opaque_elevated.lightness() < opaque_surface.lightness() + 4) {
        opaque_elevated =
            detail::blendColor(opaque_surface, QColor(255, 255, 255), 0.07);
    }

    const QColor opaque_strip =
        (sys_btn.isValid() &&
         ((t.dark && sys_btn.lightness() < 135) ||
          (!t.dark && sys_btn.lightness() > 130)))
            ? detail::blendColor(opaque_surface, QColor(sys_btn.rgb()), 0.55)
            : detail::blendColor(opaque_surface,
                                 t.dark ? QColor(255, 255, 255) : QColor(0, 0, 0),
                                 t.dark ? 0.045 : 0.035);

    const bool txt_matches =
        sys_txt.isValid() &&
        ((t.dark && sys_txt.lightness() > 165) ||
         (!t.dark && sys_txt.lightness() < 110));
    t.text_primary =
        txt_matches ? QColor(sys_txt.rgb())
                    : (t.dark ? QColor(238, 241, 247) : QColor(24, 28, 36));
    t.text_secondary =
        (sys_sub.isValid() &&
         std::abs(sys_sub.lightness() - opaque_surface.lightness()) > 45)
            ? detail::blendColor(opaque_surface, QColor(sys_sub.rgb()), 0.85)
            : detail::blendColor(t.text_primary, opaque_surface, 0.42);

    t.surface = detail::withAlpha(opaque_surface, t.dark ? 245 : 249);
    t.surface_elevated = detail::withAlpha(opaque_elevated, t.dark ? 250 : 254);
    t.surface_input = detail::withAlpha(opaque_input, t.dark ? 248 : 252);
    t.card_strip = detail::withAlpha(opaque_strip, t.dark ? 236 : 244);

    if (t.dark) {
        t.border = QColor(255, 255, 255, 34);
        t.divider = QColor(255, 255, 255, 22);
        t.hover_bg = QColor(255, 255, 255, 24);
        t.pill_bg = QColor(255, 255, 255, 16);
        t.orb_bg = detail::withAlpha(opaque_surface, 232);
        t.orb_border = detail::withAlpha(t.accent, 185);
    } else {
        t.border = QColor(15, 23, 42, 30);
        t.divider = QColor(15, 23, 42, 18);
        t.hover_bg = QColor(15, 23, 42, 16);
        t.pill_bg = QColor(15, 23, 42, 10);
        t.orb_bg = detail::withAlpha(opaque_elevated, 240);
        t.orb_border = detail::withAlpha(t.accent, 190);
    }

    // Solid opaque dropdown/menu/tooltip colors (never translucent over translucent parent).
    t.popup_bg = opaque_elevated;
    t.popup_bg.setAlpha(255);
    t.popup_border = detail::blendColor(
        t.popup_bg, t.text_primary, t.dark ? 0.18 : 0.15);
    t.popup_border.setAlpha(255);
    t.popup_hover =
        detail::blendColor(t.popup_bg, t.accent, t.dark ? 0.24 : 0.16);
    t.popup_hover.setAlpha(255);
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
