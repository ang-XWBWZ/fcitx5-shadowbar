#include "shadowbar_addon.hpp"
#include "shadowbar/utf8_util.hpp"

#include <fcitx-config/iniparser.h>
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/misc.h>
#if defined(__has_include) && __has_include(<fcitx-utils/standardpaths.h>)
#include <fcitx-utils/standardpaths.h>
#define SHADOWBAR_HAS_FCITX_STANDARDPATHS 1
#else
#include <fcitx-utils/standardpath.h>
#define SHADOWBAR_HAS_FCITX_STANDARDPATHS 0
#endif
#include <fcitx-utils/utf8.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/inputmethodgroup.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/inputpanel.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace shadowbar::fcitx_addon {

namespace {

#ifndef SHADOWBAR_FCITX5_MAJOR
#define SHADOWBAR_FCITX5_MAJOR 5
#define SHADOWBAR_FCITX5_MINOR 0
#define SHADOWBAR_FCITX5_PATCH 0
#endif

#define SHADOWBAR_FCITX5_AT_LEAST(major, minor, patch)                         \
    ((SHADOWBAR_FCITX5_MAJOR > (major)) ||                                     \
     (SHADOWBAR_FCITX5_MAJOR == (major) &&                                     \
      (SHADOWBAR_FCITX5_MINOR > (minor) ||                                     \
       (SHADOWBAR_FCITX5_MINOR == (minor) &&                                   \
        SHADOWBAR_FCITX5_PATCH >= (patch)))))

/** Show an Fcitx5 popup toast, falling back gracefully on Fcitx5 < 5.1.11. */
void showToast(fcitx::Instance *instance,
               fcitx::InputContext *ic,
               const std::string &message) {
    if (!instance || !ic || message.empty()) {
        return;
    }
#if SHADOWBAR_FCITX5_AT_LEAST(5, 1, 11)
    instance->showCustomInputMethodInformation(ic, message);
#else
    (void)message;
    instance->showInputMethodInformation(ic);
#endif
}

#if SHADOWBAR_HAS_FCITX_STANDARDPATHS
constexpr auto kFcitxPkgConfigType = fcitx::StandardPathsType::PkgConfig;
#else
constexpr auto kFcitxPkgConfigType = fcitx::StandardPath::Type::PkgConfig;
#endif

std::filesystem::path userPkgConfigConfPath() {
#if SHADOWBAR_HAS_FCITX_STANDARDPATHS
    return std::filesystem::path(
               fcitx::StandardPaths::global().userDirectory(kFcitxPkgConfigType)) /
           "conf" / "shadowbar.conf";
#else
    return std::filesystem::path(
               fcitx::StandardPath::global().userDirectory(kFcitxPkgConfigType)) /
           "conf" / "shadowbar.conf";
#endif
}

/** Set display hotkey metadata on an Action when Fcitx5 supports it (>= 5.1.11). */
void setActionHotkey(fcitx::Action &action, const fcitx::KeyList &keys) {
#if SHADOWBAR_FCITX5_AT_LEAST(5, 1, 11)
    action.setHotkey(keys);
#else
    (void)action;
    (void)keys;
#endif
}

/** Locate shadowbar-float across user-local and standard system install prefixes. */
std::string resolveFloatingBinary() {
    if (const char *override_bin = std::getenv("SHADOWBAR_FLOAT_BIN");
        override_bin && *override_bin) {
        return override_bin;
    }
    std::error_code ec;
    if (const char *home = std::getenv("HOME"); home && *home) {
        const auto local_bin =
            std::filesystem::path(home) / ".local" / "bin" / "shadowbar-float";
        if (std::filesystem::exists(local_bin, ec)) {
            return local_bin.string();
        }
    }
    for (const char *candidate : {"/usr/local/bin/shadowbar-float",
                                  "/usr/bin/shadowbar-float"}) {
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return "shadowbar-float";
}

bool hasGuiDisplayEnv() {
    const char *wl = std::getenv("WAYLAND_DISPLAY");
    const char *x11 = std::getenv("DISPLAY");
    return (wl && *wl) || (x11 && *x11);
}

ShadowProviderOption toFcitxProvider(ProviderKind kind) {
    switch (kind) {
    case ProviderKind::GoogleFree:
        return ShadowProviderOption::Google;
    case ProviderKind::OpenAICompatible:
        return ShadowProviderOption::OpenAI;
    case ProviderKind::Ollama:
        return ShadowProviderOption::Ollama;
    case ProviderKind::DeepL:
        return ShadowProviderOption::DeepL;
    case ProviderKind::Offline:
        return ShadowProviderOption::Offline;
    }
    return ShadowProviderOption::Google;
}

ProviderKind fromFcitxProvider(ShadowProviderOption opt) {
    switch (opt) {
    case ShadowProviderOption::Google:
        return ProviderKind::GoogleFree;
    case ShadowProviderOption::OpenAI:
        return ProviderKind::OpenAICompatible;
    case ShadowProviderOption::Ollama:
        return ProviderKind::Ollama;
    case ShadowProviderOption::DeepL:
        return ProviderKind::DeepL;
    case ShadowProviderOption::Offline:
        return ProviderKind::Offline;
    }
    return ProviderKind::GoogleFree;
}

bool isIgnoredContext(const fcitx::InputContext *ic) {
    if (!ic) {
        return true;
    }
    const auto flags = ic->capabilityFlags();
    return flags.testAny(fcitx::CapabilityFlag::PasswordOrSensitive) ||
           flags.test(fcitx::CapabilityFlag::Disable);
}

void appendSourceWithCursor(fcitx::Text &out,
                            std::string_view text,
                            size_t cursor_byte) {
    cursor_byte = std::min(cursor_byte, text.size());
    if (cursor_byte > 0) {
        out.append(std::string(text.substr(0, cursor_byte)));
    }
    out.append("\xE2\x94\x82", fcitx::TextFormatFlag::Bold);
    if (cursor_byte < text.size()) {
        out.append(std::string(text.substr(cursor_byte)));
    }
}

class ShadowBarCandidateList final : public fcitx::CommonCandidateList {
public:
    ShadowBarCandidateList() {
        setLayoutHint(fcitx::CandidateLayoutHint::Vertical);
        setPageSize(6);
        setCursorIncludeUnselected(true);
        setCursorKeepInSamePage(true);
    }
};

class ShadowBarCandidateWord final : public fcitx::CandidateWord {
public:
    ShadowBarCandidateWord(ShadowBarAddon *addon,
                           ShadowBarAddon::CandidateAction action,
                           std::string label,
                           fcitx::Text text,
                           fcitx::Text comment = {})
        : CandidateWord(std::move(text)),
          addon_(addon),
          action_(action) {
        setCustomLabel(fcitx::Text(std::move(label)));
        if (!comment.empty()) {
            setComment(std::move(comment));
        }
    }

    void select(fcitx::InputContext *ic) const override {
        if (addon_ && ic) {
            addon_->executeCandidateAction(ic, action_);
        }
    }

private:
    ShadowBarAddon *addon_;
    ShadowBarAddon::CandidateAction action_;
};

void clearShadowBarCandidateList(fcitx::InputPanel &panel) {
    if (std::dynamic_pointer_cast<const ShadowBarCandidateList>(
            panel.candidateList())) {
        panel.setCandidateList(nullptr);
    }
}

} // namespace

ShadowBarToggleAction::ShadowBarToggleAction(ShadowBarAddon *addon)
    : addon_(addon) {
    setCheckable(true);
}

std::string ShadowBarToggleAction::shortText(fcitx::InputContext *ic) const {
    return addon_->actionShortText(ic);
}

std::string ShadowBarToggleAction::longText(fcitx::InputContext *) const {
    return "开启 ShadowBar 实时翻译或切回纯中文输入 (快捷键 Ctrl+Shift+T)";
}

std::string ShadowBarToggleAction::icon(fcitx::InputContext *) const {
    return "accessories-dictionary";
}

bool ShadowBarToggleAction::isChecked(fcitx::InputContext *ic) const {
    return addon_->isActionChecked(ic);
}

void ShadowBarToggleAction::activate(fcitx::InputContext *ic) {
    addon_->toggleStickyFromAction(ic);
}

ShadowBarAddon::ShadowBarAddon(fcitx::Instance *instance)
    : instance_(instance),
      main_thread_id_(std::this_thread::get_id()),
      factory_([this](fcitx::InputContext &) {
          return new ShadowBarICState(config_mgr_.config());
      }),
      toggle_action_(this) {
    config_mgr_.ensureAndLoad();
    translator_.setCacheCapacity(config_mgr_.config().cache_size);
    translator_.reloadOfflineDictionaries(
        config_mgr_.config().effectiveOfflineDicts());

    instance_->inputContextManager().registerProperty("shadowbarState", &factory_);
    instance_->userInterfaceManager().registerAction("shadowbar-toggle",
                                                     &toggle_action_);
    setupStatusMenu();

    reloadConfig();
    if (config_mgr_.config().floating_enabled &&
        config_mgr_.config().floating_show_orb) {
        launchFloatingUi("--sync-orb");
    }

    commit_filter_conn_ = instance_->connect<fcitx::Instance::CommitFilter>(
        [this](fcitx::InputContext *ic, std::string &orig) {
            onCommitFilter(ic, orig);
        });

    event_watchers_.emplace_back(
        instance_->watchEvent(fcitx::EventType::InputContextKeyEvent,
                              fcitx::EventWatcherPhase::PreInputMethod,
                              [this](fcitx::Event &event) {
                                  onKeyEventPre(static_cast<fcitx::KeyEvent &>(event));
                              }));

    event_watchers_.emplace_back(
        instance_->watchEvent(fcitx::EventType::InputContextKeyEvent,
                              fcitx::EventWatcherPhase::PostInputMethod,
                              [this](fcitx::Event &event) {
                                  onKeyEventPost(static_cast<fcitx::KeyEvent &>(event));
                              }));

    event_watchers_.emplace_back(
        instance_->watchEvent(fcitx::EventType::InputContextUpdateUI,
                              fcitx::EventWatcherPhase::PreInputMethod,
                              [this](fcitx::Event &event) {
                                  onUpdateUI(
                                      static_cast<fcitx::InputContextUpdateUIEvent &>(
                                          event));
                              }));

    event_watchers_.emplace_back(
        instance_->watchEvent(fcitx::EventType::InputContextFocusIn,
                              fcitx::EventWatcherPhase::Default,
                              [this](fcitx::Event &event) {
                                  auto *ic = static_cast<fcitx::FocusInEvent &>(event).inputContext();
                                  if (isIgnoredContext(ic)) {
                                      return;
                                  }
                                  attachStatusAction(ic);
                                  if (auto *state = ic->propertyFor(&factory_)) {
                                      if (isStickyInputMethod(ic) &&
                                          state->session.mode() == Mode::Off &&
                                          !state->im_forced_sticky &&
                                          !state->im_user_paused) {
                                          enterStickyMode(ic, state, false);
                                      }
                                      if (shouldShowBarForPanel(ic, state)) {
                                          renderPanel(ic, state, false, true);
                                      }
                                  }
                              }));

    event_watchers_.emplace_back(
        instance_->watchEvent(fcitx::EventType::InputContextInputMethodActivated,
                              fcitx::EventWatcherPhase::Default,
                              [this](fcitx::Event &event) {
                                  auto &ev = static_cast<fcitx::InputMethodActivatedEvent &>(event);
                                  auto *ic = ev.inputContext();
                                  if (isIgnoredContext(ic)) {
                                      return;
                                  }
                                  attachStatusAction(ic);
                                  auto *state = ic->propertyFor(&factory_);
                                  if (!state) {
                                      return;
                                  }
                                  if (ev.name() == "shadowbar") {
                                      state->im_user_paused = false;
                                      state->im_forced_sticky = true;
                                      if (state->session.mode() != Mode::Sticky) {
                                          auto act = state->session.enterSticky(true);
                                          handleSessionActions(ic, state, act);
                                      }
                                  } else if (state->im_forced_sticky || state->im_user_paused) {
                                      state->im_forced_sticky = false;
                                      state->im_user_paused = false;
                                      if (state->session.mode() != Mode::Off) {
                                          auto act = state->session.exitToOff();
                                          handleSessionActions(ic, state, act);
                                      }
                                  }
                                  toggle_action_.update(ic);
                              }));

    event_watchers_.emplace_back(
        instance_->watchEvent(fcitx::EventType::InputContextInputMethodDeactivated,
                              fcitx::EventWatcherPhase::Default,
                              [this](fcitx::Event &event) {
                                  auto &ev = static_cast<fcitx::InputMethodDeactivatedEvent &>(event);
                                  auto *ic = ev.inputContext();
                                  if (!ic) {
                                      return;
                                  }
                                  if (auto *state = ic->propertyFor(&factory_);
                                      state && ev.name() == "shadowbar") {
                                      const bool was_forced = state->im_forced_sticky;
                                      state->im_forced_sticky = false;
                                      state->im_user_paused = false;
                                      if (was_forced && state->session.mode() != Mode::Off) {
                                          auto act = state->session.exitToOff();
                                          handleSessionActions(ic, state, act);
                                      }
                                      toggle_action_.update(ic);
                                  }
                              }));

    event_watchers_.emplace_back(
        instance_->watchEvent(fcitx::EventType::InputContextFocusOut,
                              fcitx::EventWatcherPhase::Default,
                              [this](fcitx::Event &event) {
                                  auto &ev = static_cast<fcitx::FocusOutEvent &>(event);
                                  onFocusOutOrReset(ev.inputContext());
                              }));

    event_watchers_.emplace_back(
        instance_->watchEvent(fcitx::EventType::InputContextReset,
                              fcitx::EventWatcherPhase::Default,
                              [this](fcitx::Event &event) {
                                  auto &ev = static_cast<fcitx::ResetEvent &>(event);
                                  onFocusOutOrReset(ev.inputContext());
                              }));
}

ShadowBarAddon::~ShadowBarAddon() {
    translator_.shutdown();
}

void ShadowBarAddon::syncFcitxConfigFromToml() {
    const auto &cfg = config_mgr_.config();
    fcitx_config_.toggleHotkey.setValue(
        fcitx::Key::keyListFromString(cfg.toggle_hotkey));
    setActionHotkey(toggle_action_, fcitx_config_.toggleHotkey.value());
    fcitx_config_.cycleLangHotkey.setValue(
        fcitx::Key::keyListFromString(cfg.cycle_lang_hotkey));
    fcitx_config_.cyclePresetHotkey.setValue(
        fcitx::Key::keyListFromString(cfg.cycle_preset_hotkey));
    fcitx_config_.oneshotHotkey.setValue(
        fcitx::Key::keyListFromString(cfg.oneshot_hotkey));
    fcitx_config_.enablePrefix.setValue(cfg.enable_prefix);
    fcitx_config_.prefix.setValue(cfg.prefix);
    fcitx_config_.prefixOnlyInCjkIm.setValue(cfg.prefix_only_in_cjk_im);
    fcitx_config_.debounceMs.setValue(cfg.debounce_ms);
    fcitx_config_.targetLang.setValue(cfg.target_lang);
    fcitx_config_.activePreset.setValue(cfg.active_preset);
    fcitx_config_.expandActions.setValue(cfg.expand_actions);
    fcitx_config_.floatingEnabled.setValue(cfg.floating_enabled);
    fcitx_config_.floatingHotkey.setValue(
        fcitx::Key::keyListFromString(cfg.floating_hotkey));
    fcitx_config_.floatingAutoReadClipboard.setValue(
        cfg.floating_auto_read_clipboard);
    fcitx_config_.floatingShowOrb.setValue(cfg.floating_show_orb);
    if (const auto *preset = cfg.findPreset(cfg.active_preset)) {
        fcitx_config_.provider.setValue(toFcitxProvider(preset->provider));
        fcitx_config_.endpoint.setValue(preset->endpoint);
        fcitx_config_.apiKey.setValue(preset->api_key);
        fcitx_config_.model.setValue(preset->model);
    }
    updateStatusMenuState();
}

void ShadowBarAddon::applyFcitxConfigToToml() {
    auto &cfg = config_mgr_.mutableConfig();
    cfg.toggle_hotkey =
        fcitx::Key::keyListToString(fcitx_config_.toggleHotkey.value());
    cfg.cycle_lang_hotkey =
        fcitx::Key::keyListToString(fcitx_config_.cycleLangHotkey.value());
    cfg.cycle_preset_hotkey =
        fcitx::Key::keyListToString(fcitx_config_.cyclePresetHotkey.value());
    cfg.oneshot_hotkey =
        fcitx::Key::keyListToString(fcitx_config_.oneshotHotkey.value());
    cfg.enable_prefix = fcitx_config_.enablePrefix.value();
    cfg.prefix = fcitx_config_.prefix.value();
    cfg.prefix_only_in_cjk_im = fcitx_config_.prefixOnlyInCjkIm.value();
    cfg.debounce_ms = fcitx_config_.debounceMs.value();
    cfg.expand_actions = fcitx_config_.expandActions.value();
    cfg.floating_enabled = fcitx_config_.floatingEnabled.value();
    cfg.floating_hotkey =
        fcitx::Key::keyListToString(fcitx_config_.floatingHotkey.value());
    cfg.floating_auto_read_clipboard =
        fcitx_config_.floatingAutoReadClipboard.value();
    cfg.floating_show_orb = fcitx_config_.floatingShowOrb.value();
    if (!fcitx_config_.targetLang.value().empty()) {
        cfg.target_lang = fcitx_config_.targetLang.value();
    }
    if (!fcitx_config_.activePreset.value().empty() &&
        cfg.findPreset(fcitx_config_.activePreset.value())) {
        cfg.active_preset = fcitx_config_.activePreset.value();
    }
    if (auto *preset = cfg.findPresetMutable(cfg.active_preset)) {
        preset->provider = fromFcitxProvider(fcitx_config_.provider.value());
        preset->endpoint = fcitx_config_.endpoint.value();
        preset->api_key = fcitx_config_.apiKey.value();
        preset->model = fcitx_config_.model.value();
    }
    config_mgr_.save();
    updateStatusMenuState();
}

void ShadowBarAddon::syncAllSessionsConfig() {
    instance_->inputContextManager().foreach([this](fcitx::InputContext *ic) {
        if (auto *state = ic->propertyFor(&factory_)) {
            state->session.updateConfig(config_mgr_.config());
        }
        return true;
    });
}

void ShadowBarAddon::persistSelectionDefaults(fcitx::InputContext *source_ic,
                                              ShadowBarICState *source_state) {
    if (!source_state) {
        return;
    }
    const std::string &new_preset = source_state->session.activePresetId();
    const std::string &new_lang = source_state->session.rawTargetLang();

    auto &cfg = config_mgr_.mutableConfig();
    if (cfg.active_preset == new_preset && cfg.target_lang == new_lang) {
        return;
    }
    cfg.active_preset = new_preset;
    cfg.target_lang = new_lang;
    fcitx_config_.activePreset.setValue(new_preset);
    fcitx_config_.targetLang.setValue(new_lang);
    if (const auto *preset = cfg.findPreset(new_preset)) {
        fcitx_config_.provider.setValue(toFcitxProvider(preset->provider));
        fcitx_config_.endpoint.setValue(preset->endpoint);
        fcitx_config_.apiKey.setValue(preset->api_key);
        fcitx_config_.model.setValue(preset->model);
    }
    config_mgr_.save();

    instance_->inputContextManager().foreach([&](fcitx::InputContext *other_ic) {
        if (other_ic == source_ic) {
            return true;
        }
        if (auto *other_state = other_ic->propertyFor(&factory_)) {
            other_state->session.syncSelectionDefaults(new_preset, new_lang);
        }
        return true;
    });
}

void ShadowBarAddon::reloadConfig() {
    config_mgr_.ensureAndLoad();
    syncFcitxConfigFromToml();

    // Only apply Fcitx5 INI file if it exists and is newer than config.toml,
    // so manual edits to config.toml are never overwritten on Fcitx5 restart.
    const auto fcitx_conf_path = userPkgConfigConfPath();
    std::error_code ec;
    if (std::filesystem::exists(fcitx_conf_path, ec)) {
        auto ini_time = std::filesystem::last_write_time(fcitx_conf_path, ec);
        auto toml_time = std::filesystem::last_write_time(config_mgr_.path(), ec);
        if (!ec && ini_time > toml_time) {
            fcitx::readAsIni(fcitx_config_, kFcitxPkgConfigType,
                             "conf/shadowbar.conf");
            applyFcitxConfigToToml();
        }
    }

    translator_.setCacheCapacity(config_mgr_.config().cache_size);
    translator_.reloadOfflineDictionaries(
        config_mgr_.config().effectiveOfflineDicts());
    syncAllSessionsConfig();
}

void ShadowBarAddon::save() {
    // Settings changed via setConfig() are persisted immediately.
    // On shutdown / Controller1.Save, sync manual edits from disk rather than
    // overwriting config.toml with stale in-memory state.
    hotReloadTomlIfNeeded();
}

void ShadowBarAddon::setConfig(const fcitx::RawConfig &config) {
    const bool prev_enabled = config_mgr_.config().floating_enabled;
    const bool prev_orb = config_mgr_.config().floating_show_orb;
    fcitx_config_.load(config, true);
    fcitx::safeSaveAsIni(fcitx_config_, kFcitxPkgConfigType,
                         "conf/shadowbar.conf");
    applyFcitxConfigToToml();
    translator_.clearCache();
    translator_.reloadOfflineDictionaries(
        config_mgr_.config().effectiveOfflineDicts());
    syncAllSessionsConfig();
    if (prev_enabled != config_mgr_.config().floating_enabled ||
        prev_orb != config_mgr_.config().floating_show_orb) {
        launchFloatingUi("--sync-orb");
    }
}

void ShadowBarAddon::hotReloadTomlIfNeeded() {
    const bool prev_enabled = config_mgr_.config().floating_enabled;
    const bool prev_orb = config_mgr_.config().floating_show_orb;
    if (config_mgr_.reloadIfModified()) {
        syncFcitxConfigFromToml();
        translator_.setCacheCapacity(config_mgr_.config().cache_size);
        translator_.reloadOfflineDictionary(
            config_mgr_.config().offline_dict_path);
        syncAllSessionsConfig();
        if (prev_enabled != config_mgr_.config().floating_enabled ||
            prev_orb != config_mgr_.config().floating_show_orb) {
            launchFloatingUi("--sync-orb");
        }
    }
}

bool ShadowBarAddon::isImeComposing(const fcitx::InputContext *ic) const {
    if (!ic) {
        return false;
    }
    const auto &panel = ic->inputPanel();
    if (!panel.preedit().empty() || !panel.clientPreedit().empty()) {
        return true;
    }
    auto cand = panel.candidateList();
    return cand && !cand->empty() &&
           !std::dynamic_pointer_cast<const ShadowBarCandidateList>(cand);
}

bool ShadowBarAddon::shouldShowBarForPanel(const fcitx::InputContext *ic,
                                           const ShadowBarICState *state) const {
    if (!ic || !state) {
        return false;
    }
    const ViewModel vm = state->session.viewModel();
    return vm.bar_visible || (vm.mode == Mode::Sticky && isImeComposing(ic));
}

bool ShadowBarAddon::isCjkInputMethod(fcitx::InputContext *ic) const {
    const auto *entry = instance_->inputMethodEntry(ic);
    if (!entry) {
        return false;
    }
    if (entry->isKeyboard()) {
        return false;
    }
    const std::string &lang = entry->languageCode();
    return lang.rfind("zh", 0) == 0 || lang.rfind("ja", 0) == 0 ||
           lang.rfind("ko", 0) == 0 || entry->uniqueName() == "pinyin" ||
           entry->uniqueName() == "rime" || entry->uniqueName() == "shuangpin" ||
           entry->uniqueName() == "wbx" || entry->uniqueName() == "shadowbar";
}

bool ShadowBarAddon::isStickyInputMethod(fcitx::InputContext *ic) const {
    if (!ic) {
        return false;
    }
    const auto *entry = instance_->inputMethodEntry(ic);
    return entry && entry->uniqueName() == "shadowbar";
}

bool ShadowBarAddon::isTranslationActive(fcitx::InputContext *ic) const {
    return isActionChecked(ic);
}

std::string ShadowBarAddon::findPureChineseImName() const {
    const auto &items =
        instance_->inputMethodManager().currentGroup().inputMethodList();
    static constexpr const char *kPreferred[] = {"pinyin", "shuangpin", "rime",
                                                 "wbx", "cangjie"};
    for (const char *candidate : kPreferred) {
        for (const auto &item : items) {
            if (item.name() == candidate) {
                return item.name();
            }
        }
    }
    for (const auto &item : items) {
        if (item.name() == "shadowbar") {
            continue;
        }
        const auto *entry = instance_->inputMethodManager().entry(item.name());
        if (entry && !entry->isKeyboard() &&
            entry->languageCode().rfind("zh", 0) == 0) {
            return item.name();
        }
    }
    return {};
}

void ShadowBarAddon::enterStickyMode(fcitx::InputContext *ic,
                                     ShadowBarICState *state,
                                     bool show_toast) {
    if (!ic || !state) {
        return;
    }
    state->im_user_paused = false;
    state->im_forced_sticky = isStickyInputMethod(ic);
    auto act = state->session.enterSticky(show_toast);
    handleSessionActions(ic, state, act);
    toggle_action_.update(ic);
}

void ShadowBarAddon::exitToPureChinese(fcitx::InputContext *ic,
                                       ShadowBarICState *state) {
    if (!ic || !state) {
        return;
    }
    state->im_forced_sticky = false;
    state->im_user_paused = false;
    auto act = state->session.exitToOff();
    handleSessionActions(ic, state, act);

    if (isStickyInputMethod(ic)) {
        const std::string pure_im = findPureChineseImName();
        if (!pure_im.empty()) {
            instance_->setCurrentInputMethod(ic, pure_im, false);
            showToast(instance_, ic,
                      "已切回纯中文 (" + pure_im + ") · ;; 或 Ctrl+Shift+T 可再翻译");
            return;
        }
        state->im_user_paused = true;
        toggle_action_.update(ic);
        showToast(instance_, ic, "已切回纯中文 · Ctrl+Shift+T 可再开翻译");
        return;
    }
    toggle_action_.update(ic);
    showToast(instance_, ic, "已关闭翻译 · 纯中文输入 (;; 或 Ctrl+Shift+T 可再开)");
}

bool ShadowBarAddon::isActionChecked(fcitx::InputContext *ic) const {
    if (!ic) {
        return false;
    }
    if (auto *state = const_cast<fcitx::InputContext *>(ic)->propertyFor(&factory_)) {
        return state->session.mode() != Mode::Off;
    }
    return false;
}

std::string ShadowBarAddon::actionShortText(fcitx::InputContext *ic) const {
    if (!ic) {
        return "开启 ShadowBar 翻译";
    }
    if (auto *state = const_cast<fcitx::InputContext *>(ic)->propertyFor(&factory_)) {
        if (state->session.mode() != Mode::Off) {
            return "ShadowBar 翻译: 开 (切回纯中文)";
        }
    }
    return "开启 ShadowBar 翻译";
}

void ShadowBarAddon::toggleStickyFromAction(fcitx::InputContext *ic) {
    if (!ic || isIgnoredContext(ic)) {
        return;
    }
    if (auto *state = ic->propertyFor(&factory_)) {
        if (state->session.mode() != Mode::Off) {
            exitToPureChinese(ic, state);
        } else {
            enterStickyMode(ic, state, true);
        }
    }
}

void ShadowBarAddon::attachStatusAction(fcitx::InputContext *ic) {
    if (!ic) {
        return;
    }
    const auto existing =
        ic->statusArea().actions(fcitx::StatusGroup::AfterInputMethod);
    if (std::find(existing.begin(), existing.end(), &toggle_action_) ==
        existing.end()) {
        ic->statusArea().addAction(fcitx::StatusGroup::AfterInputMethod,
                                   &toggle_action_);
    }
    toggle_action_.update(ic);
}

void ShadowBarAddon::setupStatusMenu() {
    auto &ui = instance_->userInterfaceManager();

    menu_toggle_sticky_action_.setShortText("伴随翻译模式");
    menu_toggle_sticky_action_.setIcon("accessories-dictionary");
    ui.registerAction("shadowbar-menu-sticky", &menu_toggle_sticky_action_);
    menu_conns_.emplace_back(
        menu_toggle_sticky_action_.connect<fcitx::SimpleAction::Activated>(
            [this](fcitx::InputContext *ic) {
                if (!ic) {
                    ic = instance_->mostRecentInputContext();
                }
                toggleStickyFromAction(ic);
            }));

    menu_floating_action_.setShortText("打开翻译悬浮窗");
    menu_floating_action_.setIcon("edit-find");
    ui.registerAction("shadowbar-menu-floating", &menu_floating_action_);
    menu_conns_.emplace_back(
        menu_floating_action_.connect<fcitx::SimpleAction::Activated>(
            [this](fcitx::InputContext *) { launchFloatingUi("--toggle"); }));

    menu_orb_action_.setShortText("显示桌面悬浮球");
    menu_orb_action_.setIcon("view-restore");
    menu_orb_action_.setCheckable(true);
    ui.registerAction("shadowbar-menu-orb", &menu_orb_action_);
    menu_conns_.emplace_back(
        menu_orb_action_.connect<fcitx::SimpleAction::Activated>(
            [this](fcitx::InputContext *) { toggleOrbFromMenu(); }));

    menu_reset_floating_action_.setShortText("重置悬浮窗位置与大小");
    menu_reset_floating_action_.setIcon("zoom-fit-best");
    ui.registerAction("shadowbar-menu-reset-floating",
                      &menu_reset_floating_action_);
    menu_conns_.emplace_back(
        menu_reset_floating_action_.connect<fcitx::SimpleAction::Activated>(
            [this](fcitx::InputContext *) { launchFloatingUi("--reset"); }));

    menu_settings_action_.setShortText("ShadowBar 设置与 API 管理...");
    menu_settings_action_.setIcon("configure");
    ui.registerAction("shadowbar-menu-settings", &menu_settings_action_);
    menu_conns_.emplace_back(
        menu_settings_action_.connect<fcitx::SimpleAction::Activated>(
            [this](fcitx::InputContext *) { launchFloatingUi("--settings"); }));

    status_menu_.addAction(&menu_toggle_sticky_action_);
    status_menu_.addAction(&menu_floating_action_);
    status_menu_.addAction(&menu_orb_action_);
    status_menu_.addAction(&menu_reset_floating_action_);
    status_menu_.addAction(&menu_settings_action_);
    toggle_action_.setMenu(&status_menu_);
    updateStatusMenuState();
}

void ShadowBarAddon::updateStatusMenuState() {
    const auto &cfg = config_mgr_.config();
    setActionHotkey(menu_toggle_sticky_action_,
                    fcitx_config_.toggleHotkey.value());
    setActionHotkey(menu_floating_action_,
                    fcitx_config_.floatingHotkey.value());
    menu_orb_action_.setChecked(cfg.floating_enabled && cfg.floating_show_orb);
    menu_floating_action_.setShortText(
        cfg.floating_enabled ? "打开翻译悬浮窗" : "打开翻译悬浮窗 (已禁用)");
    if (auto *ic = instance_->mostRecentInputContext()) {
        menu_orb_action_.update(ic);
        menu_floating_action_.update(ic);
        menu_reset_floating_action_.update(ic);
    }
}

void ShadowBarAddon::launchFloatingUi(const std::string &arg) const {
    if (const char *no_spawn = std::getenv("SHADOWBAR_NO_SPAWN_UI");
        no_spawn && *no_spawn) {
        return;
    }
    if (!hasGuiDisplayEnv()) {
        return;
    }
    fcitx::startProcess({resolveFloatingBinary(), arg});
}

void ShadowBarAddon::toggleOrbFromMenu() {
    auto &cfg = config_mgr_.mutableConfig();
    if (!cfg.floating_enabled) {
        cfg.floating_enabled = true;
    }
    cfg.floating_show_orb = !cfg.floating_show_orb;
    config_mgr_.save();
    syncFcitxConfigFromToml();
    launchFloatingUi("--sync-orb");
}

KeyInput ShadowBarAddon::toCoreKey(const fcitx::KeyEvent &event) const {
    KeyInput k;
    const fcitx::Key key = event.key();
    const auto states = key.states();
    k.shift = states.test(fcitx::KeyState::Shift) ||
              (key.sym() == FcitxKey_ISO_Left_Tab);
    k.ctrl = states.test(fcitx::KeyState::Ctrl);
    k.alt = states.test(fcitx::KeyState::Alt);
    k.super = states.test(fcitx::KeyState::Super);
    k.is_release = event.isRelease();

    switch (key.sym()) {
    case FcitxKey_Return:
    case FcitxKey_KP_Enter:
        k.special = SpecialKey::Enter;
        break;
    case FcitxKey_BackSpace:
        k.special = SpecialKey::Backspace;
        break;
    case FcitxKey_Escape:
        k.special = SpecialKey::Escape;
        break;
    case FcitxKey_Tab:
    case FcitxKey_ISO_Left_Tab:
        k.special = SpecialKey::Tab;
        break;
    case FcitxKey_Left:
    case FcitxKey_KP_Left:
        k.special = SpecialKey::Left;
        break;
    case FcitxKey_Right:
    case FcitxKey_KP_Right:
        k.special = SpecialKey::Right;
        break;
    case FcitxKey_Up:
    case FcitxKey_KP_Up:
        k.special = SpecialKey::Up;
        break;
    case FcitxKey_Down:
    case FcitxKey_KP_Down:
        k.special = SpecialKey::Down;
        break;
    case FcitxKey_Home:
    case FcitxKey_KP_Home:
        k.special = SpecialKey::Home;
        break;
    case FcitxKey_End:
    case FcitxKey_KP_End:
        k.special = SpecialKey::End;
        break;
    case FcitxKey_Delete:
    case FcitxKey_KP_Delete:
        k.special = SpecialKey::Delete;
        break;
    case FcitxKey_space:
        k.special = SpecialKey::Space;
        k.utf8_char = " ";
        break;
    default:
        k.special = SpecialKey::None;
        if (!k.hasCmdModifier()) {
            k.utf8_char = fcitx::Key::keySymToUTF8(key.sym());
            // Strip ASCII control characters from utf8_char.
            if (k.utf8_char.size() == 1 &&
                static_cast<unsigned char>(k.utf8_char[0]) < 0x20) {
                k.utf8_char.clear();
            }
        }
        break;
    }
    return k;
}

void ShadowBarAddon::schedulePrefixTimer(fcitx::InputContext *ic,
                                           ShadowBarICState *state) {
    if (!state) {
        return;
    }
    const uint64_t timeout_usec =
        static_cast<uint64_t>(std::max(80, config_mgr_.config().prefix_timeout_ms)) *
        1000ULL;
    auto ic_ref = ic->watch();
    state->prefix_timer = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + timeout_usec, 0,
        [this, ic_ref](fcitx::EventSourceTime *, uint64_t) {
            auto *target_ic = ic_ref.get();
            if (!target_ic) {
                return false;
            }
            auto *ic_state = target_ic->propertyFor(&factory_);
            if (!ic_state) {
                return false;
            }
            auto act = ic_state->session.flushPendingPrefixIfExpired(
                SteadyClock::now());
            if (!act.replay_raw_prefix.empty()) {
                replayRawPrefix(target_ic, act.replay_raw_prefix);
            }
            ic_state->prefix_timer.reset();
            return false;
        });
}

