#include "zh_env_fixer.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTextStream>
#include <QThread>

#include <algorithm>
#include <utility>

namespace shadowbar::floating_ui {

namespace {

QString shellQuoteSingle(const QString &s) {
    QString out = s;
    out.replace('\'', QStringLiteral("'\\''"));
    return QString("'%1'").arg(out);
}

QString runQuickProcess(const QString &program,
                        const QStringList &args,
                        int timeout_ms = 5000,
                        int *exit_code = nullptr) {
    const QString exe = QStandardPaths::findExecutable(program);
    if (exe.isEmpty()) {
        if (exit_code) {
            *exit_code = -1;
        }
        return {};
    }
    QProcess proc;
    proc.start(exe, args);
    if (!proc.waitForStarted(1500)) {
        if (exit_code) {
            *exit_code = -1;
        }
        return {};
    }
    if (!proc.waitForFinished(timeout_ms)) {
        proc.kill();
        proc.waitForFinished(500);
        if (exit_code) {
            *exit_code = -1;
        }
        return {};
    }
    if (exit_code) {
        *exit_code = proc.exitCode();
    }
    return QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
}

QString readTextFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(f.readAll());
}

bool writeTextFile(const QString &path, const QString &content, QString *err = nullptr) {
    const QFileInfo fi(path);
    if (!QDir().mkpath(fi.absolutePath())) {
        if (err) {
            *err = QString("无法创建目录: %1").arg(fi.absolutePath());
        }
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (err) {
            *err = QString("无法写入文件 %1: %2").arg(path, f.errorString());
        }
        return false;
    }
    const QByteArray utf8 = content.toUtf8();
    if (f.write(utf8) != utf8.size()) {
        if (err) {
            *err = QString("写入不完整: %1").arg(path);
        }
        return false;
    }
    return true;
}

QString defaultAddonDirsValue() {
    const QString env_val = qEnvironmentVariable("FCITX_ADDON_DIRS").trimmed();
    if (!env_val.isEmpty()) {
        return env_val;
    }
    const QString user_dir = QDir::homePath() + "/.local/lib/fcitx5";
    return user_dir + ":/usr/lib/fcitx5:/usr/lib/x86_64-linux-gnu/fcitx5";
}

QString stripMatchingQuotes(QString val) {
    val = val.trimmed();
    if (val.size() >= 2 &&
        ((val.startsWith('"') && val.endsWith('"')) ||
         (val.startsWith('\'') && val.endsWith('\'')))) {
        return val.mid(1, val.size() - 2);
    }
    return val;
}

bool profileHasRequiredIms(const QString &ini) {
    const QStringList lines = ini.split('\n');
    bool has_pinyin = false;
    bool has_keyboard = false;
    bool has_shadowbar = false;
    QString default_im;
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.startsWith(QStringLiteral("DefaultIM="))) {
            default_im = line.mid(10).trimmed();
        } else if (line.startsWith(QStringLiteral("Name="))) {
            const QString name = line.mid(5).trimmed();
            if (name == QStringLiteral("pinyin") || name == QStringLiteral("rime") ||
                name == QStringLiteral("shuangpin") || name == QStringLiteral("wbx")) {
                has_pinyin = true;
            } else if (name.startsWith(QStringLiteral("keyboard-"))) {
                has_keyboard = true;
            } else if (name == QStringLiteral("shadowbar")) {
                has_shadowbar = true;
            }
        }
    }
    const bool good_default =
        !default_im.isEmpty() && !default_im.startsWith(QStringLiteral("keyboard-"));
    return has_pinyin && has_keyboard && has_shadowbar && good_default;
}

} // namespace

bool ZhEnvDiagReport::allOk() const {
    for (const auto &it : items) {
        if (!it.ok) {
            return false;
        }
    }
    return !items.isEmpty();
}

DistroFamily detectDistroFamily(const QString &os_release_text) {
    QString id;
    QString id_like;
    for (const QString &raw : os_release_text.split('\n')) {
        const QString line = raw.trimmed();
        if (line.startsWith(QStringLiteral("ID="))) {
            id = stripMatchingQuotes(line.mid(3)).toLower();
        } else if (line.startsWith(QStringLiteral("ID_LIKE="))) {
            id_like = stripMatchingQuotes(line.mid(8)).toLower();
        }
    }
    const QString combined = id + " " + id_like;
    if (combined.contains("arch") || combined.contains("manjaro") ||
        combined.contains("endeavouros") || combined.contains("cachyos")) {
        return DistroFamily::Arch;
    }
    if (combined.contains("debian") || combined.contains("ubuntu") ||
        combined.contains("linuxmint") || combined.contains("pop") ||
        combined.contains("deepin")) {
        return DistroFamily::DebianUbuntu;
    }
    if (combined.contains("fedora") || combined.contains("rhel") ||
        combined.contains("centos") || combined.contains("rocky") ||
        combined.contains("almalinux")) {
        return DistroFamily::FedoraRhel;
    }
    if (combined.contains("opensuse") || combined.contains("suse") ||
        combined.contains("tumbleweed") || combined.contains("leap")) {
        return DistroFamily::OpenSuse;
    }
    return DistroFamily::Unknown;
}

QString distroFamilyDisplayName(DistroFamily family) {
    switch (family) {
    case DistroFamily::Arch:
        return QStringLiteral("Arch / Manjaro (pacman)");
    case DistroFamily::DebianUbuntu:
        return QStringLiteral("Debian / Ubuntu (apt)");
    case DistroFamily::FedoraRhel:
        return QStringLiteral("Fedora / RHEL (dnf)");
    case DistroFamily::OpenSuse:
        return QStringLiteral("openSUSE (zypper)");
    case DistroFamily::Unknown:
        break;
    }
    return QStringLiteral("未知发行版");
}

