#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
cmake --build "${BUILD_DIR}" -j

echo "[ShadowBar] 安装 Fcitx5 模块到系统目录 (需要 sudo)..."
sudo cmake --install "${BUILD_DIR}"

if [[ -f "${BUILD_DIR}/cli-sim/shadowbar-sim" ]]; then
    sudo install -m 755 "${BUILD_DIR}/cli-sim/shadowbar-sim" /usr/local/bin/shadowbar-sim
fi

CFG_DEST="${XDG_CONFIG_HOME:-${HOME}/.config}/shadowbar"
mkdir -p "${CFG_DEST}"
if [[ ! -f "${CFG_DEST}/config.toml" ]]; then
    install -m 644 "${ROOT_DIR}/config/config.toml.example" "${CFG_DEST}/config.toml"
    echo "[ShadowBar] 已生成默认配置: ${CFG_DEST}/config.toml"
fi

echo
echo "=== 系统级安装完成 ==="
echo "请执行以下命令重启 Fcitx5 使模块生效："
echo "  fcitx5 -r -d"
echo "随后可在“系统设置 / fcitx5-configtool -> 附加组件 -> ShadowBar Translator”中可视化配置。"
