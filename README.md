# fcitx5-shadowbar

[![CI Build & Deb](https://github.com/ang-XWBWZ/fcitx5-shadowbar/actions/workflows/ci-deb.yml/badge.svg)](https://github.com/ang-XWBWZ/fcitx5-shadowbar/actions/workflows/ci-deb.yml)
[![Code License: PolyForm Noncommercial 1.0.0](https://img.shields.io/badge/Code%20License-PolyForm--Noncommercial--1.0.0-orange.svg)](LICENSES/PolyForm-Noncommercial-1.0.0.txt)
[![Docs License: CC BY-NC-SA 4.0](https://img.shields.io/badge/Docs%20License-CC%20BY--NC--SA%204.0-lightgrey.svg)](LICENSES/CC-BY-NC-SA-4.0.txt)

**Language:** English | [简体中文](README.zh-CN.md)

A dual-track real-time translation companion bar, virtual input method, Qt6 Wayland/X11 floating translator, offline multilingual dictionary engine, local **HY-MT1.5-1.8B** (`1.25bit` / `2bit`) runtime manager, and one-click Chinese environment & IME fixer for **Fcitx5** on Linux.

> **Non-Commercial Open Source:** Source code is licensed under [PolyForm Noncommercial License 1.0.0](LICENSES/PolyForm-Noncommercial-1.0.0.txt); documentation and example configs are licensed under [CC BY-NC-SA 4.0](LICENSES/CC-BY-NC-SA-4.0.txt). See [LICENSE](LICENSE).

---

## Key Features

- **Dual-Track Inline Fcitx5 Translation (`fcitx5-addon/`)**:
  - **One-Shot Mode (`;;`)**: Type `;;` followed by Chinese/source text inside Fcitx5; preview live translations non-blockingly, press `Enter` to commit the translation or `Shift+Enter` to commit the original text.
  - **Sticky Mode (`Control+Shift+T`) & Virtual IM (`shadowbar`)**: Keep real-time translation active across sentences or add `ShadowBar` directly as an input method.
  - **Global Cycling Shortcuts**: Switch target languages (`Alt+Shift+L`) and translation presets (`Alt+Shift+P`) on the fly without blocking normal typing.
- **Qt6 Floating Window & Desktop Orb (`shadowbar-float`)**:
  - Toggle anywhere (`Alt+Shift+Q`), translate with auto-detected direction, pin above windows, switch to Result-Only mode, scale typography, and process long paragraphs cleanly via word/character-aware batching (`segment_max_chars`).
- **Wayland & X11 Background Clipboard Monitor**:
  - Tracks clipboard changes in the background on Wayland (`wl-paste --watch` + KDE Klipper D-Bus + `QClipboard`) without requiring window focus or pinning.
  - Automatically skips password/sensitive hints, local file URIs/paths, URLs, Markdown links, and binary non-text payloads.
- **20-Language Unified Pipeline & Flexible Backends (`core/`)**:
  - Supports `en`, `zh`, `zh-TW`, `ja`, `ko`, `fr`, `de`, `es`, `pt`, `ru`, `it`, `ar`, `th`, `vi`, `id`, `hi`, `nl`, `pl`, `tr`, and `uk` (plus `auto` detection).
  - Works with OpenAI-compatible APIs, Ollama, DeepL, Google Free, local Hunyuan `HY-MT1.5-1.8B` servers, and offline dictionaries.
- **Local Hunyuan `HY-MT1.5-1.8B` Manager (`1.25bit` & `2bit` GGUF)**:
  - Download (`HuggingFace` / `HF-Mirror`), configure, and launch local `llama-server` directly from Settings Tab 3 with **`auto` (Vulkan GPU preferred + CPU fallback)**, `vulkan`, or `cpu` execution modes.
- **Multi-Format Offline Dictionary Engine**:
  - Built-in dictionary index plus extensible `[dicts.<id>]` manager supporting **ECDICT SQLite/CSV** (`.db`, `.sqlite`, `.sqlite3`, `.csv`, `.csv.gz`), **StarDict** (`.ifo`, `.idx`, `.dict.dz`), **CC-CEDICT** (`.u8`), **JSON / JSONL**, and delimited `.tsv`/`.txt` files.
- **One-Click Chinese Environment & IME Fixer (Settings Tab 4)**:
  - Cross-distro diagnostic and automated repair for **Arch Linux**, **Debian / Ubuntu**, **Fedora / RHEL**, and **openSUSE**: installs missing Fcitx5/CJK font/`wl-clipboard` packages, configures CJK-friendly monospace font fallback, initializes `~/.config/fcitx5/profile`, and syncs Wayland (`environment.d` + D-Bus) and X11 (`~/.xprofile`) IME variables.

---

## Quick Start

```bash
git clone https://github.com/ang-XWBWZ/fcitx5-shadowbar.git
cd fcitx5-shadowbar

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure -V

# Install to ~/.local and configure IME session environment
./scripts/install_user.sh
shadowbar-float --settings
```

Then either run `./scripts/restart_fcitx5.sh` or try the offline sandbox simulator without touching your desktop IME:

```bash
./build/cli-sim/shadowbar-sim
```

Full distro prerequisites, `.deb` packaging, Wayland/X11 setup, local TY GGUF runtime, and offline dictionary setup instructions are in **[docs/INSTALL.md](docs/INSTALL.md)** ([中文安装指南](docs/INSTALL.zh-CN.md)).

---

## Repository Layout

| Path | Purpose | License |
| --- | --- | --- |
| `core/` | Shared C++20 session state machine, 20-language registry, TOML config loader, sentence splitter, async translator & multi-format offline dictionary engine | `PolyForm-Noncommercial-1.0.0` |
| `fcitx5-addon/` | `libshadowbar.so` companion addon & `libshadowbarim.so` virtual input method + `.conf` templates | `PolyForm-Noncommercial-1.0.0` |
| `floating-ui/` | `shadowbar-float` Qt6 floating window, desktop orb, Wayland/X11 clipboard monitor, TY model manager & Chinese env fixer | `PolyForm-Noncommercial-1.0.0` |
| `cli-sim/` | `shadowbar-sim` standalone interactive CLI sandbox simulator | `PolyForm-Noncommercial-1.0.0` |
| `tests/` | CTest suites (`test_core`, `test_fcitx5_integration`, `test_floating_ui_helpers`) | `PolyForm-Noncommercial-1.0.0` |
| `scripts/` & `debian/` | User/system install scripts, Fcitx5 restart helper, and Debian `.deb` packaging rules | `PolyForm-Noncommercial-1.0.0` |
| `docs/` & `config/config.toml.example` | Installation guides and commented configuration example (`~/.config/shadowbar/config.toml`) | `CC-BY-NC-SA-4.0` |
| `LICENSES/` | Verbatim official texts of `PolyForm-Noncommercial-1.0.0` and `CC-BY-NC-SA-4.0` | — |

---

## Documentation & Community

- Installation Guide: [docs/INSTALL.md](docs/INSTALL.md) | [docs/INSTALL.zh-CN.md](docs/INSTALL.zh-CN.md)
- Example Configuration: [config/config.toml.example](config/config.toml.example)
- Changelog: [CHANGELOG.md](CHANGELOG.md)
- Contributing: [CONTRIBUTING.md](CONTRIBUTING.md)
- Security Policy: [SECURITY.md](SECURITY.md)

---

## License & Copyright

Copyright (c) 2026 **ang** ([ang-XWBWZ](https://github.com/ang-XWBWZ))

- **Code, scripts, tests, and packaging (`core/`, `fcitx5-addon/`, `floating-ui/`, `cli-sim/`, `tests/`, `scripts/`, `debian/`, `CMakeLists.txt`, `.github/`)**:
  Licensed under the **PolyForm Noncommercial License 1.0.0** ([LICENSES/PolyForm-Noncommercial-1.0.0.txt](LICENSES/PolyForm-Noncommercial-1.0.0.txt)).
- **Documentation & configuration examples (`README*.md`, `docs/`, `CHANGELOG.md`, `CONTRIBUTING.md`, `SECURITY.md`, `config/config.toml.example`)**:
  Licensed under **Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International** ([LICENSES/CC-BY-NC-SA-4.0.txt](LICENSES/CC-BY-NC-SA-4.0.txt)).

Commercial use is prohibited under both licenses. See [LICENSE](LICENSE) for complete required notices and third-party dynamic-linking disclosures.