QStringList requiredPackagesForDistro(DistroFamily family) {
    switch (family) {
    case DistroFamily::Arch:
        return {QStringLiteral("fcitx5"),
                QStringLiteral("fcitx5-chinese-addons"),
                QStringLiteral("fcitx5-configtool"),
                QStringLiteral("fcitx5-gtk"),
                QStringLiteral("fcitx5-qt"),
                QStringLiteral("noto-fonts"),
                QStringLiteral("noto-fonts-cjk"),
                QStringLiteral("noto-fonts-emoji"),
                QStringLiteral("wl-clipboard")};
    case DistroFamily::DebianUbuntu:
        return {QStringLiteral("fcitx5"),
                QStringLiteral("fcitx5-chinese-addons"),
                QStringLiteral("fcitx5-config-qt"),
                QStringLiteral("fcitx5-frontend-gtk3"),
                QStringLiteral("fcitx5-frontend-gtk4"),
                QStringLiteral("fcitx5-frontend-qt5"),
                QStringLiteral("fcitx5-frontend-qt6"),
                QStringLiteral("fonts-noto-cjk"),
                QStringLiteral("fonts-noto-color-emoji"),
                QStringLiteral("im-config"),
                QStringLiteral("wl-clipboard"),
                QStringLiteral("locales")};
    case DistroFamily::FedoraRhel:
        return {QStringLiteral("fcitx5"),
                QStringLiteral("fcitx5-chinese-addons"),
                QStringLiteral("fcitx5-configtool"),
                QStringLiteral("fcitx5-gtk"),
                QStringLiteral("fcitx5-qt"),
                QStringLiteral("google-noto-sans-cjk-fonts"),
                QStringLiteral("google-noto-sans-mono-cjk-fonts"),
                QStringLiteral("google-noto-emoji-color-fonts"),
                QStringLiteral("wl-clipboard")};
    case DistroFamily::OpenSuse:
        return {QStringLiteral("fcitx5"),
                QStringLiteral("fcitx5-chinese-addons"),
                QStringLiteral("fcitx5-configtool"),
                QStringLiteral("fcitx5-gtk"),
                QStringLiteral("fcitx5-qt"),
                QStringLiteral("noto-sans-cjk-fonts"),
                QStringLiteral("noto-coloremoji-fonts"),
                QStringLiteral("wl-clipboard")};
    case DistroFamily::Unknown:
        break;
    }
    return {};
}

QStringList missingPackagesFromOutput(DistroFamily /*family*/,
                                        const QString &installed_query_output,
                                        const QStringList &required) {
    if (required.isEmpty()) {
        return {};
    }
    QSet<QString> installed;
    for (const QString &raw : installed_query_output.split('\n', Qt::SkipEmptyParts)) {
        QString name = raw.trimmed();
        const qsizetype colon = name.indexOf(':');
        if (colon > 0) {
            name = name.left(colon);
        }
        if (!name.isEmpty()) {
            installed.insert(name);
        }
    }
    QStringList missing;
    for (const QString &pkg : required) {
        if (!installed.contains(pkg)) {
            missing.append(pkg);
        }
    }
    return missing;
}

bool hasZhCnLocale(const QString &locale_a_output) {
    for (const QString &raw : locale_a_output.split('\n', Qt::SkipEmptyParts)) {
        const QString lower = raw.trimmed().toLower();
        if (lower == QStringLiteral("zh_cn.utf8") ||
            lower == QStringLiteral("zh_cn.utf-8")) {
            return true;
        }
    }
    return false;
}

bool isMonospaceFontChineseFriendly(const QString &mono_family,
                                    const QString &mono_zh_family) {
    const QString m1 = mono_family.trimmed().toLower();
    const QString m2 = mono_zh_family.trimmed().toLower();
    if (m1.isEmpty() || m2.isEmpty()) {
        return false;
    }
    if (m1.contains("cjk kr") || m1.contains("cjk jp") ||
        m2.contains("cjk kr") || m2.contains("cjk jp")) {
        return false;
    }
    static const QStringList kGoodZhIndicators = {
        QStringLiteral("cjk sc"),       QStringLiteral("mono sc"),
        QStringLiteral("han sans sc"),  QStringLiteral("han mono sc"),
        QStringLiteral("wenquanyi"),    QStringLiteral("sarasa"),
        QStringLiteral("maple"),        QStringLiteral("pingfang"),
        QStringLiteral("yahei"),        QStringLiteral("fangsong"),
        QStringLiteral("kaiti"),        QStringLiteral("heiti"),
        QStringLiteral("songti"),       QStringLiteral("lxgw"),
    };
    for (const QString &tag : kGoodZhIndicators) {
        if (m2.contains(tag)) {
            return true;
        }
    }
    return false;
}

QString generateFontconfigXml() {
    return QStringLiteral(R"(<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<!-- Generated by ShadowBar 中文环境修复：确保终端/代码与中文优先匹配简体中文等宽与无衬线字体 -->
<fontconfig>
  <match target="pattern">
    <test qual="any" name="family">
      <string>monospace</string>
    </test>
    <edit name="family" mode="prepend" binding="strong">
      <string>Noto Sans Mono</string>
      <string>JetBrains Mono</string>
      <string>DejaVu Sans Mono</string>
      <string>Noto Sans Mono CJK SC</string>
      <string>Source Han Mono SC</string>
      <string>Sarasa Mono SC</string>
      <string>WenQuanYi Micro Hei Mono</string>
      <string>Noto Color Emoji</string>
    </edit>
  </match>

  <match target="pattern">
    <test qual="any" name="family">
      <string>sans-serif</string>
    </test>
    <edit name="family" mode="prepend" binding="strong">
      <string>Noto Sans</string>
      <string>Noto Sans CJK SC</string>
      <string>Source Han Sans SC</string>
      <string>WenQuanYi Micro Hei</string>
      <string>Noto Color Emoji</string>
    </edit>
  </match>

  <match target="pattern">
    <test qual="any" name="family">
      <string>serif</string>
    </test>
    <edit name="family" mode="prepend" binding="strong">
      <string>Noto Serif</string>
      <string>Noto Serif CJK SC</string>
      <string>Source Han Serif SC</string>
      <string>Noto Color Emoji</string>
    </edit>
  </match>
</fontconfig>
)");
}

