# Changelog

All notable changes to **fcitx5-shadowbar** are documented in this file.

## [0.1.3] - 2026-11-12

### Highlights: Hunyuan `Hy-MT2-1.8B (2bit)` & Patched `llama-server` Packaging

- **Built-In Local Model Upgrade (`Hy-MT2-1.8B 2bit`)**
  - Replaced legacy `HY-MT1.5` (`1.25bit` / `2bit`) presets with **`Hy-MT2-1.8B (2bit Q2_0C)`** (`tencent/Hy-MT2-1.8B-2Bit-GGUF`, ~573 MB; benchmarked at ~0.40s/sentence on Radeon 880M Vulkan and ~0.52s/sentence on CPU), while preserving **custom local `.gguf`** paths, **all online LLM APIs** (OpenAI / DeepSeek / Ollama), **standard translation APIs** (DeepL / Google Free), and **offline dictionaries**.
  - Existing user configurations with `variant = "1.25bit"` or legacy `hy-mt1.5` local preset names automatically migrate to `Hy-MT2-1.8B (2bit)` without manual re-setup.

- **Non-Standard `llama-server` (`Q2_0C` + `hunyuan-dense`) Tooling & Release Distribution**
  - Added reproducible [`scripts/build_llama_server.sh`](scripts/build_llama_server.sh) pinned to `sjl623/llama.cpp` commit `1e411d8f5a1e23525fa3265dfb4bd76265465397` (PR #22836) with automatic Vulkan GPU detection (`GGML_VULKAN=AUTO|ON|OFF`) and optional `--prebuilt` release archive fetch.
  - Added **one-click `⬇ 下载引擎`** in `shadowbar-float --settings` (`TyModelManager::startEngineDownload`) plus `SHADOWBAR_ENGINE_URL` override and explicit error diagnostics when an older stock `llama-server` lacks `Q2_0C` / `hunyuan-dense`.
  - Updated `scripts/install_user.sh` and `scripts/install_system.sh` with `--with-llama-server` / `--build-llama-server` flags and post-install engine detection guidance.
  - Added CI `build-llama-server` (`debian:bookworm`, Vulkan + CPU) and automated `release` jobs in [`.github/workflows/ci-deb.yml`](.github/workflows/ci-deb.yml) to publish `shadowbar-llama-server-linux-amd64.tar.gz` alongside `.deb` packages on `v*` tags.

## [0.1.2] - 2026-11-12

### Bug Fixes

- **Floating UI Startup Crash (`SIGSEGV`)**: Added a re-entrancy guard (`applying_`) and redundant-write checks to `OpaquePopupEventFilter` in `floating-ui/ui_util.hpp`, fixing an infinite `QEvent::StyleChange` -> `setStyleSheet()` recursion loop when opening `shadowbar-float` on Qt6/Wayland.

## [0.1.1] - 2026-11-12

### Popup Performance, Modernized Card Layout & System Theme Polish

- **Performance & Local Model Lifecycle**
  - Clipboard subscriptions (`wl-paste --watch`, KDE Klipper D-Bus, `QClipboard`) now activate **only while the translation popup card is open**, eliminating background idle overhead while preserving instant clipboard updates.
  - Added configurable idle auto-unload (`idle_unload_sec`, default 5 minutes) for local **混元 HY-MT1.5** (`llama-server`), plus `--wake-ty` on-demand wake and single-slot KV sizing (`-np 1`) to free RAM/VRAM when unused.
  - Optimized async HTTP/SSE streaming and lazy idle-exiting worker lifecycle in `AsyncTranslationEngine`.

- **UI Layout, Naming & Theme Fixes**
  - Separated top draggable title chrome (`✦ ShadowBar`, direction badge, clipboard/view/window actions) from a dedicated language direction & preset control strip (`control_bar_`) for clearer visual hierarchy.
  - Unified user-facing naming to **腾讯混元 HY-MT1.5** (`混元 HY-MT1.5 (1.25bit)` / `(2bit)` / `(自定义)`) across popup presets, settings tabs, status messages, and documentation while keeping existing config IDs backward-compatible.
  - Enhanced `ThemeTokens` and event/palette listeners (`QEvent::ThemeChange`, `colorSchemeChanged` on Qt 6.5+) to follow desktop light/dark mode and system accent colors cleanly.
  - Enforced 100%-opaque popup styling (`popup_bg`, `QPalette`, `OpaquePopupEventFilter`) for all `QComboBox` language/preset dropdowns and context menus over translucent windows.

## [0.1.0] - 2026-11-12

### Initial Public Non-Commercial Open-Source Release

- **Core Translation & Session Engine (`core/`)**
  - Dual-track session state machine supporting **One-Shot (`;;`)**, **Sticky (`Ctrl+Shift+T`)**, and **Virtual IM** modes.
  - Unified 20-language registry (`en`, `zh`, `zh-TW`, `ja`, `ko`, `fr`, `de`, `es`, `pt`, `ru`, `it`, `ar`, `th`, `vi`, `id`, `hi`, `nl`, `pl`, `tr`, `uk`) with automatic source language detection and language cycling (`cycle_languages`).
  - Asynchronous translation pipeline supporting OpenAI-compatible APIs, Ollama, DeepL, Google Free, local HY-MT servers, and offline dictionaries.
  - Multi-format external offline dictionary engine supporting **ECDICT SQLite/CSV**, **StarDict** (`.ifo`/`.idx`/`.dict.dz`), **CC-CEDICT** (`.u8`), **JSON/JSONL**, and delimited TSV/text dictionaries.
  - Word-aware Latin + character-counted CJK sentence splitting (`sentence_splitter`) for batched long-text translation.

- **Fcitx5 Integration & CLI Simulator (`fcitx5-addon/`, `cli-sim/`)**
  - Companion `shadowbar` Fcitx5 addon (`libshadowbar.so`) and `shadowbarim` virtual input method (`libshadowbarim.so`).
  - Interactive sandbox CLI simulator (`shadowbar-sim`) for offline testing without restarting Fcitx5.

- **Qt6 Floating UI & Tooling (`floating-ui/`)**
  - Floating translation window & desktop orb (`shadowbar-float`) with Wayland/X11 background clipboard monitoring (`wl-paste --watch`, KDE Klipper D-Bus, `QClipboard`) and automatic avoidance of passwords, file paths/URIs, URLs, and binary payloads.
  - Tabbed Settings UI: API & Presets, IME/Hotkeys/Floating/Multilingual Dictionary Manager, Hunyuan **HY-MT1.5-1.8B** (`1.25bit` / `2bit` / custom GGUF) local runtime manager with Vulkan-preferred/CPU-fallback launch, and **One-Click Chinese Environment & IME Fixer** (Arch, Debian/Ubuntu, Fedora/RHEL, openSUSE).

- **Licensing & Packaging**
  - Dual non-commercial open-source licensing: `PolyForm-Noncommercial-1.0.0` (code & scripts) + `CC-BY-NC-SA-4.0` (docs & example configs).
  - CMake, CTest suite (`test_core`, `test_fcitx5_integration`, `test_floating_ui_helpers`), Debian packaging metadata, and GitHub Actions CI.
