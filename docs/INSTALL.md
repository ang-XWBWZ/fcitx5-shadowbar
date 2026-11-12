# Installation & Setup Guide

**Language:** English | [简体中文](INSTALL.zh-CN.md)

This guide covers installing **fcitx5-shadowbar** (`libshadowbar.so`, `libshadowbarim.so`, `shadowbar-float`, and `shadowbar-sim`) from source or Debian package, setting up Wayland/X11 Chinese IME environments, and configuring optional offline models and dictionaries.

---

## 1. Prerequisites & System Dependencies

### Arch Linux / Manjaro

```bash
# Build dependencies
sudo pacman -S --needed base-devel cmake pkgconf fcitx5 curl nlohmann-json qt6-base sqlite zlib

# Recommended Chinese IME + CJK fonts + Wayland clipboard runtime
sudo pacman -S --needed fcitx5-chinese-addons fcitx5-configtool fcitx5-gtk fcitx5-qt \
  noto-fonts noto-fonts-cjk noto-fonts-emoji wl-clipboard
```

### Debian 12+ / Ubuntu 24.04+

```bash
# Build dependencies
sudo apt update
sudo apt install -y cmake g++ pkgconf debhelper dpkg-dev \
  libfcitx5core-dev libfcitx5config-dev libfcitx5utils-dev fcitx5-modules-dev \
  libcurl4-openssl-dev nlohmann-json3-dev qt6-base-dev libsqlite3-dev zlib1g-dev

# Recommended Chinese IME + CJK fonts + Wayland clipboard runtime
sudo apt install -y fcitx5 fcitx5-chinese-addons fcitx5-config-qt \
  fcitx5-frontend-gtk3 fcitx5-frontend-gtk4 fcitx5-frontend-qt5 fcitx5-frontend-qt6 \
  fonts-noto-cjk fonts-noto-color-emoji im-config wl-clipboard locales
```

### Fedora / RHEL

```bash
# Build dependencies
sudo dnf install -y cmake gcc-c++ pkgconf-pkg-config fcitx5-devel \
  libcurl-devel json-devel qt6-qtbase-devel sqlite-devel zlib-devel

# Recommended Chinese IME + CJK fonts + Wayland clipboard runtime
sudo dnf install -y fcitx5 fcitx5-chinese-addons fcitx5-configtool fcitx5-gtk fcitx5-qt \
  google-noto-sans-cjk-fonts google-noto-sans-mono-cjk-fonts google-noto-emoji-color-fonts wl-clipboard
```

---

## 2. Clone, Build & Verify

```bash
git clone https://github.com/ang-XWBWZ/fcitx5-shadowbar.git
cd fcitx5-shadowbar

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure -V
```

Expected test result: `test_core`, `test_fcitx5_integration`, and `test_floating_ui_helpers` all pass (`3/3`).

---

## 3. Install

### Option A: User-Local Installation (Recommended for Development & Single-User Wayland/X11)

Installs binaries and addon `.so` files under `~/.local` without `sudo`:

```bash
./scripts/install_user.sh
```

Make sure Fcitx5 can discover `~/.local/lib/fcitx5` via `FCITX_ADDON_DIRS` (handled automatically if you run the built-in Chinese Environment Fixer in Step 4).

### Option B: System-Wide Installation

Installs cleanly into `/usr` (default) or another `PREFIX`:

```bash
sudo ./scripts/install_system.sh
```

### Option C: Build a Debian/Ubuntu `.deb` Package

```bash
dpkg-buildpackage -us -uc -b
sudo apt install ../fcitx5-shadowbar_0.1.0-1_*.deb
```

---

## 4. First-Time Linux Host Setup (Built-in One-Click Fixer)

If you are setting up Chinese input on a fresh Linux desktop (KDE Plasma Wayland, GNOME Wayland, or X11), launch the settings window directly:

```bash
shadowbar-float --settings
```

Open **Tab 4: `中文环境修复` (Chinese Environment Fixer)**:

1. Click **重新检测 (Re-check)** to inspect Fcitx5 packages, CJK monospace font resolution (`fc-match monospace:charset=4e2d`), `~/.config/fcitx5/profile`, systemd/D-Bus Wayland variables, `~/.xprofile` X11 variables, and optional `zh_CN.UTF-8` locale status.
2. Click **一键全部修复 (Fix All)** to:
   - Install any missing Fcitx5 frontend/addon/font/`wl-clipboard` packages via `pkexec` or interactive terminal prompt.
   - Configure `~/.config/fontconfig/conf.d/` monospace CJK fallback without hijacking monospace glyphs.
   - Ensure `~/.config/fcitx5/profile` contains `keyboard-us`, `pinyin`, and `shadowbar`.
   - Write cross-session IME environment variables to both Wayland (`~/.config/environment.d/` + `dbus-update-activation-environment`) and X11 (`~/.xprofile`).
