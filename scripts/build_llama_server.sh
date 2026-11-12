#!/usr/bin/env bash
set -euo pipefail

# Build the patched llama-server binary (PR #22836: Q2_0C / STQ1_0 / hunyuan-dense)
# used by fcitx5-shadowbar's local Hy-MT2-1.8B (2bit) translator.
#
# Usage:
#   ./scripts/build_llama_server.sh [--prebuilt] [INSTALL_PATH]
#
# Options:
#   --prebuilt       Try downloading prebuilt x86_64 archive from SHADOWBAR_ENGINE_URL
#                    first, falling back to compiling from source on miss/failure.
#
# Environment overrides:
#   LLAMA_REPO_URL   Git repo URL (default: https://github.com/sjl623/llama.cpp.git)
#   LLAMA_COMMIT     Exact commit SHA to build (default: 1e411d8f5a1e23525fa3265dfb4bd76265465397)
#   SHADOWBAR_ENGINE_URL Release archive URL for --prebuilt
#   GGML_VULKAN      ON | OFF | AUTO (default: AUTO - enables Vulkan if glslc & vulkan headers exist)
#   GGML_NATIVE      ON | OFF (default: OFF for portable x86_64 binaries)
#   SHADOWBAR_LLAMA_SRC_DIR   Reuse an existing source checkout instead of cloning
#   SHADOWBAR_LLAMA_BUILD_DIR Custom build directory (default: /tmp/shadowbar-llama-build)

PREBUILT_FALLBACK="${PREBUILT_FALLBACK:-0}"
INSTALL_PATH=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --prebuilt)
            PREBUILT_FALLBACK=1
            shift
            ;;
        -h|--help)
            sed -n '4,18p' "$0"
            exit 0
            ;;
        *)
            INSTALL_PATH="$1"
            shift
            ;;
    esac
done

LLAMA_REPO_URL="${LLAMA_REPO_URL:-https://github.com/sjl623/llama.cpp.git}"
LLAMA_COMMIT="${LLAMA_COMMIT:-1e411d8f5a1e23525fa3265dfb4bd76265465397}"
ENGINE_URL="${SHADOWBAR_ENGINE_URL:-https://github.com/ang-XWBWZ/fcitx5-shadowbar/releases/latest/download/shadowbar-llama-server-linux-amd64.tar.gz}"
DEFAULT_INSTALL_PATH="${XDG_DATA_HOME:-${HOME}/.local/share}/shadowbar/bin/llama-server"
INSTALL_PATH="${INSTALL_PATH:-${DEFAULT_INSTALL_PATH}}"
SRC_DIR="${SHADOWBAR_LLAMA_SRC_DIR:-/tmp/shadowbar-llama-src}"
BUILD_DIR="${SHADOWBAR_LLAMA_BUILD_DIR:-/tmp/shadowbar-llama-build}"
GGML_VULKAN_MODE="${GGML_VULKAN:-AUTO}"
GGML_NATIVE_MODE="${GGML_NATIVE:-OFF}"

if [[ "${PREBUILT_FALLBACK}" == "1" && "$(uname -m)" == "x86_64" ]] && command -v curl >/dev/null 2>&1 && command -v tar >/dev/null 2>&1; then
    TMP_DIR="$(mktemp -d)"
    trap 'rm -rf "${TMP_DIR}"' EXIT
    echo "[prebuilt] Attempting prebuilt engine download from ${ENGINE_URL}..."
    if curl -fL --connect-timeout 10 --max-time 180 -o "${TMP_DIR}/engine.tar.gz" "${ENGINE_URL}" &&
       tar -xzf "${TMP_DIR}/engine.tar.gz" -C "${TMP_DIR}"; then
        EXTRACTED_BIN="$(find "${TMP_DIR}" -type f -name llama-server -print -quit || true)"
        if [[ -n "${EXTRACTED_BIN}" ]]; then
            install -Dm755 "${EXTRACTED_BIN}" "${INSTALL_PATH}"
            ls -lh "${INSTALL_PATH}"
            echo "Done (prebuilt). Installed Q2_0C + Vulkan llama-server -> ${INSTALL_PATH}"
            exit 0
        fi
    fi
    echo "[prebuilt] Prebuilt download unavailable; falling back to source build..."
