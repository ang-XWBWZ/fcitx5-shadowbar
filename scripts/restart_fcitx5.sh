#!/usr/bin/env bash
set -euo pipefail

ADDON_DIRS="${HOME}/.local/lib/fcitx5:/usr/lib/fcitx5"

if command -v systemctl >/dev/null 2>&1; then
    systemctl --user set-environment "FCITX_ADDON_DIRS=${ADDON_DIRS}" || true
fi
if command -v dbus-update-activation-environment >/dev/null 2>&1; then
    dbus-update-activation-environment --systemd "FCITX_ADDON_DIRS=${ADDON_DIRS}" || true
fi

if command -v shadowbar-float >/dev/null 2>&1; then
    shadowbar-float --quit >/dev/null 2>&1 || true
elif [[ -x "${HOME}/.local/bin/shadowbar-float" ]]; then
    "${HOME}/.local/bin/shadowbar-float" --quit >/dev/null 2>&1 || true
fi

if pgrep -x fcitx5 >/dev/null 2>&1 && command -v qdbus6 >/dev/null 2>&1; then
    qdbus6 org.fcitx.Fcitx5 /controller org.fcitx.Fcitx.Controller1.Restart || true
    sleep 0.6
else
    FCITX_ADDON_DIRS="${ADDON_DIRS}" fcitx5 -r -d
fi

# Wait briefly for Fcitx5 D-Bus name to come back online.
if command -v qdbus6 >/dev/null 2>&1; then
    for _ in $(seq 1 20); do
        if qdbus6 org.fcitx.Fcitx5 /controller org.fcitx.Fcitx.Controller1.CurrentInputMethod >/dev/null 2>&1; then
            break
        fi
        sleep 0.2
    done
fi

# On KDE Plasma Wayland with KWin VirtualKeyboard, re-attach privileged WAYLAND_SOCKET
# via fcitx5-wayland-launcher --reopen so native Wayland apps keep working.
if [[ "${XDG_SESSION_TYPE:-}" == "wayland" ]] &&
   command -v kwriteconfig6 >/dev/null 2>&1 &&
   command -v kreadconfig6 >/dev/null 2>&1; then
    CURRENT_IM="$(kreadconfig6 --file kwinrc --group Wayland --key InputMethod 2>/dev/null || true)"
    if [[ "${CURRENT_IM}" == *"fcitx5-wayland-launcher.desktop"* ]]; then
        kwriteconfig6 --file kwinrc --group Wayland --key InputMethod --notify ""
        sleep 0.5
        kwriteconfig6 --file kwinrc --group Wayland --key InputMethod --notify "/usr/share/applications/fcitx5-wayland-launcher.desktop"
    fi
fi

echo "[ShadowBar] Fcitx5 已安全重启并重新连接 Wayland/X11 前端。"
