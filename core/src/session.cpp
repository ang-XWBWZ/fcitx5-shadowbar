#include "shadowbar/session.hpp"
#include "shadowbar/sentence_splitter.hpp"
#include "shadowbar/utf8_util.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace shadowbar {

namespace {

std::string asciiLowerStr(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : utf8::trimView(s)) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

} // namespace

const std::vector<LanguageInfo> &supportedLanguages() {
    static const std::vector<LanguageInfo> kLangs = {
        {"en", "EN", "英语", "English"},
        {"zh", "中", "中文", "Simplified Chinese"},
        {"zh-TW", "繁中", "繁体中文", "Traditional Chinese"},
        {"ja", "日", "日语", "Japanese"},
        {"ko", "韩", "韩语", "Korean"},
        {"fr", "法", "法语", "French"},
        {"de", "德", "德语", "German"},
        {"es", "西", "西班牙语", "Spanish"},
        {"pt", "葡", "葡萄牙语", "Portuguese"},
        {"ru", "俄", "俄语", "Russian"},
        {"it", "意", "意大利语", "Italian"},
        {"ar", "阿", "阿拉伯语", "Arabic"},
        {"th", "泰", "泰语", "Thai"},
        {"vi", "越", "越南语", "Vietnamese"},
        {"id", "印", "印尼语", "Indonesian"},
        {"hi", "印地", "印地语", "Hindi"},
        {"nl", "荷", "荷兰语", "Dutch"},
        {"pl", "波", "波兰语", "Polish"},
        {"tr", "土", "土耳其语", "Turkish"},
        {"uk", "乌", "乌克兰语", "Ukrainian"},
    };
    return kLangs;
}

std::string normalizeLanguageCode(std::string_view code) {
    const std::string lower = asciiLowerStr(code);
    if (lower.empty() || lower == "auto" || lower == "自动") {
        return "auto";
    }
    struct Alias {
        std::string_view alias;
        std::string_view canonical;
    };
    static constexpr Alias kAliases[] = {
        {"zh", "zh"},         {"zh-cn", "zh"},       {"zh_cn", "zh"},
        {"zh-hans", "zh"},    {"zh_hans", "zh"},     {"chi", "zh"},
        {"zho", "zh"},        {"cmn", "zh"},         {"chinese", "zh"},
        {"中文", "zh"},       {"简体中文", "zh"},    {"汉", "zh"},
        {"zh-tw", "zh-TW"},   {"zh_tw", "zh-TW"},    {"zh-hant", "zh-TW"},
        {"zh_hant", "zh-TW"}, {"zh-hk", "zh-TW"},    {"zh_hk", "zh-TW"},
        {"繁体中文", "zh-TW"},{"繁體中文", "zh-TW"},
        {"en", "en"},         {"eng", "en"},         {"english", "en"},
        {"英语", "en"},       {"英文", "en"},        {"英", "en"},
        {"en-us", "en"},      {"en_us", "en"},       {"en-gb", "en"},
        {"en_gb", "en"},
        {"ja", "ja"},         {"jp", "ja"},          {"jpn", "ja"},
        {"japanese", "ja"},   {"日语", "ja"},        {"日文", "ja"}, {"日", "ja"},
        {"ko", "ko"},         {"kr", "ko"},          {"kor", "ko"},
        {"korean", "ko"},     {"韩语", "ko"},        {"韩文", "ko"}, {"韩", "ko"},
        {"fr", "fr"},         {"fre", "fr"},         {"fra", "fr"},
        {"french", "fr"},     {"法语", "fr"},        {"法文", "fr"}, {"法", "fr"},
        {"de", "de"},         {"ger", "de"},         {"deu", "de"},
        {"german", "de"},     {"德语", "de"},        {"德文", "de"}, {"德", "de"},
        {"es", "es"},         {"spa", "es"},         {"spanish", "es"},
        {"西班牙语", "es"},   {"西班牙文", "es"},    {"西", "es"},
        {"pt", "pt"},         {"pt-br", "pt"},       {"pt-pt", "pt"},
        {"pt_br", "pt"},      {"pt_pt", "pt"},       {"por", "pt"},
        {"portuguese", "pt"}, {"葡萄牙语", "pt"},    {"葡萄牙文", "pt"}, {"葡", "pt"},
        {"ru", "ru"},         {"rus", "ru"},         {"russian", "ru"},
        {"俄语", "ru"},       {"俄文", "ru"},        {"俄", "ru"},
        {"it", "it"},         {"ita", "it"},         {"italian", "it"},
        {"意大利语", "it"},   {"意大利文", "it"},    {"意", "it"},
        {"ar", "ar"},         {"ara", "ar"},         {"arabic", "ar"},
        {"阿拉伯语", "ar"},   {"阿拉伯文", "ar"},    {"阿", "ar"},
        {"th", "th"},         {"tha", "th"},         {"thai", "th"},
        {"泰语", "th"},       {"泰文", "th"},        {"泰", "th"},
        {"vi", "vi"},         {"vie", "vi"},         {"vietnamese", "vi"},
        {"越南语", "vi"},     {"越南文", "vi"},      {"越", "vi"},
        {"id", "id"},         {"ind", "id"},         {"indonesian", "id"},
        {"印尼语", "id"},     {"印尼文", "id"},
        {"hi", "hi"},         {"hin", "hi"},         {"hindi", "hi"},
        {"印地语", "hi"},     {"印地文", "hi"},
        {"nl", "nl"},         {"nld", "nl"},         {"dut", "nl"},
        {"dutch", "nl"},      {"荷兰语", "nl"},      {"荷兰文", "nl"},
        {"pl", "pl"},         {"pol", "pl"},         {"polish", "pl"},
        {"波兰语", "pl"},     {"波兰文", "pl"},
        {"tr", "tr"},         {"tur", "tr"},         {"turkish", "tr"},
        {"土耳其语", "tr"},   {"土耳其文", "tr"},
        {"uk", "uk"},         {"ukr", "uk"},         {"ukrainian", "uk"},
        {"乌克兰语", "uk"},   {"乌克兰文", "uk"},
    };
    for (const auto &entry : kAliases) {
        if (lower == entry.alias) {
            return std::string(entry.canonical);
        }
    }
    return std::string(utf8::trimView(code));
}

