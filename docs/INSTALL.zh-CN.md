# 安装与配置指南

**语言：** [English](INSTALL.md) | 简体中文

本文档介绍如何从源码或 Debian 软件包构建并安装 **fcitx5-shadowbar**（包含 `libshadowbar.so`、`libshadowbarim.so`、`shadowbar-float` 与 `shadowbar-sim`），以及在新主机上配置 Wayland/X11 中文输入环境、本地 HY-MT1.5 模型与多语言离线词库。

---

## 1. 环境依赖准备

### Arch Linux / Manjaro

```bash
# 编译依赖
sudo pacman -S --needed base-devel cmake pkgconf fcitx5 curl nlohmann-json qt6-base sqlite zlib

# 推荐中文输入法、CJK 字体与 Wayland 剪贴板工具
sudo pacman -S --needed fcitx5-chinese-addons fcitx5-configtool fcitx5-gtk fcitx5-qt \
  noto-fonts noto-fonts-cjk noto-fonts-emoji wl-clipboard
```

### Debian 12+ / Ubuntu 24.04+

```bash
# 编译依赖
sudo apt update
sudo apt install -y cmake g++ pkgconf debhelper dpkg-dev \
  libfcitx5core-dev libfcitx5config-dev libfcitx5utils-dev fcitx5-modules-dev \
  libcurl4-openssl-dev nlohmann-json3-dev qt6-base-dev libsqlite3-dev zlib1g-dev

# 推荐中文输入法、CJK 字体与 Wayland 剪贴板工具
sudo apt install -y fcitx5 fcitx5-chinese-addons fcitx5-config-qt \
  fcitx5-frontend-gtk3 fcitx5-frontend-gtk4 fcitx5-frontend-qt5 fcitx5-frontend-qt6 \
  fonts-noto-cjk fonts-noto-color-emoji im-config wl-clipboard locales
```

### Fedora / RHEL

```bash
# 编译依赖
sudo dnf install -y cmake gcc-c++ pkgconf-pkg-config fcitx5-devel \
  libcurl-devel json-devel qt6-qtbase-devel sqlite-devel zlib-devel

# 推荐中文输入法、CJK 字体与 Wayland 剪贴板工具
sudo dnf install -y fcitx5 fcitx5-chinese-addons fcitx5-configtool fcitx5-gtk fcitx5-qt \
  google-noto-sans-cjk-fonts google-noto-sans-mono-cjk-fonts google-noto-emoji-color-fonts wl-clipboard
```

---

## 2. 克隆、编译与测试

```bash
git clone https://github.com/ang-XWBWZ/fcitx5-shadowbar.git
cd fcitx5-shadowbar

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure -V
```

验证输出应显示 `test_core`、`test_fcitx5_integration`、`test_floating_ui_helpers` 全部通过（`3/3`）。

---

## 3. 安装方式

### 方式 A：用户目录安装（推荐，无需 `sudo`）

安装到 `~/.local/`：

```bash
./scripts/install_user.sh
```

> 若使用用户目录安装，请确保 Fcitx5 能加载 `~/.local/lib/fcitx5`（可通过第 4 节的「一键中文环境修复」自动写入环境变量 `FCITX_ADDON_DIRS`）。

### 方式 B：系统全局安装

安装到 `/usr`（默认 `PREFIX=/usr`）：

```bash
sudo ./scripts/install_system.sh
```

### 方式 C：制作 `.deb` 安装包（Debian / Ubuntu）

```bash
dpkg-buildpackage -us -uc -b
sudo apt install ../fcitx5-shadowbar_0.1.0-1_*.deb
```

---

## 4. 新主机中文环境与输入法一键初始化（Wayland + X11 通用）

如果是在全新安装的 Linux 桌面（KDE Plasma Wayland、GNOME Wayland 或 X11）上配置中文输入法，可以直接打开设置界面的修复页：

```bash
shadowbar-float --settings
```

切换到第 4 个标签页 **`中文环境修复`**：

1. 点击 **重新检测**：自动检查当前发行版（Arch / Debian / Ubuntu / Fedora / openSUSE）、Fcitx5 组件与字体是否缺失、等宽字体中文回退（`fc-match monospace:charset=4e2d`）、`~/.config/fcitx5/profile`、`systemd` / D-Bus / X11 `.xprofile` 环境变量以及可选 `zh_CN.UTF-8` locale。
2. 点击 **一键全部修复**：
   - 自动调用系统包管理器补全 Fcitx5、中文附加组件、GTK/Qt 前端、Noto CJK 字体和 `wl-clipboard`。
   - 写入用户级 fontconfig 等宽字体规则（保留英文等宽同时优先简体中文回退，避免日语/韩语异体字）。
   - 自动初始化 `~/.config/fcitx5/profile`（确保包含 `keyboard-us`、`pinyin` 与 `shadowbar`）。
   - 同时写入 Wayland（`~/.config/environment.d/` + `dbus-update-activation-environment`）与 X11（`~/.xprofile`）环境变量，确保新主机跨会话、多框架应用直接可用。
3. 修复后建议注销重登一次以使全部进程继承新环境变量，随后重启 Fcitx5：

```bash
./scripts/restart_fcitx5.sh
```

---

## 5. 基础配置与快捷键

首次运行会自动创建 `~/.config/shadowbar/config.toml`（也可参考 [config/config.toml.example](../config/config.toml.example)）。修改并保存后会热重载，无需重启输入法。

### 默认快捷键