QString mergeFcitx5ProfileIni(const QString &existing_ini) {
    struct ProfileItem {
        QString name;
        QString layout;
    };
    struct ProfileGroup {
        QString header = QStringLiteral("Groups/0");
        QString name = QStringLiteral("默认");
        QString default_layout = QStringLiteral("us");
        QString default_im = QStringLiteral("pinyin");
        QList<ProfileItem> items;
        QStringList extra_lines;
    };

    const QString trimmed = existing_ini.trimmed();
    if (trimmed.isEmpty()) {
        return QStringLiteral(
            "[Groups/0]\n"
            "# Group Name\n"
            "Name=默认\n"
            "# Layout\n"
            "Default Layout=us\n"
            "# Default Input Method\n"
            "DefaultIM=pinyin\n\n"
            "[Groups/0/Items/0]\n"
            "# Name\n"
            "Name=keyboard-us\n"
            "# Layout\n"
            "# Layout=\n\n"
            "[Groups/0/Items/1]\n"
            "# Name\n"
            "Name=pinyin\n"
            "# Layout\n"
            "# Layout=\n\n"
            "[Groups/0/Items/2]\n"
            "# Name\n"
            "Name=shadowbar\n"
            "# Layout\n"
            "# Layout=\n\n"
            "[GroupOrder]\n"
            "0=默认\n");
    }

    QList<ProfileGroup> groups;
    QStringList trailing_sections;
    bool in_group_order = false;
    QStringList group_order_lines;
    int current_group_idx = -1;
    int current_item_idx = -1;
    QString current_other_section;

    static const QRegularExpression kGroupRe(QStringLiteral(R"(^\[(Groups/\d+)\]$)"));
    static const QRegularExpression kItemRe(
        QStringLiteral(R"(^\[(Groups/\d+)/Items/\d+\]$)"));

    const QStringList lines = existing_ini.split('\n');
    for (const QString &raw_line : lines) {
        const QString line = raw_line.endsWith('\r')
                                 ? raw_line.left(raw_line.size() - 1)
                                 : raw_line;
        const QString s = line.trimmed();
        if (s.startsWith('[') && s.endsWith(']')) {
            current_item_idx = -1;
            in_group_order = false;
            current_other_section.clear();

            if (s == QStringLiteral("[GroupOrder]")) {
                in_group_order = true;
                continue;
            }
            if (const auto gm = kGroupRe.match(s); gm.hasMatch()) {
                ProfileGroup g;
                g.header = gm.captured(1);
                g.name.clear();
                g.default_layout.clear();
                g.default_im.clear();
                groups.append(g);
                current_group_idx = static_cast<int>(groups.size()) - 1;
                continue;
            }
            if (const auto im = kItemRe.match(s); im.hasMatch()) {
                const QString gh = im.captured(1);
                int gidx = -1;
                for (int i = 0; i < groups.size(); ++i) {
                    if (groups[i].header == gh) {
                        gidx = i;
                        break;
                    }
                }
                if (gidx < 0) {
                    ProfileGroup g;
                    g.header = gh;
                    groups.append(g);
                    gidx = static_cast<int>(groups.size()) - 1;
                }
                current_group_idx = gidx;
                groups[gidx].items.append(ProfileItem{});
                current_item_idx = static_cast<int>(groups[gidx].items.size()) - 1;
                continue;
            }
            current_other_section = s;
            trailing_sections.append(line);
            continue;
        }

        if (in_group_order) {
            if (!s.isEmpty() && !s.startsWith('#')) {
                group_order_lines.append(line);
            }
            continue;
        }
        if (!current_other_section.isEmpty()) {
            trailing_sections.append(line);
            continue;
        }
        if (current_group_idx < 0) {
            continue;
        }
        ProfileGroup &grp = groups[current_group_idx];
        if (current_item_idx >= 0) {
            ProfileItem &item = grp.items[current_item_idx];
            if (s.startsWith(QStringLiteral("Name="))) {
                item.name = s.mid(5).trimmed();
            } else if (s.startsWith(QStringLiteral("Layout="))) {
                item.layout = s.mid(7).trimmed();
            }
            continue;
        }
        if (s.startsWith(QStringLiteral("Name="))) {
            grp.name = s.mid(5).trimmed();
        } else if (s.startsWith(QStringLiteral("Default Layout="))) {
            grp.default_layout = s.mid(15).trimmed();
        } else if (s.startsWith(QStringLiteral("DefaultIM="))) {
            grp.default_im = s.mid(10).trimmed();
        } else if (!s.isEmpty() && !s.startsWith('#')) {
            grp.extra_lines.append(line);
        }
    }

    if (groups.isEmpty()) {
        return mergeFcitx5ProfileIni(QString());
    }

    ProfileGroup &g0 = groups[0];
    if (g0.name.isEmpty()) {
        g0.name = QStringLiteral("默认");
    }
    if (g0.default_layout.isEmpty()) {
        g0.default_layout = QStringLiteral("us");
    }
    if (g0.default_im.isEmpty() || g0.default_im.startsWith(QStringLiteral("keyboard-"))) {
        g0.default_im = QStringLiteral("pinyin");
    }

    auto has_item_matching = [&](auto pred) {
        return std::any_of(g0.items.begin(), g0.items.end(),
                           [&](const ProfileItem &it) { return pred(it.name); });
    };
    if (!has_item_matching([](const QString &n) { return n.startsWith(QStringLiteral("keyboard-")); })) {
        g0.items.prepend(ProfileItem{QStringLiteral("keyboard-us"), {}});
    }
    if (!has_item_matching([](const QString &n) {
            return n == QStringLiteral("pinyin") || n == QStringLiteral("rime") ||
                   n == QStringLiteral("shuangpin") || n == QStringLiteral("wbx");
        })) {
        g0.items.append(ProfileItem{QStringLiteral("pinyin"), {}});
    }
    if (!has_item_matching([](const QString &n) { return n == QStringLiteral("shadowbar"); })) {
        g0.items.append(ProfileItem{QStringLiteral("shadowbar"), {}});
    }

    QString out;
    for (int gi = 0; gi < groups.size(); ++gi) {
        const ProfileGroup &g = groups[gi];
        out += QString("[%1]\n").arg(g.header);
        out += QString("Name=%1\n").arg(g.name);
        out += QString("Default Layout=%1\n").arg(g.default_layout.isEmpty() ? QStringLiteral("us") : g.default_layout);
        out += QString("DefaultIM=%1\n").arg(g.default_im);
        for (const QString &ex : g.extra_lines) {
            out += ex + "\n";
        }
        out += "\n";

        int item_num = 0;
        for (const ProfileItem &it : g.items) {
            if (it.name.isEmpty()) {
                continue;
            }
            out += QString("[%1/Items/%2]\n").arg(g.header).arg(item_num++);
            out += QString("Name=%1\n").arg(it.name);
            if (!it.layout.isEmpty()) {
                out += QString("Layout=%1\n").arg(it.layout);
            }
            out += "\n";
        }
    }

    for (const QString &tl : trailing_sections) {
        out += tl + "\n";
    }
    if (!trailing_sections.isEmpty() && !out.endsWith(QStringLiteral("\n\n"))) {
        out += "\n";
    }

    out += "[GroupOrder]\n";
    if (group_order_lines.isEmpty()) {
        for (int gi = 0; gi < groups.size(); ++gi) {
            out += QString("%1=%2\n").arg(gi).arg(groups[gi].name);
        }
    } else {
        for (const QString &gl : group_order_lines) {
            out += gl + "\n";
        }
    }
    return out;
}