const LanguageInfo *findLanguageInfo(std::string_view code) {
    const std::string norm = normalizeLanguageCode(code);
    for (const auto &info : supportedLanguages()) {
        if (info.code == norm) {
            return &info;
        }
    }
    return nullptr;
}

bool containsNonAscii(std::string_view text) {
    for (unsigned char c : text) {
        if (c >= 0x80) {
            return true;
        }
    }
    return false;
}

bool isPureAsciiEnglishDraft(std::string_view text) {
    if (containsNonAscii(text)) {
        return false;
    }
    size_t ascii_letters = 0;
    for (unsigned char c : text) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            ++ascii_letters;
        }
    }
    return ascii_letters >= 2;
}

std::string detectSourceLang(std::string_view text, std::string_view fallback) {
    if (text.empty()) {
        return std::string(fallback);
    }
    if (isPureAsciiEnglishDraft(text)) {
        return "en";
    }
    size_t pos = 0;
    size_t kana_count = 0;
    size_t hangul_count = 0;
    size_t han_count = 0;
    size_t cyrillic_count = 0;
    size_t arabic_count = 0;
    size_t thai_count = 0;
    size_t devanagari_count = 0;
    size_t latin_ext_count = 0;
    size_t ascii_alpha_count = 0;

    while (pos < text.size()) {
        size_t next = pos;
        const uint32_t cp = utf8::decodeAt(text, pos, &next);
        if (next <= pos) {
            break;
        }
        pos = next;
        if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) {
            ++ascii_alpha_count;
        } else if ((cp >= 0x3040 && cp <= 0x309F) || (cp >= 0x30A0 && cp <= 0x30FF) ||
                   (cp >= 0x31F0 && cp <= 0x31FF) || (cp >= 0xFF66 && cp <= 0xFF9F)) {
            ++kana_count;
        } else if ((cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0x1100 && cp <= 0x11FF) ||
                   (cp >= 0x3130 && cp <= 0x318F)) {
            ++hangul_count;
        } else if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
                   (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x20000 && cp <= 0x2CEAF)) {
            ++han_count;
        } else if (cp >= 0x0400 && cp <= 0x052F) {
            ++cyrillic_count;
        } else if ((cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) ||
                   (cp >= 0x08A0 && cp <= 0x08FF)) {
            ++arabic_count;
        } else if (cp >= 0x0E00 && cp <= 0x0E7F) {
            ++thai_count;
        } else if (cp >= 0x0900 && cp <= 0x097F) {
            ++devanagari_count;
        } else if ((cp >= 0x00C0 && cp <= 0x024F) || (cp >= 0x1E00 && cp <= 0x1EFF)) {
            ++latin_ext_count;
        }
    }

    if (kana_count > 0) {
        return "ja";
    }
    if (hangul_count > 0) {
        return "ko";
    }
    if (han_count > 0) {
        return "zh";
    }
    if (cyrillic_count > 0) {
        return "ru";
    }
    if (arabic_count > 0) {
        return "ar";
    }
    if (thai_count > 0) {
        return "th";
    }
    if (devanagari_count > 0) {
        return "hi";
    }
    if (latin_ext_count > 0) {
        return fallback.empty() ? std::string() : std::string("auto");
    }
    if (ascii_alpha_count >= 2) {
        return "en";
    }
    return std::string(fallback);
}