| 功能 | 默认快捷键 | 说明 |
| --- | --- | --- |
| 一次性快译模式 | `;;` | 中文输入状态下输入 `;;` 触发；`Enter` 上屏译文，`Shift+Enter` 上屏原文 |
| 常驻伴随译文条 | `Control+Shift+T` | 开关常驻翻译模式 |
| 循环切换目标语言 | `Alt+Shift+L` | 在 `cycle_languages` 列表中轮换（默认 `["en", "zh", "ja"]`） |
| 切换译文预设 | `Alt+Shift+P` | 轮换当前翻译预设 |
| 唤出/收起悬浮窗 | `Alt+Shift+Q` | 控制 `shadowbar-float` 悬浮窗 |

### `shadowbar-float` 命令行参数

```text
shadowbar-float [--toggle|--show|--settings|--sync-orb|--reset|--quit]
```

### 离线沙盒命令行测试

无需重启 Fcitx5 即可测试核心翻译会话：

```bash
./build/cli-sim/shadowbar-sim
```

---

## 6. 后台剪贴板监听与避让机制

在 `shadowbar-float --settings` 或 `config.toml` 的 `[floating]` 中开启 `watch_clipboard = true`：

- **Wayland 后台监听**：结合 `wl-paste --watch`、KDE Klipper D-Bus 信号与 `QClipboard`，无需聚焦或置顶钉住悬浮窗即可感知复制内容变更。
- **自动避让保护**：自动跳过密码管理器敏感 MIME 提示、本地文件路径 / `file://` URI、HTTP(S)/mailto 网址链接、Markdown 链接及二进制非文本内容。

---

## 7. 本地轻量模型：腾讯混元 Hy-MT2-1.8B（`2bit` Q2_0C）

由于 `Hy-MT2-1.8B-2Bit.gguf` 使用非标准 `Q2_0C` 量化与 `hunyuan-dense` 架构（上游分支 `sjl623/llama.cpp` PR #22836，固定 commit `1e411d8f5a1e23525fa3265dfb4bd76265465397`），普通系统包仓库的 `llama.cpp` 可能报 `unknown tensor type` / `unknown model architecture` 而无法加载。推荐以下两种方式获取专版推理引擎：

1. **安装时一步部署、界面一键下载或源码编译专版 `llama-server`**：
   - 安装时直接追加 `--with-llama-server` 参数（`./scripts/install_user.sh --with-llama-server` 或 `./scripts/install_system.sh --with-llama-server`），会自动安装预编译引擎（不可达时自动回退源码编译）；或
   - 打开 `shadowbar-float --settings` -> **`混元 Hy-MT2 (2bit)`** 标签页，直接点击 `llama-server` 行右侧的 **`⬇ 下载引擎`**，自动安装预编译版 `shadowbar-llama-server-linux-amd64.tar.gz` 到 `~/.local/share/shadowbar/bin/llama-server`（可用环境变量 `SHADOWBAR_ENGINE_URL` 指定自定义下载源）；或
   - 使用仓库自带脚本从固定 commit 一键编译（如需 Vulkan GPU 加速，先安装 `libvulkan-dev glslc glslang-tools`；支持 `GGML_VULKAN=AUTO|ON|OFF` 与 `GGML_NATIVE=ON|OFF`）：
     ```bash
     ./scripts/build_llama_server.sh
     ```
2. 选中 **Hy-MT2-1.8B 2bit · Q2_0C（约 573 MB）**（或自定义 `.gguf`），按网络环境选择 `HuggingFace` 或 `HF-Mirror` 镜像下载模型并启动服务。
3. 推理后端默认为 **`auto`**（优先尝试 Vulkan GPU 加速，无可用 GPU 时自动回退 CPU），也可手动指定 `vulkan` 或 `cpu`。
4. 在线大模型与标准翻译接口（**Google Free / DeepL / OpenAI 兼容 / Ollama**）及本地离线词库保持完整独立，可随时切换搭配。

---

## 8. 多语言外部离线词库导入

ShadowBar 会自动兼容扫描 `~/.local/share/shadowbar/` 下的默认词库文件（`ecdict.db`、`ecdict.sqlite`、`ecdict.sqlite3`、`ecdict.csv`、`cedict_ts.u8`），并支持在设置页或 `config.toml` 的 `[dicts.<id>]` 中挂载多个多语言词典：

支持格式：
- **ECDICT SQLite / CSV**：`.db`、`.sqlite`、`.sqlite3`、`.csv`、`.csv.gz`
- **StarDict**：`.ifo`、`.idx`、`.idx.gz`、`.dict`、`.dict.dz`、`.dict.gz`
- **CC-CEDICT**：`.u8`、`.u8.gz`
- **JSON / JSONL**：`.json`、`.jsonl`、`.jsonl.gz`
- **分隔文本**：`.tsv`、`.txt`

---

## 9. 常见问题排查

- **Wayland 下不触发后台剪贴板监听**：检查终端是否可执行 `wl-paste --version` 并确认 `watch_clipboard = true`。
- **用户目录安装后 Fcitx5 配置里看不到 ShadowBar**：检查环境变量 `FCITX_ADDON_DIRS` 是否包含 `~/.local/lib/fcitx5`，并执行 `./scripts/restart_fcitx5.sh`。
- **本地混元 Hy-MT2 开启 Vulkan 失败**：将推理后端切回 `auto`（自动回退 CPU）或用 `vulkaninfo` 检查显卡驱动与 Vulkan ICD。
