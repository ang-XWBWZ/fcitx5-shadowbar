#include "shadowbar/c_api.h"

#include "shadowbar/config.hpp"
#include "shadowbar/session.hpp"
#include "shadowbar/translator.hpp"

#include <mutex>
#include <string>

struct ShadowBarHandle {
    shadowbar::ConfigManager config_mgr;
    shadowbar::ShadowSession session;
    shadowbar::AsyncTranslationEngine translator;
    std::mutex mu;

    ShadowBarUiUpdateFn ui_cb = nullptr;
    void *ui_user_data = nullptr;

    // Backing buffers for C string pointers returned in snapshots.
    std::string buf_badge;
    std::string buf_source;
    std::string buf_translation;
    std::string buf_hint;
    std::string buf_commit;
    std::string buf_replay;
    std::string buf_toast;

    explicit ShadowBarHandle(const char *config_path)
        : config_mgr(config_path && *config_path
                         ? std::filesystem::path(config_path)
                         : shadowbar::ConfigManager::defaultConfigPath()) {
        config_mgr.ensureAndLoad();
        session.updateConfig(config_mgr.config());
        translator.setCacheCapacity(config_mgr.config().cache_size);
        translator.reloadOfflineDictionaries(
            config_mgr.config().effectiveOfflineDicts());
    }

    ShadowBarViewSnapshot buildViewLocked() {
        auto vm = session.viewModel();
        buf_badge = vm.badge;
        buf_source = vm.source_text;
        buf_translation = vm.translation_text;
        buf_hint = vm.status_hint;
        return ShadowBarViewSnapshot{
            .mode = static_cast<int>(vm.mode),
            .bar_visible = vm.bar_visible ? 1 : 0,
            .status = static_cast<int>(vm.status),
            .revision = session.revision(),
            .badge = buf_badge.c_str(),
            .source_text = buf_source.c_str(),
            .cursor_byte = vm.cursor_byte,
            .translation_text = buf_translation.c_str(),
            .status_hint = buf_hint.c_str(),
        };
    }

    ShadowBarActionSnapshot dispatchActionsLocked(const shadowbar::SessionActions &act) {
        if (act.cancel_translation) {
            translator.cancelAll();
        }
        if (act.trigger_translation) {
            const auto &cfg = session.config();
            shadowbar::TranslationJob job{
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
            translator.submit(std::move(job),
                              [this](uint64_t rev, const shadowbar::TranslationResult &res) {
                                  std::lock_guard<std::mutex> lock(mu);
                                  if (session.onTranslationUpdate(rev, res) && ui_cb) {
                                      auto snap = buildViewLocked();
                                      ui_cb(ui_user_data, &snap);
                                  }
                              });
        }
        buf_commit = act.commit_to_app;
        buf_replay = act.replay_raw_prefix;
        buf_toast = act.toast_message;
        return ShadowBarActionSnapshot{
            .consume_key = act.consume_key ? 1 : 0,
            .update_ui = act.update_ui ? 1 : 0,
            .commit_to_app = buf_commit.c_str(),
            .replay_raw_prefix = buf_replay.c_str(),
            .toast_message = buf_toast.c_str(),
        };
    }
};

extern "C" {

ShadowBarHandle *shadowbar_create(const char *config_path) {
    return new ShadowBarHandle(config_path);
}

void shadowbar_destroy(ShadowBarHandle *handle) {
    delete handle;
}

void shadowbar_set_ui_callback(ShadowBarHandle *handle,
                               ShadowBarUiUpdateFn cb,
                               void *user_data) {
    if (!handle) {
        return;
    }
    std::lock_guard<std::mutex> lock(handle->mu);
    handle->ui_cb = cb;
    handle->ui_user_data = user_data;
}

ShadowBarActionSnapshot shadowbar_toggle_sticky(ShadowBarHandle *handle) {
    if (!handle) {
        return {};
    }
    std::lock_guard<std::mutex> lock(handle->mu);
    return handle->dispatchActionsLocked(handle->session.toggleSticky());
}

ShadowBarActionSnapshot shadowbar_on_key(ShadowBarHandle *handle,
                                         int special_key,
                                         const char *utf8_char,
                                         int modifiers,
                                         int ime_composing,
                                         int in_cjk_im) {
    if (!handle) {
        return {};
    }
    shadowbar::KeyInput k;
    k.special = static_cast<shadowbar::SpecialKey>(special_key);
    if (utf8_char) {
        k.utf8_char = utf8_char;
    }
    k.shift = (modifiers & 1) != 0;
    k.ctrl = (modifiers & 2) != 0;
    k.alt = (modifiers & 4) != 0;
    k.super = (modifiers & 8) != 0;

    std::lock_guard<std::mutex> lock(handle->mu);
    return handle->dispatchActionsLocked(
        handle->session.onKey(k, ime_composing != 0, in_cjk_im != 0));
}

ShadowBarActionSnapshot shadowbar_on_ime_commit(ShadowBarHandle *handle,
                                                const char *utf8_text) {
    if (!handle) {
        return {};
    }
    std::lock_guard<std::mutex> lock(handle->mu);
    return handle->dispatchActionsLocked(
        handle->session.onCommitFromIme(utf8_text ? utf8_text : ""));
}

ShadowBarViewSnapshot shadowbar_get_view(ShadowBarHandle *handle) {
    if (!handle) {
        return {};
    }
    std::lock_guard<std::mutex> lock(handle->mu);
    return handle->buildViewLocked();
}

} // extern "C"
