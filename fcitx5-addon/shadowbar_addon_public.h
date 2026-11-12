#pragma once

#include <fcitx/addoninstance.h>

namespace fcitx {
class InputContext;
} // namespace fcitx

FCITX_ADDON_DECLARE_FUNCTION(ShadowBarAddon, isTranslationActive,
                             bool(fcitx::InputContext *));