void ShadowBarAddon::replayRawPrefix(fcitx::InputContext *ic,
                                     const std::string &raw) {
    if (!ic || raw.empty()) {
        return;
    }
    replaying_key_ = true;
    struct Guard {
        bool &flag;
        ~Guard() { flag = false; }
    } guard{replaying_key_};

    size_t pos = 0;
    while (pos < raw.size()) {
        size_t start = pos;
        size_t next = pos;
        uint32_t cp = utf8::decodeAt(raw, pos, &next);
        pos = next;
        fcitx::KeySym sym = fcitx::Key::keySymFromUnicode(cp);
        if (sym != FcitxKey_None) {
            fcitx::KeyEvent kev(ic, fcitx::Key(sym));
            if (!ic->keyEvent(kev)) {
                commitDirectToApp(ic, fcitx::utf8::UCS4ToUTF8(cp));
            }
        } else {
            commitDirectToApp(ic, raw.substr(start, next - start));
        }
    }
}

void ShadowBarAddon::commitDirectToApp(fcitx::InputContext *ic,
                                       const std::string &text) {
    if (!ic || text.empty()) {
        return;
    }
    bypass_commit_filter_ = true;
    struct Guard {
        bool &flag;
        ~Guard() { flag = false; }
    } guard{bypass_commit_filter_};
    ic->commitString(text);
}

