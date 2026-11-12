#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
cmake --build "${BUILD_DIR}" -j

LIB_DEST="${HOME}/.local/lib/fcitx5"
ADDON_CONF_DEST="${XDG_DATA_HOME:-${HOME}/.local/share}/fcitx5/addon"
IM_CONF_DEST="${XDG_DATA_HOME:-${HOME}/.local/share}/fcitx5/inputmethod"
BIN_DEST="${HOME}/.local/bin"
CFG_DEST="${XDG_CONFIG_HOME:-${HOME}/.config}/shadowbar"
ENVD_DEST="${XDG_CONFIG_HOME:-${HOME}/.config}/environment.d"

mkdir -p "${LIB_DEST}" "${ADDON_CONF_DEST}" "${IM_CONF_DEST}" "${BIN_DEST}" "${CFG_DEST}" "${ENVD_DEST}"

install -m 755 "${BUILD_DIR}/fcitx5-addon/libshadowbar.so" "${LIB_DEST}/libshadowbar.so"
install -m 755 "${BUILD_DIR}/fcitx5-addon/libshadowbarim.so" "${LIB_DEST}/libshadowbarim.so"
install -m 644 "${BUILD_DIR}/fcitx5-addon/shadowbar.conf" "${ADDON_CONF_DEST}/shadowbar.conf"
install -m 644 "${BUILD_DIR}/fcitx5-addon/shadowbarim.conf" "${ADDON_CONF_DEST}/shadowbarim.conf"
install -m 644 "${ROOT_DIR}/fcitx5-addon/inputmethod-shadowbar.conf" "${IM_CONF_DEST}/shadowbar.conf"
install -m 755 "${BUILD_DIR}/cli-sim/shadowbar-sim" "${BIN_DEST}/shadowbar-sim"
if [[ -f "${BUILD_DIR}/floating-ui/shadowbar-float" ]]; then
    APP_DEST="${XDG_DATA_HOME:-${HOME}/.local/share}/applications"
    mkdir -p "${APP_DEST}"
    install -m 755 "${BUILD_DIR}/floating-ui/shadowbar-float" "${BIN_DEST}/shadowbar-float"
    install -m 644 "${ROOT_DIR}/floating-ui/fcitx5-shadowbar-float.desktop" "${APP_DEST}/fcitx5-shadowbar-float.desktop"
    # Stop any stale background shadowbar-float instance so next hotkey/menu opens the updated binary.
    if pgrep -x shadowbar-float >/dev/null 2>&1; then
        "${BIN_DEST}/shadowbar-float" --quit >/dev/null 2>&1 || true
        sleep 0.2
        pkill -x shadowbar-float >/dev/null 2>&1 || true
        echo "[ShadowBar] 已停止旧版悬浮窗后台进程，下次呼出将自动使用新版本"
    fi
fi

if [[ ! -f "${CFG_DEST}/config.toml" ]]; then
    install -m 644 "${ROOT_DIR}/config/config.toml.example" "${CFG_DEST}/config.toml"
    echo "[ShadowBar] 已生成默认配置: ${CFG_DEST}/config.toml"
else
    echo "[ShadowBar] 保留现有配置: ${CFG_DEST}/config.toml"
fi

ENV_FILE="${ENVD_DEST}/fcitx5-shadowbar.conf"
ADDON_DIRS_VAL="${LIB_DEST}:/usr/lib/fcitx5"
cat > "${ENV_FILE}" <<EOF
# Added by fcitx5-shadowbar install_user.sh so Fcitx5 loads user-local addons.
FCITX_ADDON_DIRS=${ADDON_DIRS_VAL}
EOF

echo
echo "=== 用户级安装完成 (无需 root) ==="
echo "插件库:   ${LIB_DEST}/libshadowbar.so , ${LIB_DEST}/libshadowbarim.so"
echo "插件声明: ${ADDON_CONF_DEST}/shadowbar.conf , shadowbarim.conf"
echo "输入法项: ${IM_CONF_DEST}/shadowbar.conf (翻译输入 (ShadowBar))"
echo "沙盒CLI:  ${BIN_DEST}/shadowbar-sim"
echo "配置文件: ${CFG_DEST}/config.toml"
echo "环境文件: ${ENV_FILE}"
echo
echo "下一步激活方式："
echo "  1. 立即安全重启并保持 Wayland 连接: ${ROOT_DIR}/scripts/restart_fcitx5.sh"
echo "  2. 长期生效: 重新登录 KDE 会话（systemd environment.d 将自动生效）"
echo "     或在 fcitx5-configtool -> 插件 -> ShadowBar Translator 中调整热键与引擎"