std::pair<std::string, std::string>
resolveTranslationDirection(std::string_view text,
                            std::string_view configured_source,
                            std::string_view configured_target,
                            bool auto_detect) {
    std::string base_source =
        configured_source.empty() ? std::string("zh") : std::string(configured_source);
    std::string base_target =
        configured_target.empty() ? std::string("en") : std::string(configured_target);

    const std::string detected =
        auto_detect ? detectSourceLang(text, "") : std::string();
    const bool ascii_en = isPureAsciiEnglishDraft(text) || detected == "en";

    if (base_source == "auto") {
        if (!detected.empty() && detected != "auto") {
            base_source = detected;
            if (auto_detect && base_source == base_target) {
                base_target = (base_target == "en") ? "zh" : "en";
            }
        }
        return {std::move(base_source), std::move(base_target)};
    }

    if (auto_detect && ascii_en && base_source == "zh") {
        if (base_target == "en") {
            return {"en", "zh"};
        }
        if (base_target != "zh") {
            return {"en", std::move(base_target)};
        }
    }

    if (base_source == base_target) {
        if (!detected.empty() && detected != base_target) {
            base_source = detected;
        } else {
            base_source = ascii_en ? "en" : "auto";
        }
    }
    return {std::move(base_source), std::move(base_target)};
}

std::string formatLanguageLabel(std::string_view code) {
    const std::string trimmed = utf8::trim(code);
    if (trimmed.empty() || trimmed == "auto") {
        return "外";
    }
    if (const LanguageInfo *info = findLanguageInfo(trimmed)) {
        return std::string(info->short_label);
    }
    return trimmed;
}

ShadowSession::ShadowSession(ShadowConfig config) : config_(std::move(config)) {
    active_preset_id_ = config_.active_preset;
    if (const auto *p = config_.findPreset(active_preset_id_)) {
        active_preset_id_ = p->id;
        target_lang_ = !config_.target_lang.empty()
                           ? config_.target_lang
                           : (p->target_lang.empty() ? std::string("en")
                                                     : p->target_lang);
    } else {
        target_lang_ =
            config_.target_lang.empty() ? std::string("en") : config_.target_lang;
    }
}

void ShadowSession::updateConfig(ShadowConfig config) {
    config_ = std::move(config);
    syncSelectionDefaults(config_.active_preset, config_.target_lang);
}

void ShadowSession::syncSelectionDefaults(const std::string &preset_id,
                                          const std::string &target_lang) {
    if (!preset_id.empty()) {
        if (const auto *p = config_.findPreset(preset_id)) {
            active_preset_id_ = p->id;
        }
    } else if (!config_.findPreset(active_preset_id_)) {
        if (const auto *p = config_.findPreset(config_.active_preset)) {
            active_preset_id_ = p->id;
        }
    }
    if (!target_lang.empty()) {
        target_lang_ = target_lang;
    }
}

bool ShadowSession::shouldShowBar() const {
    return mode_ == Mode::OneShot ||
           (mode_ == Mode::Sticky && !source_text_.empty());
}

std::pair<std::string, std::string> ShadowSession::effectiveDirection() const {
    const Preset *p = config_.findPreset(active_preset_id_);
    std::string base_source =
        config_.source_lang.empty() ? std::string("zh") : config_.source_lang;
    std::string base_target = target_lang_;
    if (base_target.empty()) {
        base_target =
            (p && !p->target_lang.empty()) ? p->target_lang : config_.target_lang;
    }
    if (base_target.empty()) {
        base_target = "en";
    }

    return resolveTranslationDirection(source_text_, base_source, base_target,
                                       config_.auto_detect_direction);
}

