# Changelog

All notable changes to **fcitx5-shadowbar** are documented in this file.

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
