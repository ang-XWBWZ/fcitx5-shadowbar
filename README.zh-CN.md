# fcitx5-shadowbar

[![CI Build & Deb](https://github.com/ang-XWBWZ/fcitx5-shadowbar/actions/workflows/ci-deb.yml/badge.svg)](https://github.com/ang-XWBWZ/fcitx5-shadowbar/actions/workflows/ci-deb.yml)
[![代码协议: PolyForm Noncommercial 1.0.0](https://img.shields.io/badge/Code%20License-PolyForm--Noncommercial--1.0.0-orange.svg)](LICENSES/PolyForm-Noncommercial-1.0.0.txt)
[![文档协议: CC BY-NC-SA 4.0](https://img.shields.io/badge/Docs%20License-CC%20BY--NC--SA%204.0-lightgrey.svg)](LICENSES/CC-BY-NC-SA-4.0.txt)

**语言：** [English](README.md) | 简体中文

面向 **Fcitx5** 的双轨实时伴随翻译输入法插件、独立输入法、`Qt6` Wayland/X11 悬浮翻译窗、多格式离线词库引擎、本地 **混元 HY-MT1.5-1.8B**（`1.25bit` / `2bit`）模型管家以及新主机一键中文环境与输入法修复工具。

> **开源非商用授权：** 本项目源码与脚本采用 [PolyForm Noncommercial License 1.0.0](LICENSES/PolyForm-Noncommercial-1.0.0.txt) 授权；文档与配置示例采用 [CC BY-NC-SA 4.0](LICENSES/CC-BY-NC-SA-4.0.txt) 授权。详见 [LICENSE](LICENSE)。

---

## 核心特性

- **Fcitx5 双轨行内实时伴随翻译（`fcitx5-addon/`）**：
  - **OneShot 一次性快译（`;;`）**：中文输入状态下键入 `;;` 即进入伴随翻译；不阻塞正常拼音/五笔打字，按 `Enter` 上屏译文，按 `Shift+Enter` 上屏中文原文。
  - **Sticky 常驻模式（`Control+Shift+T`）与独立虚拟输入法（`shadowbar`）**：支持跨句连续翻译或直接作为 Fcitx5 输入法添加使用。
  - **全局快捷键**：`Alt+Shift+L` 循环切换目标语言，`Alt+Shift+P` 快速切换译文风格预设。
- **Qt6 悬浮翻译窗与桌面悬浮球（`shadowbar-float`）**：
  - 支持全局快捷键 `Alt+Shift+Q` 呼出、双向语种自动识别、置顶固定、仅结果简洁视图、字体缩放以及按英文词数与中文字符数智能切分长文本分批翻译（`segment_max_chars`）。
- **Wayland + X11 后台剪贴板监听与智能避让**：
  - 通过 `wl-paste --watch`、KDE Klipper D-Bus 与 `QClipboard` 协同工作，在 Wayland 下无需聚焦或置顶悬浮窗即可后台感知复制变化。
  - 自动识别并避让密码管理器敏感 MIME 提示、本地文件路径 / `file://` URI、网页链接 URL、Markdown 链接及二进制非文本内容。
- **20 种语言统一路由与多引擎支持（`core/`）**：
  - 原生支持 `en`、`zh`、`zh-TW`、`ja`、`ko`、`fr`、`de`、`es`、`pt`、`ru`、`it`、`ar`、`th`、`vi`、`id`、`hi`、`nl`、`pl`、`tr`、`uk`（外加 `auto` 自动检测）。
  - 支持 OpenAI 兼容接口、Ollama、DeepL、Google Free、本地混元 HY-MT 服务与离线词库引擎。
- **本地腾讯混元 `HY-MT1.5-1.8B` 模型管理（`1.25bit` / `2bit` GGUF）**：
  - 设置页内建 `HuggingFace` / `HF-Mirror` 下载、自定义 GGUF 路径与本地 `llama-server` 启停管理，默认 **`auto`（Vulkan GPU 优先，失败自动回退 CPU）**，也可手动锁定 `vulkan` 或 `cpu`。
- **多格式多语言外部离线词库引擎**：
  - 内置基础词典索引，同时支持 **ECDICT SQLite/CSV**（`.db`、`.sqlite`、`.sqlite3`、`.csv`、`.csv.gz`）、**StarDict**（`.ifo`、`.idx`、`.dict.dz`）、**CC-CEDICT**（`.u8`）、**JSON / JSONL** 与分隔文本（`.tsv`、`.txt`），可在设置界面或 `[dicts.<id>]` 中多词典协同启用。
- **一键中文环境与输入法修复（设置页第 4 标签页）**：
  - 自动识别 **Arch Linux**、**Debian / Ubuntu**、**Fedora / RHEL** 与 **openSUSE**，一键安装 Fcitx5/CJK 字体/`wl-clipboard` 依赖、生成简体中文优先的等宽字体 fontconfig 回退、初始化 `~/.config/fcitx5/profile` 并同步配置 Wayland（`environment.d` + D-Bus）与 X11（`~/.xprofile`）输入法环境变量。

---

## 快速开始

```bash
git clone https://github.com/ang-XWBWZ/fcitx5-shadowbar.git
cd fcitx5-shadowbar

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure -V

# 用户级安装并打开设置与一键中文环境修复界面
./scripts/install_user.sh
shadowbar-float --settings
```

随后可执行 `./scripts/restart_fcitx5.sh` 重启输入法，或先运行离线终端模拟器体验核心流水线（无需重启 Fcitx5）：

```bash
./build/cli-sim/shadowbar-sim
```

各发行版依赖、`.deb` 打包、Wayland/X11 环境初始化、本地 TY 模型与外部词库导入详解请参阅 **[docs/INSTALL.zh-CN.md](docs/INSTALL.zh-CN.md)**（[English Guide](docs/INSTALL.md)）。

---

## 仓库目录结构

| 路径 | 职责说明 | 授权协议 |
| --- | --- | --- |
| `core/` | 共享 C++20 状态机、20 语言注册表、TOML 配置加载、分句器、异步翻译流水线与多格式离线词典引擎 | `PolyForm-Noncommercial-1.0.0` |
| `fcitx5-addon/` | `libshadowbar.so` 伴随翻译插件、`libshadowbarim.so` 虚拟输入法及 `.conf` 描述文件 | `PolyForm-Noncommercial-1.0.0` |
| `floating-ui/` | `shadowbar-float` Qt6 悬浮窗、桌面悬浮球、Wayland/X11 剪贴板监听、TY 模型管家与中文环境修复 | `PolyForm-Noncommercial-1.0.0` |
| `cli-sim/` | `shadowbar-sim` 独立命令行沙盒模拟器 | `PolyForm-Noncommercial-1.0.0` |
| `tests/` | CTest 单元与集成测试（`test_core`、`test_fcitx5_integration`、`test_floating_ui_helpers`） | `PolyForm-Noncommercial-1.0.0` |
| `scripts/`、`debian/` | 用户/系统安装脚本、Fcitx5 重启助手与 Debian `.deb` 打包配置 | `PolyForm-Noncommercial-1.0.0` |
| `docs/`、`config/config.toml.example` | 正式安装指南与带注释的配置示例（`~/.config/shadowbar/config.toml`） | `CC-BY-NC-SA-4.0` |
| `LICENSES/` | `PolyForm-Noncommercial-1.0.0` 与 `CC-BY-NC-SA-4.0` 完整原始授权文本 | — |

---

## 文档与项目规范

- 安装指南：[docs/INSTALL.zh-CN.md](docs/INSTALL.zh-CN.md) | [docs/INSTALL.md](docs/INSTALL.md)
- 配置模板：[config/config.toml.example](config/config.toml.example)
- 更新日志：[CHANGELOG.md](CHANGELOG.md)
- 贡献指南：[CONTRIBUTING.md](CONTRIBUTING.md)
- 安全与隐私策略：[SECURITY.md](SECURITY.md)

---

## 授权协议与版权声明

Copyright (c) 2026 **ang** ([ang-XWBWZ](https://github.com/ang-XWBWZ))

- **源码、构建脚本、测试与打包配置（`core/`、`fcitx5-addon/`、`floating-ui/`、`cli-sim/`、`tests/`、`scripts/`、`debian/`、`CMakeLists.txt`、`.github/`）**：
  适用 **PolyForm Noncommercial License 1.0.0**（完整条款见 [LICENSES/PolyForm-Noncommercial-1.0.0.txt](LICENSES/PolyForm-Noncommercial-1.0.0.txt)）。
- **文档与配置示例（`README*.md`、`docs/`、`CHANGELOG.md`、`CONTRIBUTING.md`、`SECURITY.md`、`config/config.toml.example`）**：
  适用 **Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International**（完整条款见 [LICENSES/CC-BY-NC-SA-4.0.txt](LICENSES/CC-BY-NC-SA-4.0.txt)）。

以上双协议均**禁止任何形式的商业使用**。必需保留声明（Required Notice）及第三方动态链接库声明请参阅 [LICENSE](LICENSE)。