3. Log out and back in once so GTK/Qt/Wayland/X11 apps pick up all session environment variables, then run:

```bash
./scripts/restart_fcitx5.sh
```

---

## 5. Configuration & Key Shortcuts

On first launch, ShadowBar creates `~/.config/shadowbar/config.toml` (or copy [config/config.toml.example](../config/config.toml.example)). Edits reload automatically without restarting Fcitx5.

### Default Shortcuts

| Action | Default Binding | Notes |
| --- | --- | --- |
| Inline One-Shot translate | `;;` | Type `;;` while in CJK IM; `Enter` commits translation, `Shift+Enter` commits original text |
| Inline Sticky toggle | `Control+Shift+T` | Persistent companion translation bar across sentences |
| Cycle target language | `Alt+Shift+L` | Cycles through `cycle_languages` (default `["en", "zh", "ja"]`) |
| Cycle preset | `Alt+Shift+P` | Switches translation style / backend preset |
| Floating window toggle | `Alt+Shift+Q` | Toggles `shadowbar-float` |

### `shadowbar-float` CLI Actions

```text
shadowbar-float [--toggle|--show|--settings|--sync-orb|--reset|--quit]
```

### Testing Without Restarting Fcitx5

Use the interactive terminal simulator:

```bash
./build/cli-sim/shadowbar-sim
```

---

## 6. Optional: Background Wayland/X11 Clipboard Monitor

In `shadowbar-float --settings` (Tab 2) or `[floating]` in `config.toml`, enable `watch_clipboard = true`.

- **Wayland**: Install `wl-clipboard` (`wl-paste --watch`). ShadowBar listens for clipboard updates in the background without requiring window focus or pinning, and also integrates with KDE Klipper D-Bus (`clipboardHistoryUpdated`).
- **Safety filters**: Password manager sensitive hints, local file URIs/paths (`file://`, `/home/...`), URLs (`https://...`), Markdown links, and binary non-text MIME payloads are automatically skipped.

---

## 7. Optional: Local Hunyuan HY-MT1.5-1.8B Model (`1.25bit` / `2bit`)

1. Install or build `llama-server` (`llama.cpp`) on your system or place it at `~/.local/share/shadowbar/bin/llama-server`.
2. Open `shadowbar-float --settings` -> **Tab 3: `混元 HY-MT1.5 (1.25bit/2bit)`**.
3. Choose **1.25bit (`STQ1_0`, ~441 MB)** or **2bit (`Q2_0C`, ~573 MB)** (or point to a custom `.gguf`), pick **HuggingFace** or **HF-Mirror**, download, and start the local server.
4. Set backend to **`auto`** (Vulkan GPU preferred with automatic CPU fallback), **`vulkan`**, or **`cpu`**.

---

## 8. Optional: Multilingual Offline Dictionaries

ShadowBar searches `~/.local/share/shadowbar/` for legacy filenames (`ecdict.db`, `ecdict.sqlite`, `ecdict.sqlite3`, `ecdict.csv`, `cedict_ts.u8`) and also loads any number of dictionaries declared under `[dicts.<id>]` in `config.toml` or via Settings.

Supported formats:
- **ECDICT SQLite / CSV**: `.db`, `.sqlite`, `.sqlite3`, `.csv`, `.csv.gz`
- **StarDict**: `.ifo`, `.idx`, `.idx.gz`, `.dict`, `.dict.dz`, `.dict.gz`
- **CC-CEDICT**: `.u8`, `.u8.gz`
- **JSON / JSONL**: `.json`, `.jsonl`, `.jsonl.gz`
- **Delimited text**: `.tsv`, `.txt`

---

## 9. Troubleshooting

- **Floating window does not respond to Wayland background copy**: Verify `wl-paste --version` works in terminal and `watch_clipboard = true` is enabled.
- **Fcitx5 does not list ShadowBar after user-local install**: Check `FCITX_ADDON_DIRS` includes `~/.local/lib/fcitx5` and run `./scripts/restart_fcitx5.sh`.
- **Vulkan GPU offload fails on local HY-MT1.5**: Switch backend to `auto` (falls back to CPU) or verify your Vulkan ICD drivers (`vulkaninfo`).
