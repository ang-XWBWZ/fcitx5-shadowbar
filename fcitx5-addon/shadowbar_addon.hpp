#pragma once

#include "shadowbar/config.hpp"
#include "shadowbar/session.hpp"
#include "shadowbar/translator.hpp"
#include "shadowbar_addon_public.h"

#include <fcitx-config/configuration.h>
#include <fcitx-config/enum.h>
#include <fcitx-config/option.h>
#if defined(__has_include) && __has_include(<fcitx-utils/eventloopinterface.h>)
#include <fcitx-utils/eventloopinterface.h>
#else
#include <fcitx-utils/event.h>
#endif
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/signals.h>
#include <fcitx/action.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/event.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/instance.h>
#include <fcitx/menu.h>
#include <fcitx/statusarea.h>
#include <fcitx/userinterfacemanager.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace shadowbar::fcitx_addon {

FCITX_CONFIG_ENUM(ShadowProviderOption, Google, OpenAI, Ollama, DeepL, Offline);

FCITX_CONFIGURATION(
    ShadowBarFcitxConfig,
    fcitx::KeyListOption toggleHotkey{
        this,
        "ToggleHotkey",
        "切换翻译开关快捷键 (默认译英文)",
        {fcitx::Key("Control+Shift+T")},
        fcitx::KeyListConstrain(fcitx::KeyConstrainFlag::AllowModifierLess)};
    fcitx::KeyListOption cycleLangHotkey{
        this,
        "CycleLangHotkey",
        "切换目标语言快捷键 (全局记忆)",
        {fcitx::Key("Alt+Shift+L")},
        fcitx::KeyListConstrain(fcitx::KeyConstrainFlag::AllowModifierLess)};
    fcitx::KeyListOption cyclePresetHotkey{
        this,
        "CyclePresetHotkey",
        "切换翻译配置/预设快捷键 (全局记忆)",
        {fcitx::Key("Alt+Shift+P")},
        fcitx::KeyListConstrain(fcitx::KeyConstrainFlag::AllowModifierLess)};
    fcitx::KeyListOption oneshotHotkey{
        this,
        "OneShotHotkey",
        "单句快译快捷键 (可选)",
        {},
        fcitx::KeyListConstrain(fcitx::KeyConstrainFlag::AllowModifierLess)};
    fcitx::Option<bool> enablePrefix{
        this, "EnablePrefix", "启用引导前缀触发 (如 ;;)", true};
    fcitx::Option<std::string> prefix{
        this, "Prefix", "引导前缀字符串", ";;"};
    fcitx::Option<bool> prefixOnlyInCjkIm{
        this, "PrefixOnlyInCjkIM", "仅在中文输入法下监听前缀 (避免写代码误触)", true};
    fcitx::Option<int, fcitx::IntConstrain> debounceMs{
        this, "DebounceMs", "防抖延迟 (毫秒)", 260, fcitx::IntConstrain(0, 2000)};
    fcitx::Option<std::string> targetLang{
        this, "TargetLang", "默认目标语言 (en/zh/ja)", "en"};
    fcitx::Option<std::string> activePreset{
        this, "ActivePreset", "默认译文预设 (quick/natural/work)", "quick"};
    fcitx::Option<bool> expandActions{
        this, "ExpandActions", "始终展开悬浮操作菜单 (否则精简展示，按 ↓ 可临时展开)", false};
    fcitx::Option<ShadowProviderOption> provider{
        this, "Provider", "当前预设引擎类型", ShadowProviderOption::Google};
    fcitx::Option<std::string> endpoint{
        this, "Endpoint", "API 地址 (OpenAI 兼容 / DeepL)", ""};
    fcitx::Option<std::string> apiKey{
        this, "ApiKey", "API Key (支持 env:OPENAI_API_KEY)", ""};
    fcitx::Option<std::string> model{
        this, "Model", "模型名称 (如 gpt-4o-mini / deepseek-chat)", ""};
    fcitx::Option<bool> floatingEnabled{
        this, "FloatingEnabled", "启用独立翻译悬浮窗与快捷键", true};
    fcitx::KeyListOption floatingHotkey{
        this,
        "FloatingHotkey",
        "呼出/隐藏翻译悬浮窗快捷键",
        {fcitx::Key("Alt+Shift+Q")},
        fcitx::KeyListConstrain(fcitx::KeyConstrainFlag::AllowModifierLess)};
    fcitx::Option<bool> floatingAutoReadClipboard{
        this, "FloatingAutoReadClipboard", "打开悬浮窗时默认读取剪贴板", true};
    fcitx::Option<bool> floatingShowOrb{
        this, "FloatingShowOrb", "显示桌面迷你悬浮球 (点击展开悬浮窗)", false};);

struct ShadowBarICState : public fcitx::InputContextProperty {
    explicit ShadowBarICState(ShadowConfig cfg) : session(std::move(cfg)) {}