Preset ShadowSession::activePreset() const {
    Preset out;
    if (const auto *p = config_.findPreset(active_preset_id_)) {
        out = *p;
    }
    auto [src, tgt] = effectiveDirection();
    (void)src;
    out.target_lang = std::move(tgt);
    return out;
}

std::string ShadowSession::languageLabel(std::string_view code) const {
    return formatLanguageLabel(code);
}

void ShadowSession::insertSourceAtCursor(std::string_view text) {
    if (text.empty()) {
        return;
    }
    cursor_byte_ = std::min(cursor_byte_, source_text_.size());
    source_text_.insert(cursor_byte_, text);
    cursor_byte_ += text.size();
}

bool ShadowSession::deleteBeforeCursor() {
    if (source_text_.empty() || cursor_byte_ == 0) {
        return false;
    }
    cursor_byte_ = std::min(cursor_byte_, source_text_.size());
    const size_t prev = utf8::prevCharOffset(source_text_, cursor_byte_);
    source_text_.erase(prev, cursor_byte_ - prev);
    cursor_byte_ = prev;
    return true;
}

void ShadowSession::resetDraft(bool cancel_async, SessionActions &actions) {
    source_text_.clear();
    cursor_byte_ = 0;
    translation_text_.clear();
    status_ = TranslationStatus::Idle;
    status_hint_.clear();
    ++revision_;
    if (cancel_async) {
        actions.cancel_translation = true;
    }
}

void ShadowSession::scheduleTranslation(bool immediate, SessionActions &actions) {
    ++revision_;
    if (utf8::trimView(source_text_).empty()) {
        translation_text_.clear();
        status_ = TranslationStatus::Idle;
        status_hint_.clear();
        actions.cancel_translation = true;
        return;
    }
    status_ = TranslationStatus::Pending;
    status_hint_ = "···";
    actions.trigger_translation = true;
    actions.immediate_translation =
        immediate || (config_.lock_on_punctuation &&
                      endsWithSentenceTerminator(source_text_));
}

SessionActions ShadowSession::toggleSticky() {
    SessionActions actions;
    pending_prefix_raw_.clear();
    if (mode_ == Mode::Sticky) {
        resetDraft(true, actions);
        mode_ = Mode::Off;
        actions.update_ui = true;
        actions.toast_message = "ShadowBar 翻译模式: 关闭";
    } else {
        return enterSticky(true);
    }
    return actions;
}

SessionActions ShadowSession::enterSticky(bool show_toast) {
    SessionActions actions;
    pending_prefix_raw_.clear();
    resetDraft(true, actions);
    mode_ = Mode::Sticky;
    actions.update_ui = true;
    if (show_toast) {
        auto [eff_src, eff_tgt] = effectiveDirection();
        actions.toast_message = "ShadowBar 翻译模式: 开启 [" +
                                languageLabel(eff_src) + "→" +
                                languageLabel(eff_tgt) + "]";
    }
    return actions;
}

SessionActions ShadowSession::enterOneShot() {
    SessionActions actions;
    pending_prefix_raw_.clear();
    resetDraft(true, actions);
    mode_ = Mode::OneShot;
    actions.update_ui = true;
    return actions;
}

SessionActions ShadowSession::exitToOff() {
    SessionActions actions;
    pending_prefix_raw_.clear();
    resetDraft(true, actions);
    mode_ = Mode::Off;
    actions.update_ui = true;
    return actions;
}

SessionActions ShadowSession::flushPendingPrefixIfExpired(SteadyTimePoint now) {
    SessionActions actions;
    if (!pending_prefix_raw_.empty() && now >= pending_prefix_deadline_) {
        actions.replay_raw_prefix = std::move(pending_prefix_raw_);
        pending_prefix_raw_.clear();
    }
    return actions;
}

