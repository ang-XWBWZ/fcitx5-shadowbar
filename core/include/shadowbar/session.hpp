#pragma once

#include "shadowbar/types.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace shadowbar {

bool containsNonAscii(std::string_view text);
bool isPureAsciiEnglishDraft(std::string_view text);
std::string detectSourceLang(std::string_view text, std::string_view fallback = "zh");
std::pair<std::string, std::string>
resolveTranslationDirection(std::string_view text,
                            std::string_view configured_source,
                            std::string_view configured_target,
                            bool auto_detect);
std::string formatLanguageLabel(std::string_view code);

/// Pure deterministic interaction state machine for ShadowBar.
/// Contains zero OS/GUI/threading dependencies so it is 100% unit-testable and portable.
class ShadowSession {
public:
    explicit ShadowSession(ShadowConfig config = ShadowConfig::makeDefault());

    void updateConfig(ShadowConfig config);
    void syncSelectionDefaults(const std::string &preset_id,
                               const std::string &target_lang);
    const ShadowConfig &config() const { return config_; }
    const std::string &activePresetId() const { return active_preset_id_; }
    const std::string &rawTargetLang() const { return target_lang_; }

    Mode mode() const { return mode_; }
    bool isActive() const { return mode_ != Mode::Off; }
    bool shouldShowBar() const;
    bool hasPendingPrefix() const { return !pending_prefix_raw_.empty(); }
    uint64_t revision() const { return revision_; }

    /// Mode switching via explicit hotkeys.
    SessionActions toggleSticky();
    SessionActions enterSticky(bool show_toast = false);
    SessionActions enterOneShot();
    SessionActions exitToOff();

    /// Processes a physical/logical key event when `ime_composing` indicates whether
    /// an underlying CJK IME (e.g. Pinyin/Rime) currently owns an active composition.
    SessionActions onKey(const KeyInput &key,
                         bool ime_composing,
                         bool in_cjk_im,
                         SteadyTimePoint now = SteadyClock::now());

    /// Processes text committed by the underlying IME (e.g. chosen Chinese phrase or punctuation).
    SessionActions onCommitFromIme(const std::string &committed_text,
                                   SteadyTimePoint now = SteadyClock::now());

    /// Checks whether a buffered first prefix char (e.g. single ';') has timed out and must be flushed.
    SessionActions flushPendingPrefixIfExpired(SteadyTimePoint now = SteadyClock::now());

    /// Receives async or sync translation output for `rev`.
    bool onTranslationUpdate(uint64_t rev, const TranslationResult &result);

    /// Commits current translation (or retries if last attempt failed).
    SessionActions commitTranslation();

    /// Forces an immediate cache-bypassing re-translation of the active draft.
    SessionActions retryTranslation();

    /// Commits original draft text directly.
    SessionActions commitOriginal();

    /// Clears current draft text cleanly without leaving Sticky mode.
    SessionActions clearDraft();

    /// Cycles active translation preset (e.g. Tab / Shift+Tab key).
    SessionActions cyclePreset(bool reverse = false);

    /// Cycles target language among common targets (en <-> zh <-> ja).
    SessionActions cycleTargetLang();

    /// Produces UI view model snapshot.
    ViewModel viewModel() const;

    /// Returns active preset object (falls back to default preset).
    Preset activePreset() const;

private:
    void insertSourceAtCursor(std::string_view text);
    bool deleteBeforeCursor();
    void resetDraft(bool cancel_async, SessionActions &actions);
    void scheduleTranslation(bool immediate, SessionActions &actions);
    std::pair<std::string, std::string> effectiveDirection() const;
    std::string languageLabel(std::string_view code) const;

    ShadowConfig config_;
    Mode mode_ = Mode::Off;

    std::string pending_prefix_raw_;
    SteadyTimePoint pending_prefix_deadline_{};

    std::string source_text_;
    size_t cursor_byte_ = 0;

    std::string translation_text_;
    TranslationStatus status_ = TranslationStatus::Idle;
    std::string status_hint_;

    std::string active_preset_id_;
    std::string target_lang_;

    uint64_t revision_ = 0;
};

} // namespace shadowbar