QList<QPair<QString, QString>> buildImeEnvPairs(const ImeEnvPlanOptions &opts) {
    QList<QPair<QString, QString>> pairs = {
        // Qt 6.7+ prefers Wayland text-input first and falls back to fcitx on X11/XWayland.
        {QStringLiteral("QT_IM_MODULES"), QStringLiteral("wayland;fcitx")},
        {QStringLiteral("QT_IM_MODULE"), QStringLiteral("fcitx")},
        {QStringLiteral("GTK_IM_MODULE"), QStringLiteral("fcitx")},
        {QStringLiteral("XMODIFIERS"), QStringLiteral("@im=fcitx")},
        {QStringLiteral("SDL_IM_MODULE"), QStringLiteral("fcitx")},
        {QStringLiteral("GLFW_IM_MODULE"), QStringLiteral("ibus")},
        {QStringLiteral("INPUT_METHOD"), QStringLiteral("fcitx")},
    };
    if (!opts.addon_dirs.trimmed().isEmpty()) {
        pairs.append({QStringLiteral("FCITX_ADDON_DIRS"), opts.addon_dirs.trimmed()});
    }
    if (opts.include_zh_locale) {
        pairs.append({QStringLiteral("LANG"), QStringLiteral("zh_CN.UTF-8")});
        pairs.append({QStringLiteral("LC_CTYPE"), QStringLiteral("zh_CN.UTF-8")});
    }
    return pairs;
}

QString generateEnvironmentDContent(const ImeEnvPlanOptions &opts) {
    QString out = QStringLiteral(
        "# Generated by ShadowBar 中文环境与输入法一键修复 (Wayland + XWayland + X11)\n");
    for (const auto &kv : buildImeEnvPairs(opts)) {
        out += QString("%1=%2\n").arg(kv.first, kv.second);
    }
    return out;
}

QString generateXprofileExportBlock(const ImeEnvPlanOptions &opts) {
    QString out;
    for (const auto &kv : buildImeEnvPairs(opts)) {
        out += QString("export %1=\"%2\"\n").arg(kv.first, kv.second);
    }
    return out;
}

QString upsertManagedBlock(const QString &existing_text,
                           const QString &begin_marker,
                           const QString &end_marker,
                           const QString &body) {
    const QString block = begin_marker + "\n" + body.trimmed() + "\n" + end_marker;
    const qsizetype start = existing_text.indexOf(begin_marker);
    if (start >= 0) {
        const qsizetype end = existing_text.indexOf(end_marker, start);
        if (end >= start) {
            const qsizetype after = end + end_marker.size();
            QString updated = existing_text.left(start) + block + existing_text.mid(after);
            if (!updated.endsWith('\n')) {
                updated += '\n';
            }
            return updated;
        }
    }
    QString updated = existing_text;
    if (!updated.isEmpty() && !updated.endsWith('\n')) {
        updated += '\n';
    }
    if (!updated.isEmpty()) {
        updated += '\n';
    }
    updated += block + "\n";
    return updated;
}

QString buildPrivilegedSetupScript(DistroFamily family,
                                   const QStringList &missing_pkgs,
                                   bool need_zh_locale,
                                   bool set_system_locale) {
    QStringList lines;
    lines << QStringLiteral("#!/usr/bin/env bash")
          << QStringLiteral("set -euo pipefail");

    if (!missing_pkgs.isEmpty()) {
        QStringList quoted;
        for (const QString &p : missing_pkgs) {
            quoted << shellQuoteSingle(p);
        }
        const QString joined = quoted.join(' ');
        switch (family) {
        case DistroFamily::Arch:
            lines << QString("echo '[1/2] 正在通过 pacman 安装缺失软件与字体: %1'")
                         .arg(missing_pkgs.join(' '))
                  << QString("pacman -Sy --needed --noconfirm %1").arg(joined);
            break;
        case DistroFamily::DebianUbuntu:
            lines << QStringLiteral("export DEBIAN_FRONTEND=noninteractive")
                  << QStringLiteral("apt-get update")
                  << QStringLiteral("PKGS_TO_INSTALL=()")
                  << QString("for p in %1; do if apt-cache show \"$p\" >/dev/null 2>&1; then PKGS_TO_INSTALL+=(\"$p\"); fi; done")
                         .arg(joined)
                  << QStringLiteral("if [ ${#PKGS_TO_INSTALL[@]} -gt 0 ]; then apt-get install -y \"${PKGS_TO_INSTALL[@]}\"; fi");
            break;
        case DistroFamily::FedoraRhel:
            lines << QString("dnf install -y --skip-broken %1").arg(joined);
            break;
        case DistroFamily::OpenSuse:
            lines << QString("zypper --non-interactive install --no-recommends %1")
                         .arg(joined);
            break;
        case DistroFamily::Unknown:
            break;
        }
    }

    if (need_zh_locale) {
        lines << QStringLiteral("echo '[2/2] 正在启用并生成 zh_CN.UTF-8 系统 Locale…'")
              << QStringLiteral("if [ -f /etc/locale.gen ]; then")
              << QStringLiteral("  sed -i -E 's/^#\\s*(zh_CN\\.UTF-8\\s+UTF-8)/\\1/' /etc/locale.gen")
              << QStringLiteral("  grep -qE '^zh_CN\\.UTF-8\\s+UTF-8' /etc/locale.gen || echo 'zh_CN.UTF-8 UTF-8' >> /etc/locale.gen")
              << QStringLiteral("fi")
              << QStringLiteral("if command -v locale-gen >/dev/null 2>&1; then")
              << QStringLiteral("  locale-gen zh_CN.UTF-8 || locale-gen")
              << QStringLiteral("fi");
    }

    if (set_system_locale) {
        lines << QStringLiteral("if command -v localectl >/dev/null 2>&1; then localectl set-locale LANG=zh_CN.UTF-8 || true; fi");
    }
    return lines.join('\n') + "\n";
}

ZhEnvFixer::ZhEnvFixer(QObject *parent) : QObject(parent) {}

