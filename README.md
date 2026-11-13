# fcitx5-shadowbar

[![CI Build & Deb](https://github.com/ang-XWBWZ/fcitx5-shadowbar/actions/workflows/ci-deb.yml/badge.svg)](https://github.com/ang-XWBWZ/fcitx5-shadowbar/actions/workflows/ci-deb.yml)
[![Latest Release](https://img.shields.io/github/v/release/ang-XWBWZ/fcitx5-shadowbar?label=Release)](https://github.com/ang-XWBWZ/fcitx5-shadowbar/releases/latest)
[![Code License: PolyForm Noncommercial 1.0.0](https://img.shields.io/badge/Code%20License-PolyForm--Noncommercial--1.0.0-orange.svg)](LICENSES/PolyForm-Noncommercial-1.0.0.txt)
[![Docs License: CC BY-NC-SA 4.0](https://img.shields.io/badge/Docs%20License-CC%20BY--NC--SA%204.0-lightgrey.svg)](LICENSES/CC-BY-NC-SA-4.0.txt)

**Language:** English | [简体中文](README.zh-CN.md)

**A dual-mode real-time translator for Linux (Wayland / X11): Fcitx5 Inline Companion Translation + Smart Qt6 Floating Translation Popup (`shadowbar-float`).**  
Translate seamlessly while typing inside any Chinese/CJK input method, or pop up an instant clipboard/on-demand translator window anywhere — powered by **20 languages**, **cloud/local LLMs**, local **Hunyuan `Hy-MT2-1.8B` (`2bit` Q2_0C)**, and **multi-format millisecond offline dictionaries**.

> **Non-Commercial Open Source:** Source code & scripts are licensed under [PolyForm Noncommercial License 1.0.0](LICENSES/PolyForm-Noncommercial-1.0.0.txt); documentation & example configs are licensed under [CC BY-NC-SA 4.0](LICENSES/CC-BY-NC-SA-4.0.txt). See [LICENSE](LICENSE).

---

## ✨ Dual Flagship Translation Modes

### 1. Smart Floating Translation Popup & Desktop Orb (`shadowbar-float`)

A fast, always-ready Qt6 translation popup tailored for Wayland (KDE Plasma / GNOME / wlroots) and X11 reading, writing, and clipboard lookup workflows:

```text
╭───────────────────────────────────────────────────────────────────╮
│ 🌐 [Auto Detect ▾] ⇄ [English ▾]  [Natural ▾]  👁Watch  🎯Focus 📌 ⚙│
├───────────────────────────────────────────────────────────────────┤
│  Clipboard monitoring works seamlessly across Wayland and X11.    │
│                                              63 chars · Auto EN→ZH│
├───────────────────────────────────────────────────────────────────┤
│  ✦ 剪贴板监听可在 Wayland 和 X11 上无缝工作。                      │
├───────────────────────────────────────────────────────────────────┤
│  Esc Hide · Ctrl+Enter Copy & Hide     [⇅ Back-Translate] [📋] [↵] │
╰───────────────────────────────────────────────────────────────────╯
```

- **Instant Global Toggle & Desktop Orb**: Press `Alt+Shift+Q` from any app to show/hide the popup, or enable the draggable floating desktop orb (left-click expand/collapse, right-click quick actions).
- **Popup-Only Unfocused Clipboard Watch (Zero Background Listeners)**: Combines `wl-paste --watch`, KDE Klipper D-Bus signals, and `QClipboard` **only while the popup window is open** so copied text translates without focusing or pinning, then cleanly disconnects all listeners and probes the instant you hide or close the popup.
- **Built-in Clipboard Safety Guards**: Automatically skips password/sensitive hints, local file URIs/paths (`/home/...`, `file://`), URLs, Markdown links, and binary non-text payloads.
- **Popup Productivity Features**:
  - **20-Language Auto-Detection & Quick Swap**: Automatically detects source language and flips direction (`Alt+S` swaps languages, `Alt+V` pulls current clipboard).
  - **⇅ Back-Translation Verification (`Alt+R`)**: Moves the translation back into the source pane and re-translates to the original language so you can double-check tone and nuance before sending.
  - **Smart Long-Text Batching**: Splits long passages using word counting for Latin scripts and character counting for CJK (`segment_max_chars`), translating progressively on demand or scroll.
  - **Result-Only Focus Mode & Ergonomic Controls**: `Alt+M` toggles Result-Only card view, `Ctrl+R` / `F5` forces cache-bypass retranslation, `Ctrl+Wheel` scales typography (`80%–160%`), `Ctrl+Shift+C` copies translation, `Ctrl+Enter` copies & hides, `Esc` dismisses.

### 2. Inline Fcitx5 Companion Bar & Virtual Input Method

Attach a non-blocking live translation overlay right next to Pinyin/Wubi preedit candidates without interrupting your typing flow:

```text
  [Target App] Typing normally -> press ;; to start inline translation:
  ┌──────────────────────────────────────────────────────────┐
  │ [ZH→EN · Natural] 今天把接口文档同步一下│                 │
  │ ✦ Let's sync the API documentation today.     · Ready    │
  ├──────────────────────────────────────────────────────────┤
  │  ↵ Enter Commit Translation   ⇧ Shift+Enter Commit Raw   │
  └──────────────────────────────────────────────────────────┘
```

- **One-Shot (`;;`)**: Type `;;` mid-typing to translate the next phrase and automatically return to normal input after commit/cancel.
- **Sticky (`Control+Shift+T`) & Virtual IM (`ShadowBar`)**: Stay in live bilingual translation mode across sentences or add `ShadowBar` directly as an input method in Fcitx5.
- **Multi-Window Isolation & Focus Protection**: Independent asynchronous translation queues per window; background completions for unfocused windows never steal active UI, and floating companion panels close cleanly upon focus loss or caret reset.
- **Instant Shortcuts**: `Alt+Shift+L` cycles target languages (default `["en", "zh", "ja"]`), `Alt+Shift+P` cycles style/provider presets, `Enter` commits the translation, and `Shift+Enter` commits the original text.

---

## 🌐 Multi-Engine Translation Pipeline (20 Languages)

ShadowBar routes across cloud APIs, local quantized LLMs, and zero-latency offline dictionaries from one shared `core/` engine:

| Backend Tier | Supported Engines / Formats | Highlights |
| --- | --- | --- |
| **Cloud LLMs & APIs** | OpenAI-compatible (DeepSeek, GPT, custom endpoints), DeepL, Ollama | Custom system prompts (`Direct`, `Natural`, `Work`, etc.), async debouncing & cancel-on-type |
| **Zero-Config Online** | Google Free | Instant setup with no API key required across all 20 languages |
| **Local Offline AI** | **Hunyuan `Hy-MT2-1.8B`** (`2bit` Q2_0C ~573 MB / custom `.gguf`) | Built-in `HuggingFace` / `HF-Mirror` downloader, one-click prebuilt engine download & reproducible [`scripts/build_llama_server.sh`](scripts/build_llama_server.sh) for `llama-server` (`auto`: Vulkan GPU first, CPU fallback), **idle auto-unload** & on-demand wake |
| **Millisecond Offline Dicts** | ECDICT (`.db`/`.sqlite`/`.csv`), StarDict (`.ifo`/`.idx`/`.dict.dz`), CC-CEDICT (`.u8`), JSON/JSONL, TSV | Instant offline lookups & fallback; mount multiple language-pair dicts simultaneously via Settings or `[dicts.<id>]` |

> **Local `llama-server` Engine Details:** `Hy-MT2-1.8B-2Bit.gguf` uses `Q2_0C` + `hunyuan-dense` (`sjl623/llama.cpp` PR #22836, pinned commit `1e411d8`). To keep git history and the main `.deb` (`< 1 MB`) clean, `llama-server` is shipped separately via CI artifact [`shadowbar-llama-server-linux-amd64.tar.gz`](.github/workflows/ci-deb.yml) (one-click **`⬇ 下载引擎`** inside Settings -> `~/.local/share/shadowbar/bin/llama-server`) or built locally with [`scripts/build_llama_server.sh`](scripts/build_llama_server.sh) (Vulkan GPU + CPU fallback; host benchmark on Ryzen AI 9 H 365 / Radeon 880M: **~0.40s/sentence** Vulkan, **~0.52s/sentence** CPU).

**20 Canonical Languages Supported (+ `auto` detection):** `en`, `zh`, `zh-TW`, `ja`, `ko`, `fr`, `de`, `es`, `pt`, `ru`, `it`, `ar`, `th`, `vi`, `id`, `hi`, `nl`, `pl`, `tr`, `uk`.

---

## ⌨️ Essential Shortcuts

| Scope | Shortcut | Action |
| --- | --- | --- |
| **Global Popup** | `Alt+Shift+Q` | Toggle `shadowbar-float` translation popup |
| **Popup Commit / Close** | `Ctrl+Enter` / `Esc` | Copy translation & hide / dismiss popup |
| **Popup Actions** | `Alt+V` / `Alt+R` / `Alt+S` / `Alt+M` | Read clipboard / Back-translate / Swap languages / Result-Only mode |
| **Popup Retry / Copy** | `Ctrl+R` (`F5`) / `Ctrl+Shift+C` | Retranslate bypassing cache / Copy translation |
| **Fcitx5 Inline** | `;;` / `Control+Shift+T` | Trigger One-Shot / Toggle Sticky companion bar |
| **Fcitx5 Cycling** | `Alt+Shift+L` / `Alt+Shift+P` | Cycle target language / Cycle translation preset |

---

## 🚀 Quick Start

> Prebuilt `.deb` packages (Ubuntu 24.04 / Debian 12 & 13) and the companion `shadowbar-llama-server-linux-amd64.tar.gz` runtime are available on **[GitHub Releases](https://github.com/ang-XWBWZ/fcitx5-shadowbar/releases/latest)**. To build & install from source:

```bash
git clone https://github.com/ang-XWBWZ/fcitx5-shadowbar.git
cd fcitx5-shadowbar

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure -V

# Install into ~/.local (no root required)
./scripts/install_user.sh
# Or install ShadowBar + the patched Q2_0C/Vulkan llama-server engine in one step:
# ./scripts/install_user.sh --with-llama-server

# Launch settings & popup (API presets, Hunyuan Hy-MT2-1.8B local model, dicts & one-click Chinese env fixer)
shadowbar-float --settings
```

> **Fresh Host Setup Tip:** Tab 4 (**`中文环境修复` / Chinese Environment Fixer**) inside `shadowbar-float --settings` automatically checks and repairs Fcitx5 packages, CJK monospace font fallback, `~/.config/fcitx5/profile`, and Wayland/X11 IME environment variables on Arch, Debian/Ubuntu, Fedora/RHEL, and openSUSE.

See **[docs/INSTALL.md](docs/INSTALL.md)** ([中文安装指南](docs/INSTALL.zh-CN.md)) for full distro packages, `.deb` builds, local Hunyuan `Hy-MT2-1.8B` / `llama-server` setup, and external dictionary mounting. You can also test translation routing immediately in terminal via `./build/cli-sim/shadowbar-sim`.

---

## 📁 Repository Structure

| Path | Purpose | License |
| --- | --- | --- |
| `core/` | C++20 translation pipeline, session state machine, 20-language registry, TOML hot-reload config, sentence splitter & offline dictionary engine | `PolyForm-Noncommercial-1.0.0` |
| `floating-ui/` | `shadowbar-float` Qt6 popup window, desktop orb, Wayland/X11 clipboard monitor, Hunyuan Hy-MT2 model manager & Chinese env fixer | `PolyForm-Noncommercial-1.0.0` |
| `fcitx5-addon/` | `libshadowbar.so` companion addon & `libshadowbarim.so` virtual IM | `PolyForm-Noncommercial-1.0.0` |
| `cli-sim/` | `shadowbar-sim` standalone CLI simulator | `PolyForm-Noncommercial-1.0.0` |
| `tests/` | CTest suites (`test_core`, `test_fcitx5_integration`, `test_floating_ui_helpers`) | `PolyForm-Noncommercial-1.0.0` |
| `scripts/` & `debian/` | Install/restart/engine-build scripts (`build_llama_server.sh`) and Debian `.deb` packaging files | `PolyForm-Noncommercial-1.0.0` |
| `docs/` & `config/config.toml.example` | Installation guides and commented configuration example (`~/.config/shadowbar/config.toml`) | `CC-BY-NC-SA-4.0` |
| `LICENSES/` | Verbatim official texts of `PolyForm-Noncommercial-1.0.0` and `CC-BY-NC-SA-4.0` | — |

---

## 📄 License & Community

Copyright (c) 2026 **ang** ([ang-XWBWZ](https://github.com/ang-XWBWZ))

- **Code, scripts & packaging**: [PolyForm Noncommercial License 1.0.0](LICENSES/PolyForm-Noncommercial-1.0.0.txt)
- **Documentation & example configs**: [CC BY-NC-SA 4.0](LICENSES/CC-BY-NC-SA-4.0.txt)
- Commercial use is prohibited under both licenses. See [LICENSE](LICENSE) for full notices and third-party library disclosures.
- Links: [Install Guide](docs/INSTALL.md) · [Config Example](config/config.toml.example) · [Changelog](CHANGELOG.md) · [Contributing](CONTRIBUTING.md) · [Security Policy](SECURITY.md)