void ShadowBarAddon::executeCandidateAction(fcitx::InputContext *ic,
                                            CandidateAction action) {
    if (!ic || isIgnoredContext(ic)) {
        return;
    }
    hotReloadTomlIfNeeded();
    auto *state = ic->propertyFor(&factory_);
    if (!state) {
        return;
    }
    switch (action) {
    case CandidateAction::CommitTranslation: {
        auto act = state->session.commitTranslation();
        handleSessionActions(ic, state, act);
        break;
    }
    case CandidateAction::CommitOriginal: {
        auto act = state->session.commitOriginal();
        handleSessionActions(ic, state, act);
        break;
    }
    case CandidateAction::Retranslate: {
        auto act = state->session.retryTranslation();
        handleSessionActions(ic, state, act);
        break;
    }
    case CandidateAction::CyclePreset: {
        auto act = state->session.cyclePreset(false);
        persistSelectionDefaults(ic, state);
        handleSessionActions(ic, state, act);
        break;
    }
    case CandidateAction::CycleLang: {
        auto act = state->session.cycleTargetLang();
        persistSelectionDefaults(ic, state);
        handleSessionActions(ic, state, act);
        break;
    }
    case CandidateAction::ExitPureChinese:
        exitToPureChinese(ic, state);
        break;
    }
}

