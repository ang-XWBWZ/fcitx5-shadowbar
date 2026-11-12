#pragma once

#include <fcitx/addonfactory.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/instance.h>

#include <string>
#include <vector>

namespace shadowbar::fcitx_addon {

/// Dedicated switchable InputMethod entry ("翻译输入 (ShadowBar)").
/// Delegates Chinese Pinyin composition to Fcitx5's `pinyin` engine when installed
/// and keeps ShadowBar translation mode continuously enabled while active.
class ShadowBarIMEngine final : public fcitx::InputMethodEngineV2 {
public:
    explicit ShadowBarIMEngine(fcitx::Instance *instance);

    std::vector<fcitx::InputMethodEntry> listInputMethods() override;

    void activate(const fcitx::InputMethodEntry &entry,
                  fcitx::InputContextEvent &event) override;
    void deactivate(const fcitx::InputMethodEntry &entry,
                    fcitx::InputContextEvent &event) override;
    void reset(const fcitx::InputMethodEntry &entry,
               fcitx::InputContextEvent &event) override;
    void keyEvent(const fcitx::InputMethodEntry &entry,
                  fcitx::KeyEvent &keyEvent) override;
    void filterKey(const fcitx::InputMethodEntry &entry,
                   fcitx::KeyEvent &event) override;

    std::string subMode(const fcitx::InputMethodEntry &entry,
                        fcitx::InputContext &ic) override;
    std::string subModeIconImpl(const fcitx::InputMethodEntry &entry,
                                fcitx::InputContext &ic) override;
    std::string subModeLabelImpl(const fcitx::InputMethodEntry &entry,
                                 fcitx::InputContext &ic) override;

    const fcitx::Configuration *
    getConfigForInputMethod(const fcitx::InputMethodEntry &entry) const override;
    void setConfigForInputMethod(const fcitx::InputMethodEntry &entry,
                                 const fcitx::RawConfig &config) override;

private:
    struct Delegate {
        fcitx::InputMethodEngine *engine = nullptr;
        const fcitx::InputMethodEntry *entry = nullptr;
    };

    Delegate resolveDelegate() const;
    fcitx::AddonInstance *shadowbarModule() const;

    fcitx::Instance *instance_;
};

class ShadowBarIMFactory final : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override;
};

} // namespace shadowbar::fcitx_addon
