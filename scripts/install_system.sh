#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

INSTALL_LLAMA_MODE="none"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --with-llama-server)
            INSTALL_LLAMA_MODE="prebuilt"
            shift
            ;;
        --build-llama-server)
            INSTALL_LLAMA_MODE="source"
            shift
            ;;
        -h|--help)
            cat <<EOF
Usage: $0 [--with-llama-server | --build-llama-server]

Options:
  --with-llama-server   Also install the patched llama-server (Q2_0C + Vulkan) to
                        /usr/local/bin/shadowbar-llama-server (downloads prebuilt
                        release archive first, or builds from source on fallback).
  --build-llama-server  Force compiling the patched llama-server from source.
EOF
            exit 0
            ;;
        *)
            echo "Unknown option: $1 (try --help)" >&2
            exit 1
            ;;
    esac
done

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

if [[ "${INSTALL_LLAMA_MODE}" != "none" ]]; then
    TMP_ENGINE="$(mktemp)"
    if [[ "${INSTALL_LLAMA_MODE}" == "prebuilt" ]]; then
        "${ROOT_DIR}/scripts/build_llama_server.sh" --prebuilt "${TMP_ENGINE}"
    else
        "${ROOT_DIR}/scripts/build_llama_server.sh" "${TMP_ENGINE}"
    fi
    sudo install -m 755 "${TMP_ENGINE}" /usr/local/bin/shadowbar-llama-server
    rm -f "${TMP_ENGINE}"
    echo "[ShadowBar] 已安装专版引擎到 /usr/local/bin/shadowbar-llama-server"
elif ! command -v shadowbar-llama-server >/dev/null 2>&1 &&
     ! command -v llama-server >/dev/null 2>&1 &&
     [[ ! -x "${XDG_DATA_HOME:-${HOME}/.local/share}/shadowbar/bin/llama-server" ]]; then
    echo
    echo "[提示] 尚未检测到支持 Q2_0C 的 llama-server（仅使用在线/词典模式可忽略）："
    echo "  · 一键安装专版引擎: $0 --with-llama-server"
    echo "  · 或用户目录编译安装: ${ROOT_DIR}/scripts/build_llama_server.sh"
fi

echo
echo "=== 系统级安装完成 ==="
echo "请执行以下命令重启 Fcitx5 使模块生效："
echo "  fcitx5 -r -d"
echo "随后可在“系统设置 / fcitx5-configtool -> 附加组件 -> ShadowBar Translator”中可视化配置。"