void ShadowBarAddon::renderPanel(fcitx::InputContext *ic,
                                 ShadowBarICState *state,
                                 bool force_clear,
                                 bool notify_ui) {
    if (!ic || !state || rendering_panel_) {
        return;
    }
    rendering_panel_ = true;
    struct Guard {
        bool &flag;
        ~Guard() { flag = false; }
    } guard{rendering_panel_};

    const ViewModel vm = state->session.viewModel();
    auto &panel = ic->inputPanel();

    if (!shouldShowBarForPanel(ic, state)) {
        state->transient_expand = false;
        if (state->was_bar_visible || force_clear) {
            panel.setAuxUp(fcitx::Text());
            panel.setAuxDown(fcitx::Text());
            clearShadowBarCandidateList(panel);
            if (notify_ui) {
                ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
            }
            state->was_bar_visible = false;
        }
        return;
    }

    state->was_bar_visible = true;
    if (vm.source_text.empty() && state->last_panel_revision != state->session.revision()) {
        state->transient_expand = false;
    }

    const auto badge_flags =
        fcitx::TextFormatFlags{fcitx::TextFormatFlag::HighLight} |
        fcitx::TextFormatFlag::Bold;

    auto append_header = [&](fcitx::Text &target, bool with_status) {
        target.append(" " + vm.badge + " ", badge_flags);
        if (config_mgr_.config().show_source_line) {
            target.append(" ");
            if (vm.source_text.empty()) {
                target.append("请输入中文或英文…");
            } else {
                appendSourceWithCursor(target, vm.source_text, vm.cursor_byte);
            }
        }
        if (with_status) {
            if (vm.status == TranslationStatus::Pending ||
                vm.status == TranslationStatus::Streaming) {
                target.append("  ···");
            } else if (vm.status == TranslationStatus::FallbackOffline) {
                target.append("  · 离线词典");
            } else if (vm.status == TranslationStatus::Error &&
                       !vm.status_hint.empty()) {
                target.append("  · " + vm.status_hint);
            }
        }
    };

    const bool has_translation = !utf8::trimView(vm.translation_text).empty();
    const bool ime_busy = isImeComposing(ic);

    if (ime_busy) {
        // Keep Pinyin's candidates intact; show a compact live header above them.
        fcitx::Text up;
        if (!panel.preedit().empty()) {
            if (has_translation) {
                up.append("✦ " + vm.translation_text + "\n",
                          fcitx::TextFormatFlag::Bold);
            }
            append_header(up, false);
            up.append(" ");
        } else {
            append_header(up, true);
            if (has_translation) {
                up.append("\n");
                up.append("✦ " + vm.translation_text,
                          fcitx::TextFormatFlag::Bold);
            }
        }
        panel.setAuxUp(up);
        panel.setAuxDown(fcitx::Text());
    } else if (!config_mgr_.config().expand_actions && !state->transient_expand) {
        // Compact default view: 1-2 crisp lines without expanding full action card.
        fcitx::Text up;
        append_header(up, true);
        fcitx::Text down;
        if (!vm.source_text.empty()) {
            if (has_translation) {
                down.append("✦ " + vm.translation_text, fcitx::TextFormatFlag::Bold);
            } else if (vm.status == TranslationStatus::Error) {
                down.append("✦ 翻译失败 · Enter 再次翻译 / Shift+Enter 发原文",
                            fcitx::TextFormatFlag::Bold);
            } else {
                down.append("✦ 翻译中…", fcitx::TextFormatFlag::Bold);
            }
        } else {
            up.append("  · ↓ 展开操作");
        }
        clearShadowBarCandidateList(panel);
        panel.setAuxUp(up);
        panel.setAuxDown(down);
        state->last_panel_revision = state->session.revision();
    } else {
        // Expanded view (expand_actions = true or user tapped Down): sleek vertical card.
        fcitx::Text up;
        append_header(up, true);
        panel.setAuxUp(up);
        panel.setAuxDown(fcitx::Text());

        int preserved_cursor = -1;
        if (auto prev_sbc = std::dynamic_pointer_cast<const ShadowBarCandidateList>(
                panel.candidateList())) {
            if (state->last_panel_revision == state->session.revision()) {
                preserved_cursor = prev_sbc->globalCursorIndex();
            }
        }

        auto list = std::make_unique<ShadowBarCandidateList>();
        std::vector<std::string> row_labels;
        auto add_row = [&](CandidateAction action, std::string label,
                           fcitx::Text text, fcitx::Text comment = {}) {
            row_labels.push_back(label);
            list->append<ShadowBarCandidateWord>(
                this, action, std::move(label), std::move(text),
                std::move(comment));
        };

        if (!vm.source_text.empty()) {
            if (!has_translation && vm.status == TranslationStatus::Error) {
                add_row(CandidateAction::Retranslate, "↻",
                        fcitx::Text("再次翻译 (上次请求失败)"),
                        fcitx::Text("Enter 重试 / Shift+Enter 发原文"));
            } else {
                fcitx::Text trans_row_text;
                fcitx::Text trans_comment;
                if (has_translation) {
                    trans_row_text.append(vm.translation_text);
                    trans_comment.append("上屏译文 · Enter");
                } else {
                    trans_row_text.append("正在翻译…");
                }
                add_row(CandidateAction::CommitTranslation, "↵",
                        std::move(trans_row_text), std::move(trans_comment));
            }

            add_row(CandidateAction::CommitOriginal, "⇧",
                    fcitx::Text(vm.source_text),
                    fcitx::Text("直出原文 · Shift+Enter"));

            add_row(CandidateAction::CyclePreset, "⇥",
                    fcitx::Text("风格: " + vm.active_preset_label),
                    fcitx::Text("Tab / Shift+Tab"));

            add_row(CandidateAction::CycleLang, "⇄",
                    fcitx::Text("方向: " + vm.direction_label),
                    fcitx::Text("Alt+L"));

            add_row(CandidateAction::ExitPureChinese, "✕",
                    fcitx::Text("退出翻译 / 切回纯中文"), fcitx::Text("Esc"));
        } else {
            add_row(CandidateAction::CyclePreset, "⇥",
                    fcitx::Text("风格: " + vm.active_preset_label),
                    fcitx::Text("Tab"));

            add_row(CandidateAction::CycleLang, "⇄",
                    fcitx::Text("方向: " + vm.direction_label),
                    fcitx::Text("Alt+L"));

            add_row(CandidateAction::ExitPureChinese, "✕",
                    fcitx::Text("取消 · 切回纯中文"), fcitx::Text("Esc / Space"));
        }

        list->setLabels(row_labels);

        const int total = list->totalSize();
        if (preserved_cursor >= 0 && preserved_cursor < total) {
            list->setGlobalCursorIndex(preserved_cursor);
        } else {
            const bool keep_unselected =
                vm.source_text.empty() && !state->transient_expand;
            list->setGlobalCursorIndex(keep_unselected ? -1 : 0);
        }
        state->last_panel_revision = state->session.revision();
        panel.setCandidateList(std::move(list));
    }

    if (notify_ui) {
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    }
}