    ShadowSession session;
    std::unique_ptr<fcitx::EventSourceTime> prefix_timer;
    uint64_t last_panel_revision = 0;
    bool was_bar_visible = false;
    bool transient_expand = false;
    bool im_forced_sticky = false;
    bool im_user_paused = false;
};

class ShadowBarAddon;

class ShadowBarToggleAction final : public fcitx::Action {
public:
    explicit ShadowBarToggleAction(ShadowBarAddon *addon);

    std::string shortText(fcitx::InputContext *ic) const override;
    std::string longText(fcitx::InputContext *ic) const override;
    std::string icon(fcitx::InputContext *ic) const override;
    bool isChecked(fcitx::InputContext *ic) const override;
    void activate(fcitx::InputContext *ic) override;

private:
    ShadowBarAddon *addon_;
};

class ShadowBarAddon final : public fcitx::AddonInstance {
public:
    enum class CandidateAction {
        CommitTranslation,
        CommitOriginal,
        Retranslate,
        CyclePreset,
        CycleLang,
        ExitPureChinese,
    };

    explicit ShadowBarAddon(fcitx::Instance *instance);
    ~ShadowBarAddon() override;

    void reloadConfig() override;
    void save() override;
    const fcitx::Configuration *getConfig() const override { return &fcitx_config_; }
    void setConfig(const fcitx::RawConfig &config) override;

    void executeCandidateAction(fcitx::InputContext *ic, CandidateAction action);

    bool isTranslationActive(fcitx::InputContext *ic) const;
    FCITX_ADDON_EXPORT_FUNCTION(ShadowBarAddon, isTranslationActive);

private:
    void syncFcitxConfigFromToml();
    void applyFcitxConfigToToml();
    void hotReloadTomlIfNeeded();
    void syncAllSessionsConfig();
    void persistSelectionDefaults(fcitx::InputContext *source_ic,
                                  ShadowBarICState *source_state);

    void onKeyEventPre(fcitx::KeyEvent &event);
    void onKeyEventPost(fcitx::KeyEvent &event);
    void onCommitFilter(fcitx::InputContext *ic, std::string &orig);
    void onUpdateUI(fcitx::InputContextUpdateUIEvent &event);
    void onFocusOutOrReset(fcitx::InputContext *ic);

    void handleSessionActions(fcitx::InputContext *ic,
                              ShadowBarICState *state,
                              const SessionActions &actions);
    void schedulePrefixTimer(fcitx::InputContext *ic, ShadowBarICState *state);
    void replayRawPrefix(fcitx::InputContext *ic, const std::string &raw);
    void commitDirectToApp(fcitx::InputContext *ic, const std::string &text);
    void renderPanel(fcitx::InputContext *ic,
                     ShadowBarICState *state,
                     bool force_clear,
                     bool notify_ui = true);

    bool isImeComposing(fcitx::InputContext *ic) const;
    bool shouldShowBarForPanel(fcitx::InputContext *ic,
                               const ShadowBarICState *state) const;
    bool isCjkInputMethod(fcitx::InputContext *ic) const;
    bool isStickyInputMethod(fcitx::InputContext *ic) const;
    std::string findPureChineseImName() const;
    void enterStickyMode(fcitx::InputContext *ic,
                         ShadowBarICState *state,
                         bool show_toast);
    void exitToPureChinese(fcitx::InputContext *ic, ShadowBarICState *state);
    KeyInput toCoreKey(const fcitx::KeyEvent &event) const;

    bool isActionChecked(fcitx::InputContext *ic) const;
    std::string actionShortText(fcitx::InputContext *ic) const;
    void toggleStickyFromAction(fcitx::InputContext *ic);
    void attachStatusAction(fcitx::InputContext *ic);
    void setupStatusMenu();
    void updateStatusMenuState();
    void launchFloatingUi(const std::string &arg) const;
    void toggleOrbFromMenu();

    friend class ShadowBarToggleAction;

    fcitx::Instance *instance_;
    std::thread::id main_thread_id_;

    ConfigManager config_mgr_;
    ShadowBarFcitxConfig fcitx_config_;
    fcitx::EventDispatcher event_dispatcher_;
    AsyncTranslationEngine translator_;

    fcitx::FactoryFor<ShadowBarICState> factory_;
    ShadowBarToggleAction toggle_action_;
    fcitx::Menu status_menu_;
    fcitx::SimpleAction menu_toggle_sticky_action_;
    fcitx::SimpleAction menu_floating_action_;
    fcitx::SimpleAction menu_orb_action_;
    fcitx::SimpleAction menu_reset_floating_action_;
    fcitx::SimpleAction menu_settings_action_;

    fcitx::ScopedConnection commit_filter_conn_;
    std::vector<fcitx::ScopedConnection> menu_conns_;
    std::vector<std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>>>
        event_watchers_;

    bool bypass_commit_filter_ = false;
    bool replaying_key_ = false;
    bool rendering_panel_ = false;
};

class ShadowBarAddonFactory final : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override;
};

} // namespace shadowbar::fcitx_addon