SessionActions ShadowSession::onKey(const KeyInput &key,
                                    bool ime_composing,
                                    bool in_cjk_im,
                                    SteadyTimePoint now) {
    SessionActions actions;
    if (key.is_release) {
        return actions;
    }

    // 1. Handle inactive mode: prefix trigger watch (e.g. ";;").
    if (mode_ == Mode::Off) {
        if (!config_.enable_prefix || config_.prefix.empty()) {
            return actions;
        }

        const bool prefix_allowed =
            in_cjk_im || !config_.prefix_only_in_cjk_im || !pending_prefix_raw_.empty();
        if (!prefix_allowed || ime_composing) {
            if (!pending_prefix_raw_.empty()) {
                actions.replay_raw_prefix = std::move(pending_prefix_raw_);
                pending_prefix_raw_.clear();
            }
            return actions;
        }

        if (!pending_prefix_raw_.empty() && now >= pending_prefix_deadline_) {
            actions.replay_raw_prefix = std::move(pending_prefix_raw_);
            pending_prefix_raw_.clear();
        }

        if (!key.hasCmdModifier() && !key.utf8_char.empty()) {
            const size_t next_idx = pending_prefix_raw_.size();
            const std::string_view prefix_view(config_.prefix);
            if (next_idx < prefix_view.size() &&
                prefix_view.substr(next_idx, key.utf8_char.size()) == key.utf8_char) {
                pending_prefix_raw_ += key.utf8_char;
                pending_prefix_deadline_ =
                    now + std::chrono::milliseconds(config_.prefix_timeout_ms);
                actions.consume_key = true;

                if (pending_prefix_raw_ == config_.prefix) {
                    pending_prefix_raw_.clear();
                    resetDraft(true, actions);
                    mode_ = Mode::OneShot;
                    actions.update_ui = true;
                }
                return actions;
            }
        }

        // Different key arrived while buffering partial prefix -> flush prefix first and pass current key.
        if (!pending_prefix_raw_.empty()) {
            actions.replay_raw_prefix = std::move(pending_prefix_raw_);
            pending_prefix_raw_.clear();
        }
        return actions;
    }

    // 2. Active mode (OneShot or Sticky): let underlying IME handle keys while composing pinyin.
    if (ime_composing) {
        return actions;
    }

    if (key.ctrl && !key.alt && !key.super && key.special == SpecialKey::Backspace) {
        return clearDraft();
    }

    if (key.hasCmdModifier()) {
        return actions;
    }

    switch (key.special) {
    case SpecialKey::Enter:
        return key.shift ? commitOriginal() : commitTranslation();

    case SpecialKey::Backspace:
        if (deleteBeforeCursor()) {
            actions.consume_key = true;
            actions.update_ui = true;
            if (source_text_.empty()) {
                resetDraft(true, actions);
            } else {
                scheduleTranslation(false, actions);
            }
            return actions;
        }
        if (mode_ == Mode::OneShot) {
            mode_ = Mode::Off;
            actions.consume_key = true;
            actions.update_ui = true;
            return actions;
        }
        // Sticky mode with empty buffer: pass BackSpace through to target application.
        return actions;

    case SpecialKey::Escape:
        if (!source_text_.empty()) {
            resetDraft(true, actions);
            if (mode_ == Mode::OneShot) {
                mode_ = Mode::Off;
            }
            actions.consume_key = true;
            actions.update_ui = true;
            return actions;
        }
        if (mode_ == Mode::OneShot) {
            mode_ = Mode::Off;
            actions.consume_key = true;
            actions.update_ui = true;
            return actions;
        }
        // Sticky mode with empty buffer: NEVER steal Esc (e.g. Vim / dialog dismiss).
        return actions;

    case SpecialKey::Tab:
        if (!source_text_.empty() || mode_ == Mode::OneShot) {
            return cyclePreset(key.shift);
        }
        return actions;

    case SpecialKey::Left:
        if (!source_text_.empty()) {
            cursor_byte_ = utf8::prevCharOffset(source_text_, cursor_byte_);
            actions.consume_key = true;
            actions.update_ui = true;
            return actions;
        }
        return actions;

    case SpecialKey::Right:
        if (!source_text_.empty()) {
            cursor_byte_ = utf8::nextCharOffset(source_text_, cursor_byte_);
            actions.consume_key = true;
            actions.update_ui = true;
            return actions;
        }
        return actions;

    case SpecialKey::Home:
        if (!source_text_.empty()) {
            cursor_byte_ = 0;
            actions.consume_key = true;
            actions.update_ui = true;
            return actions;
        }
        return actions;

    case SpecialKey::End:
        if (!source_text_.empty()) {
            cursor_byte_ = source_text_.size();
            actions.consume_key = true;
            actions.update_ui = true;
            return actions;
        }
        return actions;

    case SpecialKey::Up:
    case SpecialKey::Down:
        return actions;

    case SpecialKey::Space:
        if (!source_text_.empty()) {
            insertSourceAtCursor(" ");
            actions.consume_key = true;
            actions.update_ui = true;
            scheduleTranslation(false, actions);
            return actions;
        }
        if (mode_ == Mode::OneShot) {
            // Treat ";;<Space>" as accidental prefix in prose/code and exit cleanly.
            mode_ = Mode::Off;
            actions.consume_key = true;
            actions.update_ui = true;
            return actions;
        }
        return actions;

    case SpecialKey::Delete:
        if (!source_text_.empty() && cursor_byte_ < source_text_.size()) {
            size_t next = utf8::nextCharOffset(source_text_, cursor_byte_);
            source_text_.erase(cursor_byte_, next - cursor_byte_);
            actions.consume_key = true;
            actions.update_ui = true;
            if (source_text_.empty()) {
                resetDraft(true, actions);
            } else {
                scheduleTranslation(false, actions);
            }
            return actions;
        }
        return actions;

    case SpecialKey::None:
        break;
    }

    // Printable character fallback (e.g. uppercase English like "API", digits, or English mode).
    if (!key.utf8_char.empty()) {
        insertSourceAtCursor(key.utf8_char);
        actions.consume_key = true;
        actions.update_ui = true;
        scheduleTranslation(false, actions);
    }
    return actions;
}

