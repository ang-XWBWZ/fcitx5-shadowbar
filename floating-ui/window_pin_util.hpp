#pragma once

#include <QList>
#include <QPoint>
#include <QSize>
#include <QString>

#include <functional>

class QWidget;

namespace shadowbar::floating_ui {

/** Native stacking, geometry & desktop-behavior options for top-level floating widgets. */
struct PinWindowOptions {
    bool keep_above = false;
    bool on_all_desktops = false;
    bool skip_taskbar = false;
    bool skip_switcher = false;
    bool apply_position = false;
    QPoint target_pos{};
    bool apply_size = false;
    QSize target_size{};
};

/** Returns true when the current Qt platform plugin is Wayland. */
bool isWaylandPlatform();

/**
 * Synchronizes always-on-top state (and optionally KWin Wayland position)
 * with the active windowing backend.
 * - On KDE Plasma Wayland (KWin): invokes an ephemeral KWin D-Bus script
 *   matching this process's pid and window caption to toggle Window::keepAbove
 *   and optional Window::frameGeometry placement.
 * - On X11 / other platforms: keeps Qt::WindowStaysOnTopHint synced directly.
 */
void syncNativeWindowPinState(QWidget *widget, const PinWindowOptions &opts);

/**
 * Schedules short post-map synchronization retries so Wayland compositors
 * receive keepAbove and remembered position once the newly mapped xdg_toplevel surface exists.
 */
void scheduleNativeWindowPinSync(QWidget *widget, const PinWindowOptions &opts);

/**
 * Requests compositor global-coordinate geometry snapshots on KDE Plasma Wayland.
 * Invokes `dbus_method(caption: QString, rect_csv: QString)` via KWin callDBus
 * for each visible widget in `widgets`, then calls `on_dispatched(true)` (or `false`
 * immediately if KWin scripting is unavailable).
 */
void requestKWinWindowGeometrySnapshots(
    const QList<QWidget *> &widgets,
    const QString &dbus_service,
    const QString &dbus_path,
    const QString &dbus_iface,
    const QString &dbus_method,
    std::function<void(bool dispatched)> on_dispatched = {});

} // namespace shadowbar::floating_ui
