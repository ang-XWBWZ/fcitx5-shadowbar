# fcitx5-shadowbar

[![CI Build & Deb](https://github.com/ang-XWBWZ/fcitx5-shadowbar/actions/workflows/ci-deb.yml/badge.svg)](https://github.com/ang-XWBWZ/fcitx5-shadowbar/actions/workflows/ci-deb.yml)
[![最新发布版本](https://img.shields.io/github/v/release/ang-XWBWZ/fcitx5-shadowbar?label=Release)](https://github.com/ang-XWBWZ/fcitx5-shadowbar/releases/latest)
[![代码协议: PolyForm Noncommercial 1.0.0](https://img.shields.io/badge/Code%20License-PolyForm--Noncommercial--1.0.0-orange.svg)](LICENSES/PolyForm-Noncommercial-1.0.0.txt)
[![文档协议: CC BY-NC-SA 4.0](https://img.shields.io/badge/Docs%20License-CC%20BY--NC--SA%204.0-lightgrey.svg)](LICENSES/CC-BY-NC-SA-4.0.txt)

**语言：** [English](README.md) | 简体中文

**面向 Linux（Wayland / X11）的双形态实时翻译工具：Fcitx5 行内伴随翻译 + Qt6 独立翻译弹窗（`shadowbar-float`）。**  
打字时无缝同步出译文，复制/阅读时一秒弹窗互译；支持 **20 种语言**、**云端 AI 与标准翻译 API**、**本地混元 `Hy-MT2-1.8B`（`2bit` Q2_0C）推理** 与 **多格式毫秒级离线词库**。

> **开源非商用授权：** 源码与脚本使用 [PolyForm Noncommercial License 1.0.0](LICENSES/PolyForm-Noncommercial-1.0.0.txt)，文档与配置示例使用 [CC BY-NC-SA 4.0](LICENSES/CC-BY-NC-SA-4.0.txt)。详见 [LICENSE](LICENSE)。

---

## ✨ 两大核心翻译形态

### 1. 独立翻译弹窗 & 桌面悬浮球（`shadowbar-float`）

专为 Linux Wayland（KDE Plasma / GNOME / wlroots）与 X11 打造的轻量级置顶翻译弹窗，兼顾高频划词阅读、写文档对照与隐私保护：

```text
╭───────────────────────────────────────────────────────────────────╮
│ 🌐 [自动识别 ▾] ⇄ [英语 ▾]   [自然 ▾]        👁监听  🎯极简  📌置顶 ⚙│
├───────────────────────────────────────────────────────────────────┤
│  Clipboard monitoring works seamlessly across Wayland and X11.    │
│                                                  63 字符 · 自动英→中│
├───────────────────────────────────────────────────────────────────┤
│  ✦ 剪贴板监听可在 Wayland 和 X11 上无缝工作。                      │
├───────────────────────────────────────────────────────────────────┤
│  按 Esc 收起 · Ctrl+Enter 复制并收起     [⇅ 反向重译] [📋 复制] [↵] │
╰───────────────────────────────────────────────────────────────────╯
```

- **秒呼秒收 & 桌面悬浮球**：按 `Alt+Shift+Q` 随时呼出/隐藏翻译弹窗，也支持开启边缘悬浮球（左键展开、拖动定位、右键菜单）。
- **弹窗期间无焦点剪贴板监听（后台零常驻监听）**：融合 `wl-paste --watch`、KDE Klipper D-Bus 与 `QClipboard`，**仅在翻译弹窗显示时激活监听**，无需点击聚焦弹窗即可自动翻译新复制文本；弹窗收起或关闭即刻断联并停止相关探测进程。
- **智能避让敏感与非文本内容**：自动跳过密码管理器敏感提示、本地文件路径（`/home/...`、`file://`）、网址 URL、Markdown 链接与二进制负载，避免误译与隐私泄露。
- **弹窗专属效率操作**：
  - **20 语种自动识别 & 一键互换**：`Alt+S` 秒换源/目标语种，`Alt+V` 主动读取剪贴板。
  - **⇅ 反向重译校验语义（`Alt+R`）**：一键将下方译文回填为原文反向翻译，发送重要邮件/代码评审前快速核对原意。
  - **长文本智能分批翻译**：英文按词数、中文按字数自动分块，滚动或点击即可续译下一段，拒绝截断或超长卡死。
  - **极简模式与快捷键**：`Alt+M` 切换「仅看译文」极简卡片，`Ctrl+R` / `F5` 跳过缓存重译，`Ctrl+滚轮` 缩放字体（80%–160%），`Ctrl+Shift+C` 复制译文，`Ctrl+Enter` 复制译文并自动收起。

### 2. Fcitx5 行内伴随实时翻译（伴随浮条 + 虚拟输入法）

直接在拼音/五笔等中文输入法旁边无感挂载翻译浮条，**不打断原有中文输入节奏**：

```text
  [应用输入框] 正常打字中，键入 ;; 唤醒行内翻译 ──▶
  ┌──────────────────────────────────────────────────────────┐
  │ [中→EN · 自然] 今天把接口文档同步一下│                    │
  │ ✦ Let's sync the API documentation today.   · 云端已返回  │
  ├──────────────────────────────────────────────────────────┤
  │  ↵ Enter 上屏译文   ⇧ Shift+Enter 上屏原文   ⇥ 切预设     │
  └──────────────────────────────────────────────────────────┘
```

- **OneShot 一次性快译（`;;`）**：输入中文前/中敲入 `;;` 即刻触发翻译，上屏后自动退出回正常中文输入。
- **Sticky 常驻伴随模式（`Control+Shift+T`）& 独立虚拟输入法（`ShadowBar`）**：适合连续外语聊天、写英文 commit/文档；亦可直接作为 Fcitx5 输入法添加使用。
- **多窗口/跨会话严格隔离与失焦防护**：各应用输入框具备独立会话队列与去抖防抢占机制，后台慢速译文返回不会跨窗口覆盖或抢占焦点，输入框失焦或重置（FocusOut/Reset）时即刻安全关闭候选浮条并退出临时状态。
- **全局热键切换**：`Alt+Shift+L` 循环切换目标语言（默认 `["en", "zh", "ja"]`），`Alt+Shift+P` 实时轮换译文风格预设，`Shift+Enter` 保留原中文直出上屏。

---

## 🌐 全能多引擎翻译矩阵 & 20 种语言支持

ShadowBar 内建统一翻译调度引擎（`core/`），可自由组合在线 API、本地大模型与离线词库：

| 翻译后端类型 | 典型引擎 / 格式 | 适用场景与特点 |
| --- | --- | --- |
| **云端大模型 / API** | OpenAI 兼容接口（DeepSeek / GPT / 自定义）、DeepL、Ollama | 支持自定义 `Prompt` 预设（直译 / 自然 / 职场 / 学术），自动异步防抖 |
| **免配置在线引擎** | Google Free | 开箱即用、无需 API Key，支持 20 语种自动识别互译 |
| **本地轻量 AI 模型** | **腾讯混元 `Hy-MT2-1.8B`**（`2bit` Q2_0C ~573MB / 自定义 `.gguf`） | 设置页内置 `HuggingFace` / `HF-Mirror` 下载、支持一键下载预编译 `Q2_0C+Vulkan` 引擎或执行 [`scripts/build_llama_server.sh`](scripts/build_llama_server.sh) 自编译（默认 `auto` 优先 Vulkan、回退 CPU）与 **空闲自动卸载 / 按需秒拉起** |
| **毫秒级本地离线词库** | ECDICT（`.db`/`.sqlite`/`.csv`）、StarDict（`.ifo`/`.idx`/`.dict.dz`）、CC-CEDICT（`.u8`）、JSON/JSONL、TSV | 断网兜底 + 单词短语瞬时响应；支持多语种外部词典同时挂载与优先级配置 |

> **专版 `llama-server` 引擎说明**：`Hy-MT2-1.8B-2Bit.gguf` 采用 `Q2_0C` 量化与 `hunyuan-dense` 架构（上游分支 `sjl623/llama.cpp` PR #22836，固定 commit `1e411d8`）。为保持仓库与主 `.deb` 包轻量（`< 1 MB`），推理引擎通过 CI 产物 [`shadowbar-llama-server-linux-amd64.tar.gz`](.github/workflows/ci-deb.yml) 分发（设置页点击 **`⬇ 下载引擎`** 自动安装至 `~/.local/share/shadowbar/bin/llama-server`），或运行 [`scripts/build_llama_server.sh`](scripts/build_llama_server.sh) 本地编译（支持 Vulkan GPU + CPU 回退；在 Ryzen AI 9 H 365 / Radeon 880M 实测 Vulkan 平均 **~0.40 秒/句**，CPU **~0.52 秒/句**）。

**统一支持 20 种常用语言（加 `auto` 自动检测）**：`en`、`zh`、`zh-TW`、`ja`、`ko`、`fr`、`de`、`es`、`pt`、`ru`、`it`、`ar`、`th`、`vi`、`id`、`hi`、`nl`、`pl`、`tr`、`uk`。

---

## ⌨️ 翻译弹窗与行内翻译快捷键速查

| 使用场景 | 快捷键 | 触发行为 |
| --- | --- | --- |
| **全局翻译弹窗** | `Alt+Shift+Q` | 呼出 / 隐藏 `shadowbar-float` 独立翻译弹窗 |
| **弹窗内操作** | `Ctrl+Enter` / `Esc` | 复制译文并收起弹窗 / 直接收起弹窗 |
| **弹窗辅助** | `Alt+V` / `Alt+R` / `Alt+S` / `Alt+M` | 读剪贴板 / 反向重译 / 互换语种 / 仅看译文极简模式 |
| **弹窗重译/复制** | `Ctrl+R`（或 `F5`）/ `Ctrl+Shift+C` | 强制跳过缓存重译 / 仅复制译文不关闭弹窗 |
| **Fcitx5 行内模式** | `;;` / `Control+Shift+T` | 触发一次性伴随翻译 / 开关常驻翻译模式 |
| **行内快速切换** | `Alt+Shift+L` / `Alt+Shift+P` | 轮换目标语言 / 轮换译文风格预设 |

---

## 🚀 快速开始

> 可直接在 **[GitHub Releases](https://github.com/ang-XWBWZ/fcitx5-shadowbar/releases/latest)** 下载 Ubuntu 24.04 / Debian 12 & 13 的 `.deb` 安装包以及配套的 `shadowbar-llama-server-linux-amd64.tar.gz` 引擎；若从源码构建安装：

```bash
git clone https://github.com/ang-XWBWZ/fcitx5-shadowbar.git
cd fcitx5-shadowbar

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure -V

# 用户级安装到 ~/.local（无需 root）
./scripts/install_user.sh
# 或一步连同专版 Q2_0C + Vulkan llama-server 引擎一起安装：
# ./scripts/install_user.sh --with-llama-server

# 打开设置与翻译弹窗（内含 API 预设、腾讯混元 Hy-MT2-1.8B 本地模型、词典导入与新主机中文环境一键修复）
shadowbar-float --settings
```

> **新 Linux 主机冷启动提示：** 打开 `shadowbar-float --settings` 第 4 标签页 **「中文环境修复」** 可一键补齐 Arch / Debian / Ubuntu / Fedora / openSUSE 的 Fcitx5 组件、简体优先等宽字体回退、输入法配置以及 Wayland / X11 环境变量。

完整发行版依赖、`.deb` 打包、本地 `Hy-MT2-1.8B` / `llama-server` 与多语种词库配置步骤请参阅 **[docs/INSTALL.zh-CN.md](docs/INSTALL.zh-CN.md)**（[English Guide](docs/INSTALL.md)）。无需重启 Fcitx5 也可先运行 `./build/cli-sim/shadowbar-sim` 在终端体验核心翻译流水线。

---

## 📁 仓库结构

| 路径 | 职责说明 | 授权协议 |
| --- | --- | --- |
| `core/` | C++20 核心翻译引擎、会话状态机、20 语种路由、TOML 热重载配置、智能分句与多格式离线词库 | `PolyForm-Noncommercial-1.0.0` |
| `floating-ui/` | `shadowbar-float` Qt6 翻译弹窗、桌面悬浮球、Wayland/X11 剪贴板监听、腾讯混元 Hy-MT2 模型管家与中文环境修复 | `PolyForm-Noncommercial-1.0.0` |
| `fcitx5-addon/` | `libshadowbar.so` 伴随翻译插件、`libshadowbarim.so` 虚拟输入法及配置描述 | `PolyForm-Noncommercial-1.0.0` |
| `cli-sim/` | `shadowbar-sim` 独立命令行沙盒模拟器 | `PolyForm-Noncommercial-1.0.0` |
| `tests/` | CTest 单元与集成测试（`test_core`、`test_fcitx5_integration`、`test_floating_ui_helpers`） | `PolyForm-Noncommercial-1.0.0` |
| `scripts/`、`debian/` | 安装/重启/引擎编译脚本（`build_llama_server.sh`）与 Debian `.deb` 打包规则 | `PolyForm-Noncommercial-1.0.0` |
| `docs/`、`config/config.toml.example` | 中英文安装指南与配置模板（`~/.config/shadowbar/config.toml`） | `CC-BY-NC-SA-4.0` |
| `LICENSES/` | 完整双非商用协议官方原文 | — |

---

## 📄 授权协议与社区

Copyright (c) 2026 **ang** ([ang-XWBWZ](https://github.com/ang-XWBWZ))

- **代码、脚本与构建打包规则**：[PolyForm Noncommercial License 1.0.0](LICENSES/PolyForm-Noncommercial-1.0.0.txt)
- **文档与配置示例**：[CC BY-NC-SA 4.0](LICENSES/CC-BY-NC-SA-4.0.txt)
- 禁止未经授权的商业用途；详细声明与第三方动态链接库许可说明见 [LICENSE](LICENSE)。
- 相关文档：[安装指南](docs/INSTALL.zh-CN.md) · [配置示例](config/config.toml.example) · [更新日志](CHANGELOG.md) · [贡献指南](CONTRIBUTING.md) · [安全策略](SECURITY.md)