fi

for cmd in git cmake c++; do
    if ! command -v "${cmd}" >/dev/null 2>&1; then
        echo "Error: missing required build tool '${cmd}'." >&2
        echo "Install build dependencies (Debian/Ubuntu): sudo apt install -y git cmake g++ libvulkan-dev glslc glslang-tools" >&2
        exit 1
    fi
done

USE_VULKAN="OFF"
case "${GGML_VULKAN_MODE}" in
    ON|on|1|true)
        if ! command -v glslc >/dev/null 2>&1; then
            echo "Error: GGML_VULKAN=ON requested, but 'glslc' was not found." >&2
            echo "Install Vulkan build dependencies: sudo apt install -y libvulkan-dev glslc glslang-tools" >&2
            exit 1
        fi
        USE_VULKAN="ON"
        ;;
    OFF|off|0|false)
        USE_VULKAN="OFF"
        ;;
    AUTO|auto)
        if command -v glslc >/dev/null 2>&1 && pkg-config --exists vulkan 2>/dev/null; then
            USE_VULKAN="ON"
        elif command -v glslc >/dev/null 2>&1 && [[ -f /usr/include/vulkan/vulkan.h ]]; then
            USE_VULKAN="ON"
        else
            echo "Note: glslc / Vulkan dev headers not found; building CPU-only llama-server (install libvulkan-dev + glslc to enable Vulkan GPU acceleration)."
            USE_VULKAN="OFF"
        fi
        ;;
    *)
        echo "Error: invalid GGML_VULKAN value '${GGML_VULKAN_MODE}' (expected ON, OFF, or AUTO)." >&2
        exit 1
        ;;
esac

echo "[1/4] Fetching llama.cpp (${LLAMA_COMMIT}) from ${LLAMA_REPO_URL}..."
if [[ ! -d "${SRC_DIR}/.git" ]]; then
    mkdir -p "${SRC_DIR}"
    git -C "${SRC_DIR}" init -q
    git -C "${SRC_DIR}" remote add origin "${LLAMA_REPO_URL}"
else
    git -C "${SRC_DIR}" remote set-url origin "${LLAMA_REPO_URL}"
fi
git -C "${SRC_DIR}" fetch --depth 1 origin "${LLAMA_COMMIT}"
git -C "${SRC_DIR}" checkout --detach -q FETCH_HEAD

echo "[2/4] Configuring CMake (GGML_VULKAN=${USE_VULKAN}, GGML_NATIVE=${GGML_NATIVE_MODE})..."
cmake -S "${SRC_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DGGML_VULKAN="${USE_VULKAN}" \
    -DGGML_NATIVE="${GGML_NATIVE_MODE}" \
    -DLLAMA_CURL=OFF \
    -DLLAMA_BUILD_TESTS=OFF \
    -DLLAMA_BUILD_SERVER=ON

echo "[3/4] Building target 'llama-server'..."
JOBS="$(nproc 2>/dev/null || echo 4)"
cmake --build "${BUILD_DIR}" --target llama-server -j "${JOBS}"

BIN_BUILT=""
for candidate in "${BUILD_DIR}/bin/llama-server" "${BUILD_DIR}/llama-server"; do
    if [[ -x "${candidate}" ]]; then
        BIN_BUILT="${candidate}"
        break
    fi
done
if [[ -z "${BIN_BUILT}" ]]; then
    echo "Error: llama-server binary not found under ${BUILD_DIR}" >&2
    exit 1
fi

if command -v strip >/dev/null 2>&1; then
    strip --strip-unneeded "${BIN_BUILT}" || true
fi

echo "[4/4] Installing llama-server -> ${INSTALL_PATH}"
install -Dm755 "${BIN_BUILT}" "${INSTALL_PATH}"
ls -lh "${INSTALL_PATH}"
echo "Done. You can now select or auto-start Hy-MT2-1.8B (2bit) inside ShadowBar Settings."