void ShadowBarAddon::handleSessionActions(fcitx::InputContext *ic,
                                          ShadowBarICState *state,
                                          const SessionActions &actions) {
    if (!ic || !state) {
        return;
    }
    if (!actions.replay_raw_prefix.empty()) {
        state->prefix_timer.reset();
        replayRawPrefix(ic, actions.replay_raw_prefix);
    }
    if (state->session.hasPendingPrefix()) {
        schedulePrefixTimer(ic, state);
    } else {
        state->prefix_timer.reset();
    }

    if (actions.cancel_translation) {
        translator_.cancelAll();
    }
    if (!actions.commit_to_app.empty()) {
        commitDirectToApp(ic, actions.commit_to_app);
    }
    if (!actions.toast_message.empty()) {
        showToast(instance_, ic, actions.toast_message);
    }
    if (actions.trigger_translation) {
        const auto &cfg = config_mgr_.config();
        const auto vm = state->session.viewModel();
        Preset active_preset = state->session.activePreset();
        const int effective_timeout_ms =
            (active_preset.provider == ProviderKind::OpenAICompatible ||
             active_preset.provider == ProviderKind::Ollama)
                ? std::max(cfg.request_timeout_ms, 10000)
                : cfg.request_timeout_ms;
        TranslationJob job{
            .revision = state->session.revision(),
            .source_text = vm.source_text,
            .source_lang = vm.effective_source_lang.empty()
                               ? cfg.source_lang
                               : vm.effective_source_lang,
            .preset = std::move(active_preset),
            .immediate = actions.immediate_translation,
            .lock_on_punctuation = cfg.lock_on_punctuation,
            .offline_fallback = cfg.offline_dict_fallback,
            .bypass_cache = actions.bypass_cache_translation,
            .debounce_ms = cfg.debounce_ms,
            .timeout_ms = effective_timeout_ms,
        };

        auto ic_ref = ic->watch();
        translator_.submit(
            std::move(job),
            [this, ic_ref](uint64_t rev, const TranslationResult &res) {
                auto apply_update = [this, ic_ref, rev, res]() {
                    auto *target_ic = ic_ref.get();
                    if (!target_ic) {
                        return;
                    }
                    auto *ic_state = target_ic->propertyFor(&factory_);
                    if (!ic_state) {
                        return;
                    }
                    if (ic_state->session.onTranslationUpdate(rev, res)) {
                        renderPanel(target_ic, ic_state, false);
                    }
                };

                if (std::this_thread::get_id() == main_thread_id_) {
                    apply_update();
                } else {
                    instance_->eventDispatcher().schedule(
                        [ic_ref, apply_update = std::move(apply_update)]() {
                            if (ic_ref.isValid()) {
                                apply_update();
                            }
                        });
                }
            });
    }
    if (actions.update_ui) {
        renderPanel(ic, state, !shouldShowBarForPanel(ic, state));
        toggle_action_.update(ic);
    }
}