ZhEnvFixer::~ZhEnvFixer() {
    if (active_proc_) {
        active_proc_->kill();
        active_proc_->deleteLater();
        active_proc_ = nullptr;
    }
    if (worker_thread_) {
        worker_thread_->requestInterruption();
        worker_thread_->quit();
        worker_thread_->wait(3000);
        delete worker_thread_;
        worker_thread_ = nullptr;
    }
    if (!privileged_script_path_.isEmpty()) {
        QFile::remove(privileged_script_path_);
    }
}

ZhEnvDiagReport ZhEnvFixer::inspectEnvironment(const QString &config_home) {
    ZhEnvDiagReport rep;
    const QString cfg_home =
        config_home.trimmed().isEmpty()
            ? (qEnvironmentVariable("XDG_CONFIG_HOME").isEmpty()
                   ? QDir::homePath() + "/.config"
                   : qEnvironmentVariable("XDG_CONFIG_HOME"))
            : config_home.trimmed();

    rep.distro = detectDistroFamily(readTextFile(QStringLiteral("/etc/os-release")));
    rep.distro_name = distroFamilyDisplayName(rep.distro);
    rep.session_type = qEnvironmentVariable("XDG_SESSION_TYPE").trimmed().toLower();
    if (rep.session_type.isEmpty()) {
        rep.session_type = qEnvironmentVariable("WAYLAND_DISPLAY").isEmpty()
                               ? QStringLiteral("x11")
                               : QStringLiteral("wayland");
    }
    rep.desktop_name = qEnvironmentVariable("XDG_CURRENT_DESKTOP").trimmed();
    rep.current_lang = qEnvironmentVariable("LANG").trimmed();

    // 1. Packages & Locale
    const QStringList required = requiredPackagesForDistro(rep.distro);
    int pkg_rc = -1;
    QString installed_out;
    if (rep.distro == DistroFamily::Arch) {
        installed_out =
            runQuickProcess(QStringLiteral("pacman"), {QStringLiteral("-Qq")}, 5000, &pkg_rc);
    } else if (rep.distro == DistroFamily::DebianUbuntu) {
        installed_out = runQuickProcess(QStringLiteral("dpkg-query"),
                                        {QStringLiteral("-W"), QStringLiteral("-f=${Package}\n")},
                                        5000, &pkg_rc);
    } else if (rep.distro == DistroFamily::FedoraRhel ||
               rep.distro == DistroFamily::OpenSuse) {
        installed_out = runQuickProcess(
            QStringLiteral("rpm"), {QStringLiteral("-qa"), QStringLiteral("--qf=%{NAME}\n")},
            5000, &pkg_rc);
    }
    if (pkg_rc == 0) {
        rep.missing_packages =
            missingPackagesFromOutput(rep.distro, installed_out, required);
    }

    const QString locale_out = runQuickProcess(QStringLiteral("locale"), {QStringLiteral("-a")});
    rep.has_zh_locale = hasZhCnLocale(locale_out);

    ZhEnvDiagItem pkg_item;
    pkg_item.id = QStringLiteral("packages_locale");
    pkg_item.title = QStringLiteral("1. 输入法软件与中文 Locale");
    if (rep.distro == DistroFamily::Unknown) {
        pkg_item.ok = rep.has_zh_locale &&
                      !QStandardPaths::findExecutable(QStringLiteral("fcitx5")).isEmpty();
        pkg_item.warning = !pkg_item.ok;
        pkg_item.summary = pkg_item.ok ? QStringLiteral("核心组件与 zh_CN.UTF-8 就绪")
                                       : QStringLiteral("请确认已安装 fcitx5 与 zh_CN.UTF-8 Locale");
    } else {
        pkg_item.ok = rep.missing_packages.isEmpty() && rep.has_zh_locale;
        pkg_item.warning = !pkg_item.ok;
        if (pkg_item.ok) {
            pkg_item.summary = QString("%1 · 已装齐全部组件且 zh_CN.UTF-8 已启用").arg(rep.distro_name);
        } else {
            QStringList gaps;
            if (!rep.missing_packages.isEmpty()) {
                gaps << QString("缺失软件包: %1").arg(rep.missing_packages.join(", "));
            }
            if (!rep.has_zh_locale) {
                gaps << QStringLiteral("未生成 zh_CN.UTF-8 Locale");
            }
            pkg_item.summary = gaps.join(" · ");
        }
    }
    pkg_item.detail = QString("当前 LANG=%1").arg(
        rep.current_lang.isEmpty() ? QStringLiteral("(未设置)") : rep.current_lang);
    rep.items.append(pkg_item);

    // 2. Monospace & Chinese fontconfig
    rep.mono_family = runQuickProcess(
        QStringLiteral("fc-match"), {QStringLiteral("--format=%{family}"), QStringLiteral("monospace")});
    rep.mono_cjk_family = runQuickProcess(
        QStringLiteral("fc-match"),
        {QStringLiteral("--format=%{family}"), QStringLiteral("monospace:charset=4e2d")});
    rep.font_ok = isMonospaceFontChineseFriendly(rep.mono_family, rep.mono_cjk_family);

    const QString fc_conf_path = cfg_home + "/fontconfig/conf.d/64-shadowbar-zh-fonts.conf";
    ZhEnvDiagItem font_item;
    font_item.id = QStringLiteral("fonts");
    font_item.title = QStringLiteral("2. 中文与等宽字体匹配");
    font_item.ok = rep.font_ok;
    font_item.warning = !rep.font_ok;
    if (rep.font_ok) {
        font_item.summary = QString("等宽主字体: %1 · 中文回退: %2")
                                .arg(rep.mono_family, rep.mono_cjk_family);
    } else {
        font_item.summary = QString("当前 monospace=%1，中文回退=%2（建议修复为简体中文等宽回退）")
                                .arg(rep.mono_family.isEmpty() ? "?" : rep.mono_family,
                                     rep.mono_cjk_family.isEmpty() ? "?" : rep.mono_cjk_family);
    }
    font_item.detail = QString("配置文件: %1").arg(fc_conf_path);
    rep.items.append(font_item);

    // 3. Fcitx5 Chinese IME profile & running status
    const QString profile_path = cfg_home + "/fcitx5/profile";
    const QString profile_text = readTextFile(profile_path);
    rep.profile_ok = profileHasRequiredIms(profile_text);
    int pgrep_rc = -1;
    runQuickProcess(QStringLiteral("pgrep"), {QStringLiteral("-x"), QStringLiteral("fcitx5")},
                    2000, &pgrep_rc);
    rep.fcitx_running = (pgrep_rc == 0);

    ZhEnvDiagItem prof_item;
    prof_item.id = QStringLiteral("fcitx5_profile");
    prof_item.title = QStringLiteral("3. Fcitx5 中文输入法列表");
    prof_item.ok = rep.profile_ok;
    prof_item.warning = !rep.profile_ok;
    prof_item.summary = rep.profile_ok
                            ? QString("已初始化 keyboard-us + pinyin + shadowbar (%1)")
                                  .arg(rep.fcitx_running ? "Fcitx5 运行中" : "Fcitx5 未启动")
                            : QStringLiteral("未初始化默认中文拼音与 ShadowBar 输入法配置");
    prof_item.detail = profile_path;
    rep.items.append(prof_item);

    // 4. Wayland + X11 environment propagation
    const QString envd_ime = cfg_home + "/environment.d/60-fcitx5-shadowbar-ime.conf";
    const QString envd_text = readTextFile(envd_ime);
    const QString live_xmod = qEnvironmentVariable("XMODIFIERS");
    const QString live_qt_mods = qEnvironmentVariable("QT_IM_MODULES");
    const QString live_gtk = qEnvironmentVariable("GTK_IM_MODULE");
    const bool live_env_ok =
        !live_xmod.isEmpty() && (!live_qt_mods.isEmpty() || !qEnvironmentVariable("QT_IM_MODULE").isEmpty()) &&
        !live_gtk.isEmpty();
    const bool persisted_env_ok =
        envd_text.contains(QStringLiteral("XMODIFIERS=")) &&
        envd_text.contains(QStringLiteral("QT_IM_MODULES="));

    bool wayland_launcher_ok = true;
    QString wayland_note;
    if (rep.session_type == QStringLiteral("wayland") &&
        rep.desktop_name.contains(QStringLiteral("KDE"), Qt::CaseInsensitive) &&
        !QStandardPaths::findExecutable(QStringLiteral("kreadconfig6")).isEmpty()) {
        const QString kwin_im = runQuickProcess(
            QStringLiteral("kreadconfig6"),
            {QStringLiteral("--file"), QStringLiteral("kwinrc"),
             QStringLiteral("--group"), QStringLiteral("Wayland"),
             QStringLiteral("--key"), QStringLiteral("InputMethod")});
        wayland_launcher_ok = kwin_im.contains(QStringLiteral("fcitx5-wayland-launcher.desktop"));
        wayland_note = wayland_launcher_ok ? QStringLiteral("KWin Wayland 输入法已绑定")
                                           : QStringLiteral("KWin Wayland 输入法尚未绑定");
    }

    rep.env_ok = (live_env_ok && persisted_env_ok && wayland_launcher_ok);
    ZhEnvDiagItem env_item;
    env_item.id = QStringLiteral("env_wayland_x11");
    env_item.title = QStringLiteral("4. Wayland + X11 环境变量与会话集成");
    env_item.ok = rep.env_ok;
    env_item.warning = !rep.env_ok;
    env_item.summary =
        rep.env_ok
            ? QString("会话 (%1 / %2) 双端输入法环境变量与持久配置均已就绪")
                  .arg(rep.session_type, rep.desktop_name.isEmpty() ? "Desktop" : rep.desktop_name)
            : QString("待完善：%1%2%3")
                  .arg(live_env_ok ? "" : "当前进程 IM 环境变量缺失 ",
                       persisted_env_ok ? "" : "environment.d 持久配置未写入 ",
                       wayland_note.isEmpty() || wayland_launcher_ok ? "" : wayland_note);
    env_item.detail = QString("environment.d: %1 · ~/.xprofile").arg(envd_ime);
    rep.items.append(env_item);

    return rep;
}

