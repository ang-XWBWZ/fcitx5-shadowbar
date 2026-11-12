#include "clipboard_monitor.hpp"
#include "hotkey_util.hpp"
#include "ty_model_helpers.hpp"
#include "zh_env_fixer.hpp"

#include <QDir>
#include <QFile>
#include <QKeySequence>
#include <QList>
#include <QTemporaryDir>
#include <QXmlStreamReader>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>

int main() {
    using shadowbar::floating_ui::fcitxToQtKeySequenceString;
    using shadowbar::floating_ui::fcitxToQtKeySequences;
    using shadowbar::floating_ui::qtKeySequenceToFcitx;

    assert(fcitxToQtKeySequenceString("Alt+Shift+Q") == "Alt+Shift+Q");
    assert(fcitxToQtKeySequenceString("Control+Shift+t Alt+Shift+Q") ==
           "Ctrl+Shift+T");
    assert(fcitxToQtKeySequenceString("Super+BackSpace") == "Meta+Backspace");
    assert(fcitxToQtKeySequenceString("   ").isEmpty());

    const QList<QKeySequence> multi =
        fcitxToQtKeySequences("Alt+Shift+Q Control+Alt+q Alt+Shift+q");
    assert(multi.size() == 2);
    assert(multi[0] == QKeySequence("Alt+Shift+Q"));
    assert(multi[1] == QKeySequence("Ctrl+Alt+Q"));

    const QList<QKeySequence> single = fcitxToQtKeySequences("Control+Shift+T");
    assert(single.size() == 1);
    assert(qtKeySequenceToFcitx(single[0]) == "Control+Shift+T");

    const QList<QKeySequence> esc = fcitxToQtKeySequences("Control+Escape");
    assert(esc.size() == 1);
    assert(esc[0] == QKeySequence("Ctrl+Esc"));

    // TY model helper tests
    using namespace shadowbar::floating_ui;
    assert(normalizeTyVariant("STQ1_0") == "1.25bit");
    assert(normalizeTyVariant("2bit") == "2bit");
    assert(normalizeTyVariant("custom") == "custom");
    assert(normalizeTyVariant("unknown") == "2bit");

    assert(normalizeTyBackend("auto") == "auto");
    assert(normalizeTyBackend("Vulkan") == "vulkan");
    assert(normalizeTyBackend("cpu") == "cpu");
    assert(normalizeTyBackend("weird") == "auto");
    assert(tyBackendGpuLayers("auto") == 99);
    assert(tyBackendGpuLayers("vulkan") == 99);
    assert(tyBackendGpuLayers("cpu") == 0);

    const QStringList args_vk = buildTyServerArgs("/tmp/m.gguf", 18081, 0, 2048, 99);
    assert(args_vk.contains("-ngl"));
    assert(args_vk.contains("99"));
    assert(args_vk.contains("-lv"));
    assert(args_vk.contains("4"));

    assert(tyDownloadUrl("1.25bit", "huggingface") ==
           "https://huggingface.co/AngelSlim/Hy-MT1.5-1.8B-1.25bit-GGUF/resolve/main/Hy-MT1.5-1.8B-1.25bit.gguf");
    assert(tyDownloadUrl("2bit", "hf-mirror") ==
           "https://hf-mirror.com/AngelSlim/Hy-MT1.5-1.8B-2bit-GGUF/resolve/main/Hy-MT1.5-1.8B-2bit.gguf");
    assert(tyDownloadUrl("custom", "huggingface").isEmpty());

    const QStringList args_auto = buildTyServerArgs("/tmp/m.gguf", 18081, 0);
    assert(args_auto.contains("-m"));
    assert(args_auto.contains("--port"));
    assert(args_auto.contains("18081"));
    assert(!args_auto.contains("-t"));

    const QStringList args_th = buildTyServerArgs("/tmp/m.gguf", 99999, 8);
    assert(args_th.contains("65535"));
    assert(args_th.contains("-t"));
    assert(args_th.contains("8"));

    shadowbar::ShadowConfig cfg;
    cfg.hymt_port = 18085;
    cfg.hymt_model_125bit_path = "/opt/models/custom125.gguf";
    assert(resolvedTyModelPath(cfg, "1.25bit") == "/opt/models/custom125.gguf");
    assert(resolvedTyModelPath(cfg, "2bit").endsWith("/Hy-MT1.5-1.8B-2bit.gguf"));

    upsertTyPreset(cfg, "1.25bit", true, "en");
    assert(cfg.active_preset == "ty_125bit");
    const shadowbar::Preset *p125 = cfg.findPreset("ty_125bit");
    assert(p125 != nullptr);
    assert(p125->endpoint == "http://127.0.0.1:18085/v1/chat/completions");
    assert(p125->model == "hy-mt1.5-1.8b-1.25bit");
    assert(p125->system_prompt.empty());
    assert(inferTyVariantForPreset(*p125, 18085) == "1.25bit");
    assert(inferTyVariantForPreset(*p125, 18081).isEmpty());

    shadowbar::Preset non_ty = *p125;
    non_ty.model = "gpt-4o-mini";
    assert(inferTyVariantForPreset(non_ty, 18085).isEmpty());

    const TyServerLogSignal sig_none = inspectTyServerLogLine(
        "warning: no usable GPU found, --gpu-layers option will be ignored");
    assert(sig_none.no_vulkan_device);
    assert(!sig_none.vulkan_device_found);
    assert(!sig_none.has_layer_offload);

    const TyServerLogSignal sig_zero = inspectTyServerLogLine(
        "ggml_vulkan: Found 0 Vulkan devices:");
    assert(sig_zero.no_vulkan_device);
    assert(!sig_zero.vulkan_device_found);

    const TyServerLogSignal sig_dev = inspectTyServerLogLine(
        "0.00.054 I cmn common_param:   - Vulkan0 : AMD Radeon 880M Graphics (RADV STRIX1)");
    assert(sig_dev.vulkan_device_found);
    assert(!sig_dev.no_vulkan_device);

    const TyServerLogSignal sig_partial = inspectTyServerLogLine(
        "0.00.367 I load_tensors: offloaded 12/33 layers to GPU");
    assert(sig_partial.has_layer_offload);
    assert(sig_partial.offloaded_layers == 12);
    assert(sig_partial.total_layers == 33);

    const TyServerLogSignal sig_full = inspectTyServerLogLine(
        "0.00.367 I load_tensors: offloaded 33/33 layers to GPU");
    assert(sig_full.has_layer_offload);
    assert(sig_full.offloaded_layers == 33);
    assert(sig_full.total_layers == 33);

    // Clipboard monitor avoidance tests
    assert(classifyAvoidedMimeTypes({}) == ClipboardAvoidReason::Empty);
    assert(classifyAvoidedMimeTypes({"text/plain", "x-kde-passwordManagerHint"}) ==
           ClipboardAvoidReason::Sensitive);
    assert(classifyAvoidedMimeTypes({"x-kde-lockscreen", "text/plain"}) ==
           ClipboardAvoidReason::Sensitive);
    assert(classifyAvoidedMimeTypes({"text/uri-list", "text/plain"}) ==
           ClipboardAvoidReason::FileOrPath);
    assert(classifyAvoidedMimeTypes({"x-special/gnome-copied-files", "text/plain"}) ==
           ClipboardAvoidReason::FileOrPath);
    assert(classifyAvoidedMimeTypes({"application/x-kde-cutselection", "text/plain"}) ==
           ClipboardAvoidReason::FileOrPath);
    assert(classifyAvoidedMimeTypes({"text/x-moz-url", "text/plain"}) ==
           ClipboardAvoidReason::UrlOrLink);
    assert(classifyAvoidedMimeTypes({"image/png"}) ==
           ClipboardAvoidReason::BinaryOrNonText);
    assert(classifyAvoidedMimeTypes({"text/plain;charset=utf-8", "image/png"}) ==
           ClipboardAvoidReason::None);

    assert(classifyAvoidedClipboardText("   \n  ") ==
           ClipboardAvoidReason::Empty);
    assert(classifyAvoidedClipboardText("file:///home/user/report.pdf") ==
           ClipboardAvoidReason::FileOrPath);
    assert(classifyAvoidedClipboardText("copy\nfile:///tmp/a.txt\nfile:///tmp/b.txt") ==
           ClipboardAvoidReason::FileOrPath);
    assert(classifyAvoidedClipboardText("/home/user/projects/fcitx5-shadowbar/CMakeLists.txt") ==
           ClipboardAvoidReason::FileOrPath);
    assert(classifyAvoidedClipboardText("~/Documents/notes.md") ==
           ClipboardAvoidReason::FileOrPath);
    assert(classifyAvoidedClipboardText("./build/shadowbar-float") ==
           ClipboardAvoidReason::FileOrPath);
    assert(classifyAvoidedClipboardText("C:\\Users\\demo\\file.txt") ==
           ClipboardAvoidReason::FileOrPath);

    assert(classifyAvoidedClipboardText("https://example.com/docs?q=1#top") ==
           ClipboardAvoidReason::UrlOrLink);
    assert(classifyAvoidedClipboardText("[ShadowBar Docs](https://example.com/guide)") ==
           ClipboardAvoidReason::UrlOrLink);
    assert(classifyAvoidedClipboardText("www.example.com/release") ==
           ClipboardAvoidReason::UrlOrLink);
    assert(classifyAvoidedClipboardText("docs.example.org/api") ==
           ClipboardAvoidReason::UrlOrLink);

    // Normal prose sentences mentioning links, versions, or titles must still translate
    assert(classifyAvoidedClipboardText("Read https://example.com for details.") ==
           ClipboardAvoidReason::None);
    assert(classifyAvoidedClipboardText("Hello v1.2 release notes") ==
           ClipboardAvoidReason::None);
    assert(classifyAvoidedClipboardText("Dr. Smith met Prof. Lee.") ==
           ClipboardAvoidReason::None);
    assert(classifyAvoidedClipboardText("请翻译这段普通中文文本。") ==
           ClipboardAvoidReason::None);
    assert(!clipboardAvoidReasonText(ClipboardAvoidReason::FileOrPath).isEmpty());
    assert(!clipboardAvoidReasonText(ClipboardAvoidReason::UrlOrLink).isEmpty());
    assert(clipboardAvoidReasonText(ClipboardAvoidReason::None).isEmpty());

    // ZhEnvFixer pure planners & checkers tests
    assert(detectDistroFamily("ID=arch\nNAME=\"Arch Linux\"\n") ==
           DistroFamily::Arch);
    assert(detectDistroFamily("ID=ubuntu\nID_LIKE=debian\n") ==
           DistroFamily::DebianUbuntu);
    assert(detectDistroFamily("ID=fedora\n") == DistroFamily::FedoraRhel);
    assert(detectDistroFamily("ID=\"opensuse-tumbleweed\"\n") ==
           DistroFamily::OpenSuse);
    assert(detectDistroFamily("ID=alpine\n") == DistroFamily::Unknown);

    const QStringList arch_pkgs = requiredPackagesForDistro(DistroFamily::Arch);
    assert(arch_pkgs.contains("fcitx5-chinese-addons"));
    assert(arch_pkgs.contains("noto-fonts-cjk"));
    assert(missingPackagesFromOutput(
               DistroFamily::Arch, "fcitx5\nnoto-fonts\n", arch_pkgs)
               .contains("fcitx5-chinese-addons"));
    assert(missingPackagesFromOutput(
               DistroFamily::DebianUbuntu,
               "fcitx5:amd64\nfcitx5-chinese-addons:all\n",
               {"fcitx5", "fcitx5-chinese-addons", "im-config"}) ==
           QStringList({"im-config"}));

    assert(hasZhCnLocale("C\nen_US.utf8\nzh_CN.utf8\n"));
    assert(hasZhCnLocale("C.UTF-8\nzh_CN.UTF-8\n"));
    assert(!hasZhCnLocale("C\nen_US.utf8\nPOSIX\n"));

    assert(!isMonospaceFontChineseFriendly("Noto Sans Mono CJK KR",
                                           "Noto Sans Mono CJK KR"));
    assert(!isMonospaceFontChineseFriendly("Noto Sans Mono",
                                           "Noto Sans Mono CJK JP"));
    assert(isMonospaceFontChineseFriendly("Noto Sans Mono",
                                          "Noto Sans Mono CJK SC"));
    assert(isMonospaceFontChineseFriendly("JetBrains Mono",
                                          "WenQuanYi Micro Hei Mono"));

    const QString fc_xml = generateFontconfigXml();
    assert(fc_xml.contains("Noto Sans Mono CJK SC"));
    assert(fc_xml.contains("<string>monospace</string>"));
    {
        QXmlStreamReader xr(fc_xml);
        while (!xr.atEnd()) {
            xr.readNext();
        }
        assert(!xr.hasError());
    }

    // Empty Fcitx5 profile -> full keyboard-us + pinyin + shadowbar
    const QString prof_empty = mergeFcitx5ProfileIni("");
    assert(prof_empty.contains("DefaultIM=pinyin"));
    assert(prof_empty.contains("Name=keyboard-us"));
    assert(prof_empty.contains("Name=pinyin"));
    assert(prof_empty.contains("Name=shadowbar"));

    // Keyboard-only profile -> switches DefaultIM=pinyin & adds pinyin + shadowbar
    const QString prof_kb_only = mergeFcitx5ProfileIni(
        "[Groups/0]\nName=Default\nDefault Layout=us\nDefaultIM=keyboard-us\n\n"
        "[Groups/0/Items/0]\nName=keyboard-us\n\n[GroupOrder]\n0=Default\n");
    assert(prof_kb_only.contains("DefaultIM=pinyin"));
    assert(prof_kb_only.contains("Name=pinyin"));
    assert(prof_kb_only.contains("Name=shadowbar"));

    // Existing Rime + secondary Group 1 -> preserves DefaultIM=rime & Group 1
    const QString prof_rime = mergeFcitx5ProfileIni(
        "[Groups/0]\nName=默认\nDefault Layout=us\nDefaultIM=rime\n\n"
        "[Groups/0/Items/0]\nName=keyboard-us\n\n"
        "[Groups/0/Items/1]\nName=rime\n\n"
        "[Groups/1]\nName=Work\nDefault Layout=us\nDefaultIM=keyboard-us\n\n"
        "[Groups/1/Items/0]\nName=keyboard-us\n\n"
        "[GroupOrder]\n0=默认\n1=Work\n");
    assert(prof_rime.contains("DefaultIM=rime"));
    assert(prof_rime.contains("Name=rime"));
    assert(prof_rime.contains("Name=shadowbar"));
    assert(prof_rime.contains("[Groups/1]"));
    assert(!prof_rime.contains("[Groups/0/Items/2]\nName=pinyin"));

    // Environment generation & idempotent block upsert
    ImeEnvPlanOptions env_opts;
    env_opts.include_zh_locale = true;
    env_opts.addon_dirs = "/tmp/addon1:/usr/lib/fcitx5";
    const QString envd = generateEnvironmentDContent(env_opts);
    assert(envd.contains("QT_IM_MODULES=wayland;fcitx"));
    assert(envd.contains("QT_IM_MODULE=fcitx"));
    assert(envd.contains("GTK_IM_MODULE=fcitx"));
    assert(envd.contains("XMODIFIERS=@im=fcitx"));
    assert(envd.contains("FCITX_ADDON_DIRS=/tmp/addon1:/usr/lib/fcitx5"));
    assert(envd.contains("LANG=zh_CN.UTF-8"));

    const QString xprof_1 = upsertManagedBlock(
        "export FOO=bar\n",
        "# >>> shadowbar-zh-env >>>",
        "# <<< shadowbar-zh-env <<<",
        generateXprofileExportBlock(env_opts));
    assert(xprof_1.contains("export FOO=bar"));
    assert(xprof_1.contains("export XMODIFIERS=\"@im=fcitx\""));
    const QString xprof_2 = upsertManagedBlock(
        xprof_1,
        "# >>> shadowbar-zh-env >>>",
        "# <<< shadowbar-zh-env <<<",
        "export UPDATED=1\n");
    assert(xprof_2.contains("export FOO=bar"));
    assert(xprof_2.contains("export UPDATED=1"));
    assert(!xprof_2.contains("export XMODIFIERS=\"@im=fcitx\""));

    const QString priv_arch = buildPrivilegedSetupScript(
        DistroFamily::Arch, {"fcitx5", "noto-fonts-cjk"}, true, true);
    assert(priv_arch.contains("pacman -Sy --needed --noconfirm"));
    assert(priv_arch.contains("locale-gen zh_CN.UTF-8"));
    assert(priv_arch.contains("localectl set-locale LANG=zh_CN.UTF-8"));

    const QString priv_deb = buildPrivilegedSetupScript(
        DistroFamily::DebianUbuntu, {"fcitx5"}, false, false);
    assert(priv_deb.contains("apt-get install -y"));
    assert(!priv_deb.contains("locale-gen"));

    // Isolated temp XDG_CONFIG_HOME inspection check
    {
        QTemporaryDir tmp_cfg;
        assert(tmp_cfg.isValid());
        const ZhEnvDiagReport empty_rep =
            ZhEnvFixer::inspectEnvironment(tmp_cfg.path());
        assert(!empty_rep.profile_ok);
        assert(!empty_rep.env_ok);
        assert(empty_rep.items.size() == 4);

        QDir(tmp_cfg.path()).mkpath("fcitx5");
        QFile pf(tmp_cfg.path() + "/fcitx5/profile");
        assert(pf.open(QIODevice::WriteOnly | QIODevice::Text));
        pf.write(prof_empty.toUtf8());
        pf.close();
        const ZhEnvDiagReport prof_rep =
            ZhEnvFixer::inspectEnvironment(tmp_cfg.path());
        assert(prof_rep.profile_ok);
    }

    std::cout << "All floating UI helper tests passed!\n";
    return 0;
}