void ShadowBarAddon::onKeyEventPre(fcitx::KeyEvent &event) {
    if (event.isRelease() || replaying_key_) {
        return;
    }
    auto *ic = event.inputContext();
    if (isIgnoredContext(ic)) {
        return;
    }
    hotReloadTomlIfNeeded();

    auto *state = ic->propertyFor(&factory_);
    if (!state) {
        return;
    }

    const auto toggle_keys = fcitx_config_.toggleHotkey.value();
    if (!toggle_keys.empty() && event.key().checkKeyList(toggle_keys)) {
        if (state->session.mode() != Mode::Off) {
            exitToPureChinese(ic, state);
        } else {
            enterStickyMode(ic, state, true);
        }
        event.filterAndAccept();
        return;
    }

    const auto oneshot_keys = fcitx_config_.oneshotHotkey.value();
    if (!oneshot_keys.empty() && event.key().checkKeyList(oneshot_keys)) {
        auto act = state->session.enterOneShot();
        handleSessionActions(ic, state, act);
        event.filterAndAccept();
        return;
    }

    const auto lang_keys = fcitx_config_.cycleLangHotkey.value();
    if (!lang_keys.empty() && event.key().checkKeyList(lang_keys)) {
        auto act = state->session.cycleTargetLang();
        persistSelectionDefaults(ic, state);
        handleSessionActions(ic, state, act);
        event.filterAndAccept();
        return;
    }

    const auto preset_keys = fcitx_config_.cyclePresetHotkey.value();
    if (!preset_keys.empty() && event.key().checkKeyList(preset_keys)) {
        auto act = state->session.cyclePreset(false);
        persistSelectionDefaults(ic, state);
        handleSessionActions(ic, state, act);
        event.filterAndAccept();
        return;
    }

    const auto floating_keys = fcitx_config_.floatingHotkey.value();
    if (config_mgr_.config().floating_enabled && !floating_keys.empty() &&
        event.key().checkKeyList(floating_keys)) {
        launchFloatingUi("--toggle");
        event.filterAndAccept();
        return;
    }

    const auto now = SteadyClock::now();
    if (state->session.hasPendingPrefix()) {
        auto flush_act = state->session.flushPendingPrefixIfExpired(now);
        if (!flush_act.replay_raw_prefix.empty()) {
            handleSessionActions(ic, state, flush_act);
        }
    }

    const bool ime_composing = isImeComposing(ic);
    const bool in_cjk = isCjkInputMethod(ic);

    // Let underlying Pinyin/Rime handle composing keys, and let printable letters flow
    // through Pinyin/Rime in PreInputMethod (non-filtered fallbacks are caught in PostInputMethod).
    if (!state->session.isActive()) {
        auto act = state->session.onKey(toCoreKey(event), ime_composing, in_cjk, now);
        handleSessionActions(ic, state, act);
        if (act.consume_key) {
            event.filterAndAccept();
        }
        return;
    }

    if (ime_composing) {
        return;
    }

    const KeyInput key = toCoreKey(event);
    const fcitx::KeySym raw_sym = event.key().sym();
    if (key.alt && !key.ctrl && !key.super && !key.shift &&
        (raw_sym == FcitxKey_l || raw_sym == FcitxKey_L)) {
        auto act = state->session.cycleTargetLang();
        persistSelectionDefaults(ic, state);
        handleSessionActions(ic, state, act);
        event.filterAndAccept();
        return;
    }
    if (key.ctrl && !key.alt && !key.super && !key.shift &&
        (raw_sym == FcitxKey_u || raw_sym == FcitxKey_U) &&
        !state->session.viewModel().source_text.empty()) {
        auto act = state->session.clearDraft();
        handleSessionActions(ic, state, act);
        event.filterAndAccept();
        return;
    }

    if ((key.special == SpecialKey::Up || key.special == SpecialKey::Down) &&
        !key.hasCmdModifier()) {
        if (auto sbc = std::dynamic_pointer_cast<ShadowBarCandidateList>(
                ic->inputPanel().candidateList())) {
            if (!sbc->empty()) {
                if (key.special == SpecialKey::Up) {
                    if (!config_mgr_.config().expand_actions &&
                        state->transient_expand && sbc->cursorIndex() <= 0) {
                        state->transient_expand = false;
                        renderPanel(ic, state, false);
                        event.filterAndAccept();
                        return;
                    }
                    sbc->prevCandidate();
                } else {
                    sbc->nextCandidate();
                }
                ic->updateUserInterface(
                    fcitx::UserInterfaceComponent::InputPanel);
                event.filterAndAccept();
                return;
            }
        } else if (key.special == SpecialKey::Down &&
                   shouldShowBarForPanel(ic, state) &&
                   !config_mgr_.config().expand_actions &&
                   !state->transient_expand) {
            state->transient_expand = true;
            renderPanel(ic, state, false);
            event.filterAndAccept();
            return;
        }
    }

    if (key.special == SpecialKey::Enter && !key.shift && !key.hasCmdModifier()) {
        if (auto sbc = std::dynamic_pointer_cast<ShadowBarCandidateList>(
                ic->inputPanel().candidateList())) {
            const int idx = sbc->cursorIndex();
            if (idx >= 0 && idx < sbc->size()) {
                sbc->candidate(idx).select(ic);
                event.filterAndAccept();
                return;
            }
        }
    }

    // In PreInputMethod, intercept navigation/control keys so Pinyin/QuickPhrase doesn't swallow them.
    if (key.special != SpecialKey::None && key.special != SpecialKey::Space) {
        auto act = state->session.onKey(key, false, in_cjk, now);
        if (key.special == SpecialKey::Tab && act.consume_key) {
            persistSelectionDefaults(ic, state);
        }
        handleSessionActions(ic, state, act);
        if (act.consume_key) {
            event.filterAndAccept();
        }
        return;
    }

    // Space when source buffer is non-empty or cancelling empty OneShot.
    if (key.special == SpecialKey::Space) {
        auto act = state->session.onKey(key, false, in_cjk, now);
        handleSessionActions(ic, state, act);
        if (act.consume_key) {
            event.filterAndAccept();
        }
    }
}