void ZhEnvFixer::runDiagnose() {
    if (busy_ || worker_thread_) {
        return;
    }
    startWorkerTask(false);
}

void ZhEnvFixer::applyEnvVarsToCurrentProcess(const ImeEnvPlanOptions &opts) {
    for (const auto &kv : buildImeEnvPairs(opts)) {
        qputenv(kv.first.toUtf8().constData(), kv.second.toUtf8());
    }
}

void ZhEnvFixer::runFix(bool install_missing_packages, bool apply_zh_locale) {
    if (busy_ || worker_thread_) {
        return;
    }
    busy_ = true;
    pending_apply_zh_locale_ = apply_zh_locale;

    last_report_ = inspectEnvironment();
    emit diagnoseFinished(last_report_);
    emit logLine(QString("=== 开始中文环境与输入法一键修复 (%1 / %2) ===")
                     .arg(last_report_.distro_name, last_report_.session_type));

    const QStringList to_install = install_missing_packages ? last_report_.missing_packages
                                                            : QStringList{};
    const bool need_locale = install_missing_packages && !last_report_.has_zh_locale;
    if ((!to_install.isEmpty() || need_locale) &&
        last_report_.distro != DistroFamily::Unknown) {
        const QString pkexec_bin = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
        if (pkexec_bin.isEmpty()) {
            emit logLine("⚠ 未找到 pkexec，跳过系统软件与 Locale 安装步骤，继续执行用户级修复。");
            continueFixAfterPrivilegedStep(true);
            return;
        }

        const QString script = buildPrivilegedSetupScript(
            last_report_.distro, to_install, need_locale, apply_zh_locale);
        QTemporaryFile tmp(QDir::tempPath() + "/shadowbar_zh_fix_XXXXXX.sh");
        tmp.setAutoRemove(false);
        if (!tmp.open()) {
            emit logLine("⚠ 无法创建临时提权脚本，跳过软件安装步骤。");
            continueFixAfterPrivilegedStep(false);
            return;
        }
        tmp.write(script.toUtf8());
        tmp.flush();
        tmp.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                           QFileDevice::ExeOwner);
        privileged_script_path_ = tmp.fileName();
        tmp.close();

        emit logLine(QString("[步骤 1/4] 正在请求管理员权限执行软件/Locale 修复…"));
        active_proc_ = new QProcess(this);
        active_proc_->setProcessChannelMode(QProcess::MergedChannels);
        connect(active_proc_, &QProcess::readyReadStandardOutput, this, [this]() {
            if (!active_proc_) {
                return;
            }
            const QString chunk =
                QString::fromUtf8(active_proc_->readAllStandardOutput());
            for (const QString &l : chunk.split('\n', Qt::SkipEmptyParts)) {
                emit logLine("  " + l.trimmed());
            }
        });
        connect(active_proc_,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int code, QProcess::ExitStatus status) {
                    if (!privileged_script_path_.isEmpty()) {
                        QFile::remove(privileged_script_path_);
                        privileged_script_path_.clear();
                    }
                    if (active_proc_) {
                        active_proc_->deleteLater();
                        active_proc_ = nullptr;
                    }
                    const bool ok = (status == QProcess::NormalExit && code == 0);
                    emit logLine(ok ? "✓ 软件与 Locale 修复完成"
                                    : QString("⚠ 提权安装步骤退出 (code=%1)，继续用户级配置修复…")
                                          .arg(code));
                    continueFixAfterPrivilegedStep(ok);
                });
        active_proc_->start(pkexec_bin, {QStringLiteral("/bin/bash"), privileged_script_path_});
        return;
    }

    emit logLine("[步骤 1/4] ✓ 必要软件与 zh_CN.UTF-8 Locale 已就绪，无需提权安装");
    continueFixAfterPrivilegedStep(true);
}

