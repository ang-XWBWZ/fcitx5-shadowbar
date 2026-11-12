#include "shadowbar/config.hpp"
#include "shadowbar/session.hpp"
#include "shadowbar/translator.hpp"
#include "shadowbar/utf8_util.hpp"

#include <iostream>
#include <mutex>
#include <string>

using namespace shadowbar;

namespace {

void printView(const ShadowSession &session) {
    const auto vm = session.viewModel();
    std::cout << "\n────────────────────────────────────────────\n";
    std::cout << "模式: "
              << (vm.mode == Mode::Off
                      ? "Off"
                      : (vm.mode == Mode::OneShot ? "OneShot(一次性)" : "Sticky(常驻)"))
              << " | 面板可见: " << (vm.bar_visible ? "是" : "否") << "\n";
    if (vm.bar_visible) {
        std::string src = vm.source_text;
        src.insert(std::min(vm.cursor_byte, src.size()), "|");
        std::cout << "│ [" << vm.badge << "] "
                  << (vm.source_text.empty() ? "请输入…" : src) << "\n";
        std::cout << "│ ✦ "
                  << (vm.translation_text.empty() ? "···" : vm.translation_text);
        if (!vm.status_hint.empty()) {
            std::cout << "  (" << vm.status_hint << ")";
        }
        std::cout << "\n";
    }
    std::cout << "────────────────────────────────────────────\n";
}

} // namespace

int main(int argc, char **argv) {
    std::filesystem::path cfg_path = ConfigManager::defaultConfigPath();
    if (argc > 1) {
        cfg_path = argv[1];
    }

    ConfigManager cfg_mgr(cfg_path);
    cfg_mgr.ensureAndLoad();

    ShadowSession session(cfg_mgr.config());
    AsyncTranslationEngine translator(curlHttpTransport,
                                      cfg_mgr.config().cache_size);
    translator.reloadOfflineDictionaries(
        cfg_mgr.config().effectiveOfflineDicts());
    std::recursive_mutex mu;

    std::cout << "ShadowBar 独立沙盒模拟器 (无需重启 Fcitx5)\n";
    std::cout << "配置文件: " << cfg_mgr.path() << "\n";
    std::cout << "命令:\n"
              << "  /toggle          切换 Sticky 模式\n"
              << "  /oneshot         进入 OneShot 模式\n"
              << "  /tab             切换译文风格预设\n"
              << "  /lang            切换目标语言\n"
              << "  /enter           模拟 Enter (上屏译文)\n"
              << "  /shift-enter     模拟 Shift+Enter (上屏中文原文)\n"
              << "  /bs              模拟 BackSpace\n"
              << "  /esc             模拟 Esc\n"
              << "  /quit            退出\n"
              << "  <任意文字>       模拟输入法选词上屏汉字或英文 (输入 ;; 进入快译)\n";

    auto dispatch = [&](const SessionActions &act) {
        if (act.cancel_translation) {
            translator.cancelAll();
        }
        if (!act.commit_to_app.empty()) {
            std::cout << "\n>>> [写入目标应用]: " << act.commit_to_app << "\n";
        }
        if (!act.toast_message.empty()) {
            std::cout << "\n[提示]: " << act.toast_message << "\n";
        }
        if (act.trigger_translation) {
            const auto &cfg = session.config();
            TranslationJob job{
                .revision = session.revision(),
                .source_text = session.viewModel().source_text,
                .source_lang = cfg.source_lang,
                .preset = session.activePreset(),
                .immediate = act.immediate_translation,
                .lock_on_punctuation = cfg.lock_on_punctuation,
                .offline_fallback = cfg.offline_dict_fallback,
                .debounce_ms = cfg.debounce_ms,
                .timeout_ms = cfg.request_timeout_ms,
            };
            translator.submit(std::move(job), [&](uint64_t rev,
                                                  const TranslationResult &res) {
                std::lock_guard<std::recursive_mutex> lock(mu);
                if (session.onTranslationUpdate(rev, res)) {
                    printView(session);
                }
            });
        }
    };

    std::string line;
    while (true) {
        std::cout << "\n输入> " << std::flush;
        if (!std::getline(std::cin, line)) {
            break;
        }
        if (cfg_mgr.reloadIfModified()) {
            std::lock_guard<std::recursive_mutex> lock(mu);
            session.updateConfig(cfg_mgr.config());
            translator.reloadOfflineDictionaries(
                cfg_mgr.config().effectiveOfflineDicts());
            std::cout << "[热重载] 已刷新 config.toml\n";
        }

        if (line == "/quit" || line == "/q") {
            break;
        }
        std::lock_guard<std::recursive_mutex> lock(mu);
        if (line == "/toggle") {
            dispatch(session.toggleSticky());
        } else if (line == "/oneshot") {
            dispatch(session.enterOneShot());
        } else if (line == "/tab") {
            dispatch(session.cyclePreset());
        } else if (line == "/lang") {
            dispatch(session.cycleTargetLang());
        } else if (line == "/enter") {
            dispatch(session.onKey({.special = SpecialKey::Enter}, false, true));
        } else if (line == "/shift-enter") {
            dispatch(session.onKey(
                {.special = SpecialKey::Enter, .shift = true}, false, true));
        } else if (line == "/bs") {
            dispatch(session.onKey({.special = SpecialKey::Backspace}, false, true));
        } else if (line == "/esc") {
            dispatch(session.onKey({.special = SpecialKey::Escape}, false, true));
        } else if (line == ";;" && !session.isActive()) {
            dispatch(session.onKey({.utf8_char = ";"}, false, true));
            dispatch(session.onKey({.utf8_char = ";"}, false, true));
        } else if (!session.isActive()) {
            std::cout << ">>> [直接输出到应用]: " << line << "\n";
        } else {
            dispatch(session.onCommitFromIme(line));
        }
        printView(session);
    }

    translator.shutdown();
    return 0;
}