void ShadowBarAddon::onKeyEventPost(fcitx::KeyEvent &event) {
    if (event.isRelease() || event.filtered() || replaying_key_) {
        return;
    }
    auto *ic = event.inputContext();
    if (isIgnoredContext(ic)) {
        return;
    }
    auto *state = ic->propertyFor(&factory_);
    if (!state || !state->session.isActive()) {
        return;
    }

    // Only capture printable characters that the underlying IME left unfiltered
    // (e.g. English words like "API", digits, or when user temporarily toggled Shift-English).
    const KeyInput key = toCoreKey(event);
    if (key.special == SpecialKey::None && !key.hasCmdModifier() &&
        !key.utf8_char.empty()) {
        auto act = state->session.onKey(key, false, isCjkInputMethod(ic),
                                        SteadyClock::now());
        handleSessionActions(ic, state, act);
        if (act.consume_key) {
            event.filterAndAccept();
        }
    }
}

void ShadowBarAddon::onCommitFilter(fcitx::InputContext *ic, std::string &orig) {
    if (bypass_commit_filter_ || !ic || orig.empty() || isIgnoredContext(ic)) {
        return;
    }
    auto *state = ic->propertyFor(&factory_);
    if (!state || !state->session.isActive()) {
        return;
    }

    const std::string committed = orig;
    orig.clear(); // Prevent raw Chinese from reaching the target app prematurely.

    auto act = state->session.onCommitFromIme(committed, SteadyClock::now());
    handleSessionActions(ic, state, act);
}