void ZhEnvFixer::continueFixAfterPrivilegedStep(bool /*privileged_ok*/) {
    ImeEnvPlanOptions env_opts;
    env_opts.include_zh_locale = pending_apply_zh_locale_;
    env_opts.addon_dirs = defaultAddonDirsValue();
    applyEnvVarsToCurrentProcess(env_opts);
    startWorkerTask(true);
}

void ZhEnvFixer::startWorkerTask(bool fix_mode) {
    ImeEnvPlanOptions env_opts;
    env_opts.include_zh_locale = pending_apply_zh_locale_;
    env_opts.addon_dirs = defaultAddonDirsValue();

    const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
    const QString session = qEnvironmentVariable("XDG_SESSION_TYPE");
    const QString cfg_home =
        qEnvironmentVariable("XDG_CONFIG_HOME").isEmpty()
            ? QDir::homePath() + "/.config"
            : qEnvironmentVariable("XDG_CONFIG_HOME");
    const QString home_dir = QDir::homePath();

    QPointer<ZhEnvFixer> self(this);
    worker_thread_ = QThread::create(
        [self, fix_mode, env_opts, desktop, session, cfg_home, home_dir]() {
            auto post_log = [self](const QString &line) {
                if (!self) {
                    return;
                }
                QMetaObject::invokeMethod(
                    self.data(),
                    [self, line]() {
                        if (self) {
                            emit self->logLine(line);
                        }
                    },
                    Qt::QueuedConnection);
            };

            bool all_steps_ok = true;
            if (fix_mode) {
                QString err;
                // Step 2: Monospace + Chinese Fontconfig
                const QString fc_path =
                    cfg_home + "/fontconfig/conf.d/64-shadowbar-zh-fonts.conf";
                if (writeTextFile(fc_path, generateFontconfigXml(), &err)) {
                    post_log(QString("[步骤 2/4] ✓ 已写入中文等宽字体配置: %1").arg(fc_path));
                    int fc_rc = -1;
                    runQuickProcess(QStringLiteral("fc-cache"),
                                    {QStringLiteral("-f")}, 15000, &fc_rc);
                    post_log(fc_rc == 0
                                 ? QStringLiteral("  ✓ 已刷新字体缓存 (fc-cache -f)")
                                 : QStringLiteral("  ⚠ fc-cache -f 执行返回非零，可稍后手动运行 fc-cache -f"));
                } else {
                    all_steps_ok = false;
                    post_log(QString("[步骤 2/4] ✕ %1").arg(err));
                }

                // Step 3: Initialize Fcitx5 Chinese IME profile
                const QString prof_path = cfg_home + "/fcitx5/profile";
                const QString old_prof = readTextFile(prof_path);
                const QString merged_prof = mergeFcitx5ProfileIni(old_prof);
                if (merged_prof != old_prof) {
                    if (!old_prof.isEmpty() &&
                        !QFileInfo::exists(prof_path + ".bak.shadowbar")) {
                        QFile::copy(prof_path, prof_path + ".bak.shadowbar");
                    }
                    if (writeTextFile(prof_path, merged_prof, &err)) {
                        post_log(
                            QString("[步骤 3/4] ✓ 已初始化 Fcitx5 中文输入法列表 (keyboard-us, pinyin, shadowbar): %1")
                                .arg(prof_path));
                    } else {
                        all_steps_ok = false;
                        post_log(QString("[步骤 3/4] ✕ %1").arg(err));
                    }
                } else {
                    post_log(QStringLiteral("[步骤 3/4] ✓ Fcitx5 输入法配置已包含拼音与 ShadowBar"));
                }

                // Step 4: Wayland + X11 Dual-Stack Env Propagation & Session Integration
                const QString envd_ime_path =
                    cfg_home + "/environment.d/60-fcitx5-shadowbar-ime.conf";
                if (writeTextFile(envd_ime_path,
                                  generateEnvironmentDContent(env_opts), &err)) {
                    post_log(QString("[步骤 4/4] ✓ 已写入 systemd environment.d: %1")
                                 .arg(envd_ime_path));
                } else {
                    all_steps_ok = false;
                    post_log(QString("[步骤 4/4] ✕ %1").arg(err));
                }

                const QString xprofile_path = home_dir + "/.xprofile";
                const QString old_xprof = readTextFile(xprofile_path);
                const QString new_xprof = upsertManagedBlock(
                    old_xprof, QStringLiteral("# >>> shadowbar-zh-env >>>"),
                    QStringLiteral("# <<< shadowbar-zh-env <<<"),
                    generateXprofileExportBlock(env_opts));
                if (writeTextFile(xprofile_path, new_xprof, &err)) {
                    post_log(QString("  ✓ 已更新 X11 会话脚本: %1").arg(xprofile_path));
                } else {
                    all_steps_ok = false;
                    post_log(QString("  ✕ 更新 .xprofile 失败: %1").arg(err));
                }

                const auto pairs = buildImeEnvPairs(env_opts);
                QStringList kv_args;
                for (const auto &kv : pairs) {
                    kv_args << QString("%1=%2").arg(kv.first, kv.second);
                }
                if (!QStandardPaths::findExecutable(QStringLiteral("systemctl"))
                         .isEmpty()) {
                    int rc = -1;
                    QStringList args = {QStringLiteral("--user"),
                                        QStringLiteral("set-environment")};
                    args.append(kv_args);
                    runQuickProcess(QStringLiteral("systemctl"), args, 5000, &rc);
                    if (rc == 0) {
                        post_log(QStringLiteral("  ✓ 已注入当前 systemd --user 运行时环境变量"));
                    }
                }
                if (!QStandardPaths::findExecutable(
                         QStringLiteral("dbus-update-activation-environment"))
                         .isEmpty()) {
                    int rc = -1;
                    QStringList args = {QStringLiteral("--systemd")};
                    args.append(kv_args);
                    runQuickProcess(
                        QStringLiteral("dbus-update-activation-environment"), args,
                        5000, &rc);
                    if (rc == 0) {
                        post_log(QStringLiteral("  ✓ 已同步 D-Bus 激活环境变量"));
                    }
                }

                if (!QStandardPaths::findExecutable(QStringLiteral("im-config"))
                         .isEmpty()) {
                    int rc = -1;
                    runQuickProcess(QStringLiteral("im-config"),
                                    {QStringLiteral("-n"), QStringLiteral("fcitx5")},
                                    5000, &rc);
                    if (rc == 0) {
                        post_log(QStringLiteral("  ✓ 已通过 im-config 注册 fcitx5 (Wayland/X11)"));
                    }
                }

                const bool is_kde =
                    desktop.contains(QStringLiteral("KDE"), Qt::CaseInsensitive);
                const QString kwin_launcher = QStringLiteral(
                    "/usr/share/applications/fcitx5-wayland-launcher.desktop");
                if (is_kde &&
                    !QStandardPaths::findExecutable(QStringLiteral("kwriteconfig6"))
                         .isEmpty() &&
                    QFileInfo::exists(kwin_launcher)) {
                    int rc2 = -1;
                    if (session.compare(QStringLiteral("wayland"),
                                        Qt::CaseInsensitive) == 0) {
                        int rc1 = -1;
                        runQuickProcess(
                            QStringLiteral("kwriteconfig6"),
                            {QStringLiteral("--file"), QStringLiteral("kwinrc"),
                             QStringLiteral("--group"), QStringLiteral("Wayland"),
                             QStringLiteral("--key"), QStringLiteral("InputMethod"),
                             QStringLiteral("--notify"), QStringLiteral("")},
                            3000, &rc1);
                        QThread::msleep(350);
                    }
                    runQuickProcess(
                        QStringLiteral("kwriteconfig6"),
                        {QStringLiteral("--file"), QStringLiteral("kwinrc"),
                         QStringLiteral("--group"), QStringLiteral("Wayland"),
                         QStringLiteral("--key"), QStringLiteral("InputMethod"),
                         QStringLiteral("--notify"), kwin_launcher},
                        3000, &rc2);
                    if (rc2 == 0) {
                        post_log(QStringLiteral(
                            "  ✓ 已配置 KDE KWin Wayland 虚拟键盘为 fcitx5-wayland-launcher"));
                    }
                } else if (QFileInfo::exists(QStringLiteral(
                               "/usr/share/applications/org.fcitx.Fcitx5.desktop"))) {
                    const QString autostart_dest =
                        cfg_home + "/autostart/org.fcitx.Fcitx5.desktop";
                    if (!QFileInfo::exists(autostart_dest)) {
                        QDir().mkpath(cfg_home + "/autostart");
                        if (QFile::copy(QStringLiteral("/usr/share/applications/"
                                                       "org.fcitx.Fcitx5.desktop"),
                                        autostart_dest)) {
                            post_log(QString("  ✓ 已启用桌面自启动: %1")
                                         .arg(autostart_dest));
                        }
                    }
                }

                const QString qdbus6 =
                    QStandardPaths::findExecutable(QStringLiteral("qdbus6"));
                int pgrep_rc = -1;
                runQuickProcess(QStringLiteral("pgrep"),
                                {QStringLiteral("-x"), QStringLiteral("fcitx5")},
                                2000, &pgrep_rc);
                if (pgrep_rc == 0 && !qdbus6.isEmpty()) {
                    int rc = -1;
                    runQuickProcess(qdbus6,
                                    {QStringLiteral("org.fcitx.Fcitx5"),
                                     QStringLiteral("/controller"),
                                     QStringLiteral("org.fcitx.Fcitx.Controller1.Restart")},
                                    4000, &rc);
                    QThread::msleep(500);
                    post_log(rc == 0
                                 ? QStringLiteral("  ✓ 已通知运行中的 Fcitx5 平滑重启并加载中文配置")
                                 : QStringLiteral("  ⚠ Fcitx5 D-Bus 重启未响应，将在下次启动时生效"));
                } else if (const QString f5 = QStandardPaths::findExecutable(
                               QStringLiteral("fcitx5"));
                           !f5.isEmpty()) {
                    QProcess::startDetached(f5, {QStringLiteral("-r"), QStringLiteral("-d")});
                    QThread::msleep(500);
                    post_log(QStringLiteral("  ✓ 已在后台拉起 Fcitx5 守护进程 (fcitx5 -r -d)"));
                }
            }

            const ZhEnvDiagReport rep = inspectEnvironment(cfg_home);
            if (!self) {
                return;
            }
            QMetaObject::invokeMethod(
                self.data(),
                [self, fix_mode, all_steps_ok, rep]() {
                    if (!self) {
                        return;
                    }
                    if (self->worker_thread_) {
                        self->worker_thread_->wait(2000);
                        self->worker_thread_->deleteLater();
                        self->worker_thread_ = nullptr;
                    }
                    self->last_report_ = rep;
                    emit self->diagnoseFinished(rep);
                    if (fix_mode) {
                        self->busy_ = false;
                        const QString summary =
                            (all_steps_ok && rep.allOk())
                                ? QStringLiteral(
                                      "✓ 中文环境、等宽字体、Fcitx5 输入法及 Wayland/X11 环境变量已全部修复就绪！")
                                : QStringLiteral(
                                      "✓ 用户级修复已完成（若刚新装输入法包或首次写入环境变量，重新登录桌面会话后所有程序生效）");
                        emit self->logLine(summary);
                        emit self->fixFinished(all_steps_ok, summary);
                    }
                },
                Qt::QueuedConnection);
        });
    worker_thread_->start();
}

} // namespace shadowbar::floating_ui