SessionActions ShadowSession::onCommitFromIme(const std::string &committed_text,
                                              SteadyTimePoint) {
    SessionActions actions;
    if (mode_ == Mode::Off || committed_text.empty()) {
        return actions;
    }
    insertSourceAtCursor(committed_text);
    actions.update_ui = true;
    scheduleTranslation(false, actions);
    return actions;
}

bool ShadowSession::onTranslationUpdate(uint64_t rev,
                                          const TranslationResult &result) {
    if (rev != revision_ || mode_ == Mode::Off || source_text_.empty()) {
        return false;
    }
    if (result.ok) {
        if (!result.translated_text.empty()) {
            translation_text_ = result.translated_text;
        }
        if (result.from_offline_dict) {
            status_ = TranslationStatus::FallbackOffline;
            status_hint_ = "本地词典";
        } else if (result.is_final) {
            status_ = TranslationStatus::Ready;
            status_hint_.clear();
        } else {
            status_ = TranslationStatus::Streaming;
            status_hint_ = "···";
        }
    } else {
        status_ = TranslationStatus::Error;
        if (!result.translated_text.empty()) {
            translation_text_ = result.translated_text;
        }
        status_hint_ = result.error_message.empty()
                           ? "翻译失败 · Enter 重试 / Shift+Enter 发原文"
                           : (result.error_message + " · Enter 重试");
    }
    return true;
}

SessionActions ShadowSession::commitTranslation() {
    SessionActions actions;
    if (!source_text_.empty()) {
        if (!utf8::trimView(translation_text_).empty()) {
            actions.commit_to_app = translation_text_;
            resetDraft(true, actions);
            if (mode_ == Mode::OneShot) {
                mode_ = Mode::Off;
            }
            actions.consume_key = true;
            actions.update_ui = true;
            return actions;
        }
        if (status_ == TranslationStatus::Error) {
            SessionActions retry = retryTranslation();
            retry.consume_key = true;
            return retry;
        }
        status_hint_ = "翻译中… Shift+Enter 可直出原文";
        actions.consume_key = true;
        actions.update_ui = true;
        return actions;
    }
    if (mode_ == Mode::OneShot) {
        mode_ = Mode::Off;
        actions.consume_key = true;
        actions.update_ui = true;
        return actions;
    }
    // Sticky mode with empty buffer: let Enter pass through to send chat messages!
    return actions;
}

SessionActions ShadowSession::retryTranslation() {
    SessionActions actions;
    if (mode_ == Mode::Off || source_text_.empty()) {
        return actions;
    }
    status_ = TranslationStatus::Pending;
    status_hint_.clear();
    actions.update_ui = true;
    scheduleTranslation(true, actions);
    actions.bypass_cache_translation = true;
    return actions;
}