void ShadowBarAddon::onUpdateUI(fcitx::InputContextUpdateUIEvent &event) {
    if (rendering_panel_ ||
        event.component() != fcitx::UserInterfaceComponent::InputPanel) {
        return;
    }
    auto *ic = event.inputContext();
    if (isIgnoredContext(ic)) {
        return;
    }
    auto *state = ic->propertyFor(&factory_);
    const bool show = shouldShowBarForPanel(ic, state);
    if (!state || (!show && !state->was_bar_visible)) {
        return;
    }
    // Re-inject ShadowBar lines after Pinyin resets/repopulates InputPanel,
    // or clear Sticky composing hint when Pinyin aborts.
    renderPanel(ic, state, false, false);
}

void ShadowBarAddon::onFocusOutOrReset(fcitx::InputContext *ic) {
    if (!ic) {
        return;
    }
    auto *state = ic->propertyFor(&factory_);
    if (!state) {
        return;
    }
    state->prefix_timer.reset();
    if (state->session.hasPendingPrefix()) {
        auto act = state->session.flushPendingPrefixIfExpired(
            SteadyTimePoint::max());
        (void)act;
    }
    if (state->session.mode() == Mode::OneShot &&
        state->session.viewModel().source_text.empty()) {
        auto act = state->session.exitToOff();
        handleSessionActions(ic, state, act);
    }
}

fcitx::AddonInstance *ShadowBarAddonFactory::create(fcitx::AddonManager *manager) {
    return new ShadowBarAddon(manager->instance());
}

} // namespace shadowbar::fcitx_addon

#ifdef FCITX_ADDON_FACTORY_V2_BACKWARDS
FCITX_ADDON_FACTORY_V2_BACKWARDS(shadowbar, shadowbar::fcitx_addon::ShadowBarAddonFactory)
#else
FCITX_ADDON_FACTORY(shadowbar::fcitx_addon::ShadowBarAddonFactory)
#endif
