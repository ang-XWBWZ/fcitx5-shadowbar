#include "shadowbar_im.hpp"
#include "shadowbar_addon_public.h"

#include <fcitx/addonmanager.h>
#include <fcitx/inputmethodmanager.h>

namespace shadowbar::fcitx_addon {

ShadowBarIMEngine::ShadowBarIMEngine(fcitx::Instance *instance)
    : instance_(instance) {}

ShadowBarIMEngine::Delegate ShadowBarIMEngine::resolveDelegate() const {
    if (!instance_) {
        return {};
    }
    const char *candidates[] = {"pinyin", "shuangpin"};
    for (const char *im_name : candidates) {
        const auto *im_entry = instance_->inputMethodManager().entry(im_name);
        if (!im_entry) {
            continue;
        }
        auto *addon = instance_->addonManager().addon(im_entry->addon(), true);
        if (auto *engine = dynamic_cast<fcitx::InputMethodEngine *>(addon)) {
            return Delegate{engine, im_entry};
        }
    }
    return {};
}

fcitx::AddonInstance *ShadowBarIMEngine::shadowbarModule() const {
    if (!instance_) {
        return nullptr;
    }
    return instance_->addonManager().addon("shadowbar", true);
}

std::vector<fcitx::InputMethodEntry> ShadowBarIMEngine::listInputMethods() {
    fcitx::InputMethodEntry entry("shadowbar", "ShadowBar Translator",
                                  "zh_CN", "shadowbarim");
    entry.setNativeName("翻译输入 (ShadowBar)")
        .setIcon("accessories-dictionary")
        .setLabel("译")
        .setConfigurable(true);
    std::vector<fcitx::InputMethodEntry> result;
    result.push_back(std::move(entry));
    return result;
}

void ShadowBarIMEngine::activate(const fcitx::InputMethodEntry &,
                                 fcitx::InputContextEvent &event) {
    // Ensure shadowbar module is loaded so its CommitFilter & bar UI handle events.
    shadowbarModule();
    if (auto del = resolveDelegate(); del.engine && del.entry) {
        del.engine->activate(*del.entry, event);
    }
}

void ShadowBarIMEngine::deactivate(const fcitx::InputMethodEntry &,
                                   fcitx::InputContextEvent &event) {
    if (auto del = resolveDelegate(); del.engine && del.entry) {
        del.engine->deactivate(*del.entry, event);
    }
}

void ShadowBarIMEngine::reset(const fcitx::InputMethodEntry &,
                              fcitx::InputContextEvent &event) {
    if (auto del = resolveDelegate(); del.engine && del.entry) {
        del.engine->reset(*del.entry, event);
    }
}

void ShadowBarIMEngine::keyEvent(const fcitx::InputMethodEntry &,
                                 fcitx::KeyEvent &keyEvent) {
    if (auto del = resolveDelegate(); del.engine && del.entry) {
        del.engine->keyEvent(*del.entry, keyEvent);
    }
}

void ShadowBarIMEngine::filterKey(const fcitx::InputMethodEntry &,
                                  fcitx::KeyEvent &event) {
    if (auto del = resolveDelegate(); del.engine && del.entry) {
        del.engine->filterKey(*del.entry, event);
    }
}

std::string ShadowBarIMEngine::subMode(const fcitx::InputMethodEntry &,
                                       fcitx::InputContext &ic) {
    bool active = true;
    if (auto *mod = shadowbarModule()) {
        active = mod->call<fcitx::IShadowBarAddon::isTranslationActive>(&ic);
    }
    if (!active) {
        return "纯中文 (译关)";
    }
    if (auto del = resolveDelegate(); del.engine && del.entry) {
        std::string sub = del.engine->subMode(*del.entry, ic);
        if (!sub.empty()) {
            return "译·" + sub;
        }
    }
    return "翻译输入";
}

std::string ShadowBarIMEngine::subModeIconImpl(const fcitx::InputMethodEntry &,
                                               fcitx::InputContext &) {
    return "accessories-dictionary";
}

std::string ShadowBarIMEngine::subModeLabelImpl(const fcitx::InputMethodEntry &,
                                                fcitx::InputContext &ic) {
    bool active = true;
    if (auto *mod = shadowbarModule()) {
        active = mod->call<fcitx::IShadowBarAddon::isTranslationActive>(&ic);
    }
    if (auto del = resolveDelegate(); del.engine && del.entry) {
        if (auto *v2 = dynamic_cast<fcitx::InputMethodEngineV2 *>(del.engine)) {
            std::string lbl = v2->subModeLabelImpl(*del.entry, ic);
            if (!active) {
                return (lbl == "En" || lbl == "英") ? "英" : "中";
            }
            if (lbl == "En" || lbl == "英") {
                return "译E";
            }
        }
    }
    return active ? "译" : "中";
}

const fcitx::Configuration *
ShadowBarIMEngine::getConfigForInputMethod(const fcitx::InputMethodEntry &) const {
    if (auto *mod = shadowbarModule()) {
        return mod->getConfig();
    }
    return nullptr;
}

void ShadowBarIMEngine::setConfigForInputMethod(const fcitx::InputMethodEntry &,
                                                const fcitx::RawConfig &config) {
    if (auto *mod = shadowbarModule()) {
        mod->setConfig(config);
    }
}

fcitx::AddonInstance *ShadowBarIMFactory::create(fcitx::AddonManager *manager) {
    return new ShadowBarIMEngine(manager->instance());
}

} // namespace shadowbar::fcitx_addon

#ifdef FCITX_ADDON_FACTORY_V2_BACKWARDS
FCITX_ADDON_FACTORY_V2_BACKWARDS(shadowbarim, shadowbar::fcitx_addon::ShadowBarIMFactory)
#else
FCITX_ADDON_FACTORY(shadowbar::fcitx_addon::ShadowBarIMFactory)
#endif