SessionActions ShadowSession::commitOriginal() {
    SessionActions actions;
    if (!source_text_.empty()) {
        actions.commit_to_app = source_text_;
        resetDraft(true, actions);
        if (mode_ == Mode::OneShot) {
            mode_ = Mode::Off;
        }
        actions.consume_key = true;
        actions.update_ui = true;
        return actions;
    }
    if (mode_ == Mode::OneShot) {
        mode_ = Mode::Off;
        actions.consume_key = true;
        actions.update_ui = true;
        return actions;
    }
    return actions;
}

SessionActions ShadowSession::clearDraft() {
    SessionActions actions;
    if (!source_text_.empty()) {
        resetDraft(true, actions);
        actions.consume_key = true;
        actions.update_ui = true;
        return actions;
    }
    if (mode_ == Mode::OneShot) {
        mode_ = Mode::Off;
        actions.consume_key = true;
        actions.update_ui = true;
        return actions;
    }
    return actions;
}

SessionActions ShadowSession::cyclePreset(bool reverse) {
    SessionActions actions;
    if (config_.presets.empty()) {
        return actions;
    }
    const size_t n = config_.presets.size();
    size_t idx = 0;
    for (size_t i = 0; i < n; ++i) {
        if (config_.presets[i].id == active_preset_id_) {
            idx = reverse ? ((i + n - 1) % n) : ((i + 1) % n);
            break;
        }
    }
    active_preset_id_ = config_.presets[idx].id;
    actions.consume_key = true;
    actions.update_ui = true;
    if (!source_text_.empty()) {
        scheduleTranslation(true, actions);
    } else {
        actions.toast_message = "ShadowBar 译文配置: " + config_.presets[idx].label;
    }
    return actions;
}

SessionActions ShadowSession::cycleTargetLang() {
    SessionActions actions;
    std::vector<std::string> langs;
    langs.reserve(config_.cycle_languages.size());
    for (const auto &raw : config_.cycle_languages) {
        std::string norm = normalizeLanguageCode(raw);
        if (norm.empty() || norm == "auto") {
            continue;
        }
        if (std::find(langs.begin(), langs.end(), norm) == langs.end()) {
            langs.push_back(std::move(norm));
        }
    }
    if (langs.empty()) {
        langs = {"en", "zh", "ja"};
    }

    const std::string cur_tgt = effectiveDirection().second;
    const std::string detected = detectSourceLang(source_text_, "");

    size_t cur_idx = langs.size() - 1;
    for (size_t i = 0; i < langs.size(); ++i) {
        if (cur_tgt == langs[i]) {
            cur_idx = i;
            break;
        }
    }

    size_t next = (cur_idx + 1) % langs.size();
    for (size_t step = 1; step <= langs.size(); ++step) {
        const size_t cand = (cur_idx + step) % langs.size();
        const std::string &code = langs[cand];
        if (!detected.empty() && code == detected) {
            continue;
        }
        next = cand;
        break;
    }

    target_lang_ = langs[next];
    actions.consume_key = true;
    actions.update_ui = true;
    if (!source_text_.empty()) {
        scheduleTranslation(true, actions);
    } else {
        auto [eff_src, eff_tgt] = effectiveDirection();
        actions.toast_message = "ShadowBar 目标语言: " + languageLabel(eff_tgt) +
                                " (" + languageLabel(eff_src) + "→" +
                                languageLabel(eff_tgt) + ")";
    }
    return actions;
}

ViewModel ShadowSession::viewModel() const {
    ViewModel vm;
    vm.mode = mode_;
    vm.bar_visible = shouldShowBar();
    auto [eff_src, eff_tgt] = effectiveDirection();
    Preset preset = activePreset();
    vm.active_preset_id = preset.id;
    vm.active_preset_label = preset.label;
    vm.effective_source_lang = eff_src;
    vm.target_lang = eff_tgt;
    vm.direction_label = languageLabel(eff_src) + "→" + languageLabel(eff_tgt);
    vm.badge = vm.direction_label + " · " + preset.label;
    vm.source_text = source_text_;
    vm.cursor_byte = std::min(cursor_byte_, source_text_.size());
    vm.translation_text = translation_text_;
    vm.status = status_;
    vm.status_hint = status_hint_;
    return vm;
}

} // namespace shadowbar
