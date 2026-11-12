#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace shadowbar {

/// Canonical metadata for a supported translation language.
struct LanguageInfo {
    std::string_view code;
    std::string_view short_label;
    std::string_view zh_name;
    std::string_view en_name;
};

/// External offline dictionary specification ([dicts.<id>] in config.toml).
struct ExternalDictSpec {
    std::string id;
    std::string path;
    std::string source_lang = "auto";
    std::string target_lang = "auto";
    bool enabled = true;
    std::string label;
};

/// Single source of truth for supported translation languages.
const std::vector<LanguageInfo> &supportedLanguages();

/// Normalizes aliases/regional variants (e.g. "zh-CN"->"zh", "zh-hant"->"zh-TW", "jp"->"ja").
std::string normalizeLanguageCode(std::string_view code);

/// Looks up language metadata by canonical code, alias, or name; returns nullptr if unknown.
const LanguageInfo *findLanguageInfo(std::string_view code);

using SteadyClock = std::chrono::steady_clock;
using SteadyTimePoint = SteadyClock::time_point;

/// Operating mode of a ShadowBar session.
enum class Mode {
    Off,     // Inactive; normal IME pass-through (still watches prefix trigger if enabled).
    OneShot, // Activated by prefix (e.g. ";;") or one-shot hotkey; returns to Off after commit/cancel.
    Sticky,  // Locked via toggle hotkey; persists across multiple sentences until toggled off.
};

/// Supported translation backend providers.
enum class ProviderKind {
    GoogleFree,
    OpenAICompatible,
    Ollama,
    DeepL,
    Offline,
};

/// Translation preset representing a provider + style/tone profile.
struct Preset {
    std::string id = "quick";
    std::string label = "自然";
    ProviderKind provider = ProviderKind::GoogleFree;
    std::string endpoint;
    std::string api_key;
    std::string model;
    std::string system_prompt =
        "Translate the user's text into natural, idiomatic target language. "
        "Output ONLY the translation without explanations or quotes.";
    std::string target_lang = "en";
};

/// User-facing ShadowBar configuration (hot-reloadable from config.toml).
struct ShadowConfig {
    // Trigger settings
    bool enable_prefix = true;
    std::string prefix = ";;";
    bool prefix_only_in_cjk_im = true;
    int prefix_timeout_ms = 380;
    std::string toggle_hotkey = "Control+Shift+T";
    std::string cycle_lang_hotkey = "Alt+Shift+L";
    std::string cycle_preset_hotkey = "Alt+Shift+P";
    std::string oneshot_hotkey = "";

    // Behavior settings
    int debounce_ms = 260;
    int request_timeout_ms = 4500;
    bool lock_on_punctuation = true;
    bool esc_clears_pinyin_first = true;
    bool offline_dict_fallback = true;
    std::string offline_dict_path;
    std::vector<ExternalDictSpec> offline_dicts;
    bool auto_detect_direction = true;
    size_t cache_size = 2000;

    // Translation settings
    std::string source_lang = "zh";
    std::string target_lang = "en";
    std::string floating_source_lang = "auto";
    std::vector<std::string> cycle_languages = {"en", "zh", "ja"};
    std::string active_preset = "quick";
    std::vector<Preset> presets;

    std::vector<ExternalDictSpec> effectiveOfflineDicts() const;

    // UI display preferences
    bool show_source_line = true;
    bool collapse_candidates_when_idle = true;
    bool expand_actions = false;

    // Floating translator window & desktop orb preferences ([floating])
    bool floating_enabled = true;
    std::string floating_hotkey = "Alt+Shift+Q";
    bool floating_auto_read_clipboard = true;
    bool floating_watch_clipboard = false;
    bool floating_show_orb = false;
    bool floating_pinned = false;
    bool floating_result_only = false;
    int floating_font_scale = 100;
    int floating_segment_chars = 500;

    // Local Hunyuan Hy-MT2-1.8B (2bit / custom GGUF) runtime preferences ([hymt])
    std::string hymt_variant = "2bit"; // "2bit" (Hy-MT2-1.8B) or "custom"
    std::string hymt_backend = "auto"; // "auto" (Vulkan preferred, CPU fallback), "vulkan", or "cpu"
    int hymt_port = 18081;
    int hymt_threads = 0; // 0 = auto CPU threads
    int hymt_idle_unload_sec = 300; // Auto-unload idle llama-server after N seconds (0 = never)
    std::string hymt_mirror = "huggingface"; // "huggingface" or "hf-mirror"
    std::string hymt_server_path;
    std::string hymt_model_125bit_path; // Legacy unused config key retained for TOML compatibility
    std::string hymt_model_2bit_path;
    std::string hymt_custom_model_path;
    bool hymt_auto_start = true;

    static ShadowConfig makeDefault();
    const Preset *findPreset(const std::string &id) const;
    Preset *findPresetMutable(const std::string &id);
};

/// Status of the asynchronous translation pipeline for the active revision.
enum class TranslationStatus {
    Idle,
    Pending,
    Streaming,
    Ready,
    FallbackOffline,
    Error,
};

/// Platform-neutral key event representation.
enum class SpecialKey {
    None,
    Enter,
    Backspace,
    Escape,
    Tab,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    Delete,
    Space,
};

struct KeyInput {
    SpecialKey special = SpecialKey::None;
    std::string utf8_char; // Printable UTF-8 character if any (excluding control chars)
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
    bool super = false;
    bool is_release = false;

    bool hasCmdModifier() const { return ctrl || alt || super; }
};

/// Output from ShadowSession after processing a key or IME commit.
struct SessionActions {
    bool consume_key = false;
    std::string commit_to_app;
    std::string replay_raw_prefix;
    bool update_ui = false;
    bool trigger_translation = false;
    bool immediate_translation = false;
    bool bypass_cache_translation = false;
    bool cancel_translation = false;
    std::string toast_message;
};

/// Immutable snapshot used by platform UI shells (Fcitx5 InputPanel, TSF popup, CLI sim).
struct ViewModel {
    Mode mode = Mode::Off;
    bool bar_visible = false;
    std::string badge;
    std::string effective_source_lang;
    std::string direction_label;
    std::string source_text;
    size_t cursor_byte = 0;
    std::string translation_text;
    TranslationStatus status = TranslationStatus::Idle;
    std::string status_hint;
    std::string active_preset_id;
    std::string active_preset_label;
    std::string target_lang;
};

/// Output returned by translator / async translation engine.
struct TranslationResult {
    bool ok = false;
    bool is_final = true;
    bool from_offline_dict = false;
    std::string translated_text;
    std::string error_message;
};

} // namespace shadowbar
