#include "shadowbar/config.hpp"
#include "shadowbar/utf8_util.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

namespace shadowbar {

namespace {

std::string escapeTomlString(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

std::string stripInlineComment(std::string_view line) {
    bool in_quotes = false;
    bool escaped = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\' && in_quotes) {
            escaped = true;
            continue;
        }
        if (c == '"') {
            in_quotes = !in_quotes;
            continue;
        }
        if (c == '#' && !in_quotes) {
            return std::string(line.substr(0, i));
        }
    }
    return std::string(line);
}

bool parseTomlValueString(std::string_view raw, std::string &out) {
    raw = utf8::trimView(raw);
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        out.clear();
        bool escaped = false;
        for (size_t i = 1; i + 1 < raw.size(); ++i) {
            char c = raw[i];
            if (escaped) {
                switch (c) {
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                default:
                    out.push_back(c);
                    break;
                }
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else {
                out.push_back(c);
            }
        }
        return !escaped;
    }
    if (raw.size() >= 2 && raw.front() == '\'' && raw.back() == '\'') {
        out = std::string(raw.substr(1, raw.size() - 2));
        return true;
    }
    out = std::string(raw);
    return true;
}

bool parseBool(std::string_view raw, bool &out) {
    raw = utf8::trimView(raw);
    if (raw == "true") {
        out = true;
        return true;
    }
    if (raw == "false") {
        out = false;
        return true;
    }
    return false;
}

bool parseInt(std::string_view raw, int &out) {
    raw = utf8::trimView(raw);
    try {
        size_t pos = 0;
        int v = std::stoi(std::string(raw), &pos);
        if (pos == raw.size()) {
            out = v;
            return true;
        }
    } catch (...) {
    }
    return false;
}

bool parseTomlStringList(std::string_view raw, std::vector<std::string> &out) {
    raw = utf8::trimView(raw);
    out.clear();
    if (raw.empty()) {
        return true;
    }
    if (raw.front() == '[' && raw.back() == ']') {
        raw = utf8::trimView(raw.substr(1, raw.size() - 2));
        if (raw.empty()) {
            return true;
        }
        size_t start = 0;
        bool in_quote = false;
        char quote_char = '"';
        bool escaped = false;
        for (size_t i = 0; i <= raw.size(); ++i) {
            const bool at_end = (i == raw.size());
            char c = at_end ? ',' : raw[i];
            if (!at_end) {
                if (escaped) {
                    escaped = false;
                    continue;
                }
                if (in_quote && quote_char == '"' && c == '\\') {
                    escaped = true;
                    continue;
                }
                if (in_quote) {
                    if (c == quote_char) {
                        in_quote = false;
                    }
                    continue;
                }
                if (c == '"' || c == '\'') {
                    in_quote = true;
                    quote_char = c;
                    continue;
                }
            }
            if (at_end || c == ',') {
                std::string_view tok = utf8::trimView(raw.substr(start, i - start));
                std::string parsed;
                if (!tok.empty() && parseTomlValueString(tok, parsed)) {
                    parsed = utf8::trim(parsed);
                    if (!parsed.empty()) {
                        out.push_back(std::move(parsed));
                    }
                }
                start = i + 1;
            }
        }
        return !in_quote;
    }

    std::string single;
    if (!parseTomlValueString(raw, single)) {
        return false;
    }
    size_t start = 0;
    while (start <= single.size()) {
        size_t comma = single.find(',', start);
        if (comma == std::string::npos) {
            comma = single.size();
        }
        std::string item = utf8::trim(std::string_view(single).substr(start, comma - start));
        if (!item.empty()) {
            out.push_back(std::move(item));
        }
        start = comma + 1;
    }
    return true;
}

} // namespace

ShadowConfig ShadowConfig::makeDefault() {
    ShadowConfig cfg;
    cfg.presets = {
        Preset{
            .id = "quick",
            .label = "直译",
            .provider = ProviderKind::GoogleFree,
            .endpoint = "",
            .api_key = "",
            .model = "",
            .system_prompt = "",
            .target_lang = "en",
        },
        Preset{
            .id = "natural",
            .label = "自然",
            .provider = ProviderKind::OpenAICompatible,
            .endpoint = "https://api.openai.com/v1/chat/completions",
            .api_key = "env:OPENAI_API_KEY",
            .model = "gpt-4o-mini",
            .system_prompt =
                "Translate the user's text into natural, idiomatic English. "
                "Output only the translation.",
            .target_lang = "en",
        },
        Preset{
            .id = "work",
            .label = "职场",
            .provider = ProviderKind::OpenAICompatible,
            .endpoint = "https://api.openai.com/v1/chat/completions",
            .api_key = "env:OPENAI_API_KEY",
            .model = "gpt-4o-mini",
            .system_prompt =
                "Translate into polite, concise, professional business English. "
                "Output only the translation.",
            .target_lang = "en",
        },
    };
    cfg.active_preset = "quick";
    return cfg;
}

const Preset *ShadowConfig::findPreset(const std::string &id) const {
    for (const auto &p : presets) {
        if (p.id == id) {
            return &p;
        }
    }
    return presets.empty() ? nullptr : &presets.front();
}

Preset *ShadowConfig::findPresetMutable(const std::string &id) {
    for (auto &p : presets) {
        if (p.id == id) {
            return &p;
        }
    }
    return presets.empty() ? nullptr : &presets.front();
}

std::vector<ExternalDictSpec> ShadowConfig::effectiveOfflineDicts() const {
    std::vector<ExternalDictSpec> result;
    std::vector<std::string> seen_paths;
    for (const auto &d : offline_dicts) {
        std::string path = utf8::trim(d.path);
        if (path.empty()) {
            continue;
        }
        seen_paths.push_back(path);
        if (!d.enabled) {
            continue;
        }
        ExternalDictSpec copy = d;
        copy.path = std::move(path);
        copy.source_lang =
            d.source_lang.empty() ? std::string("auto") : normalizeLanguageCode(d.source_lang);
        copy.target_lang =
            d.target_lang.empty() ? std::string("auto") : normalizeLanguageCode(d.target_lang);
        result.push_back(std::move(copy));
    }

    const std::string legacy = utf8::trim(offline_dict_path);
    if (!legacy.empty() &&
        std::find(seen_paths.begin(), seen_paths.end(), legacy) == seen_paths.end()) {
        result.insert(result.begin(), ExternalDictSpec{
                                          .id = "legacy",
                                          .path = legacy,
                                          .source_lang = "auto",
                                          .target_lang = "auto",
                                          .enabled = true,
                                          .label = "兼容词典",
                                      });
    }
    return result;
}

ConfigManager::ConfigManager(std::filesystem::path config_path)
    : config_path_(std::move(config_path)), config_(ShadowConfig::makeDefault()) {}

std::filesystem::path ConfigManager::defaultConfigPath() {
#ifdef _WIN32
    if (const char *appdata = std::getenv("APPDATA")) {
        return std::filesystem::path(appdata) / "shadowbar" / "config.toml";
    }
#endif
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME")) {
        if (*xdg != '\0') {
            return std::filesystem::path(xdg) / "shadowbar" / "config.toml";
        }
    }
    if (const char *home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".config" / "shadowbar" / "config.toml";
    }
    return std::filesystem::path("config.toml");
}

bool ConfigManager::ensureAndLoad(std::string *error) {
    std::error_code ec;
    if (!std::filesystem::exists(config_path_, ec)) {
        config_ = ShadowConfig::makeDefault();
        if (!save(error)) {
            return false;
        }
    }
    return load(error);
}

bool ConfigManager::load(std::string *error) {
    std::ifstream in(config_path_);
    if (!in) {
        if (error) {
            *error = "Failed to open config file: " + config_path_.string();
        }
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();

    ShadowConfig parsed = ShadowConfig::makeDefault();
    if (!parseToml(ss.str(), parsed, error)) {
        return false;
    }
    config_ = std::move(parsed);

    std::error_code ec;
    auto mtime = std::filesystem::last_write_time(config_path_, ec);
    if (!ec) {
        last_mtime_ = mtime;
        has_mtime_ = true;
    }
    return true;
}

bool ConfigManager::reloadIfModified(std::string *error) {
    std::error_code ec;
    auto mtime = std::filesystem::last_write_time(config_path_, ec);
    if (ec) {
        return false;
    }
    if (has_mtime_ && mtime == last_mtime_) {
        return false;
    }
    return load(error);
}

bool ConfigManager::save(std::string *error) {
    std::error_code ec;
    if (config_path_.has_parent_path()) {
        std::filesystem::create_directories(config_path_.parent_path(), ec);
        if (ec) {
            if (error) {
                *error = "Failed to create config directory: " + ec.message();
            }
            return false;
        }
    }

    auto tmp_path = config_path_;
    tmp_path += ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::trunc);
        if (!out) {
            if (error) {
                *error = "Failed to write temp config: " + tmp_path.string();
            }
            return false;
        }
        out << serializeToml(config_);
        out.flush();
        if (!out) {
            if (error) {
                *error = "Failed while flushing temp config";
            }
            return false;
        }
    }
    std::filesystem::rename(tmp_path, config_path_, ec);
    if (ec) {
        if (error) {
            *error = "Failed to finalize config file: " + ec.message();
        }
        return false;
    }
    auto mtime = std::filesystem::last_write_time(config_path_, ec);
    if (!ec) {
        last_mtime_ = mtime;
        has_mtime_ = true;
    }
    return true;
}

std::string ConfigManager::providerToString(ProviderKind kind) {
    switch (kind) {
    case ProviderKind::GoogleFree:
        return "google";
    case ProviderKind::OpenAICompatible:
        return "openai";
    case ProviderKind::Ollama:
        return "ollama";
    case ProviderKind::DeepL:
        return "deepl";
    case ProviderKind::Offline:
        return "offline";
    }
    return "google";
}

ProviderKind ConfigManager::providerFromString(std::string_view s) {
    std::string lower;
    lower.reserve(s.size());
    for (char c : utf8::trimView(s)) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (lower == "openai" || lower == "openai_compatible") {
        return ProviderKind::OpenAICompatible;
    }
    if (lower == "ollama") {
        return ProviderKind::Ollama;
    }
    if (lower == "deepl") {
        return ProviderKind::DeepL;
    }
    if (lower == "offline") {
        return ProviderKind::Offline;
    }
    return ProviderKind::GoogleFree;
}

std::string ConfigManager::sanitizePresetId(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : utf8::trimView(raw)) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-') {
            out.push_back(static_cast<char>(std::tolower(c)));
        } else if (c == ' ') {
            out.push_back('_');
        }
    }
    return out;
}

std::string ConfigManager::resolveSecretValue(std::string_view raw) {
    std::string trimmed = utf8::trim(raw);
    if (trimmed.rfind("env:", 0) == 0) {
        const char *val = std::getenv(trimmed.substr(4).c_str());
        return val ? std::string(val) : std::string();
    }
    if (trimmed.size() >= 4 && trimmed.rfind("${", 0) == 0 && trimmed.back() == '}') {
        std::string var_name = trimmed.substr(2, trimmed.size() - 3);
        const char *val = std::getenv(var_name.c_str());
        return val ? std::string(val) : std::string();
    }
    return trimmed;
}

bool ConfigManager::parseToml(std::string_view content, ShadowConfig &out,
                              std::string *error) {
    std::istringstream stream{std::string(content)};
    std::string line;
    std::string current_section;
    bool saw_custom_presets = false;
    bool saw_custom_dicts = false;

    auto find_or_create_preset = [&](const std::string &id) -> Preset & {
        if (!saw_custom_presets) {
            out.presets.clear();
            saw_custom_presets = true;
        }
        for (auto &p : out.presets) {
            if (p.id == id) {
                return p;
            }
        }
        Preset p;
        p.id = id;
        p.label = id;
        out.presets.push_back(std::move(p));
        return out.presets.back();
    };

    auto find_or_create_dict = [&](const std::string &id) -> ExternalDictSpec & {
        if (!saw_custom_dicts) {
            out.offline_dicts.clear();
            saw_custom_dicts = true;
        }
        for (auto &d : out.offline_dicts) {
            if (d.id == id) {
                return d;
            }
        }
        ExternalDictSpec d;
        d.id = id;
        out.offline_dicts.push_back(std::move(d));
        return out.offline_dicts.back();
    };

    size_t line_no = 0;
    while (std::getline(stream, line)) {
        ++line_no;
        std::string cleaned = stripInlineComment(line);
        std::string_view trimmed = utf8::trimView(cleaned);
        if (trimmed.empty()) {
            continue;
        }

        if (trimmed.front() == '[' && trimmed.back() == ']') {
            current_section = utf8::trim(trimmed.substr(1, trimmed.size() - 2));
            if (current_section.rfind("presets.", 0) == 0) {
                std::string id = current_section.substr(8);
                if (!id.empty()) {
                    find_or_create_preset(id);
                }
            } else if (current_section.rfind("dicts.", 0) == 0) {
                std::string id = current_section.substr(6);
                if (!id.empty()) {
                    find_or_create_dict(id);
                }
            }
            continue;
        }

        size_t eq = trimmed.find('=');
        if (eq == std::string_view::npos) {
            if (error) {
                *error = "Invalid syntax at line " + std::to_string(line_no);
            }
            return false;
        }
        std::string key = utf8::trim(trimmed.substr(0, eq));
        std::string_view val_raw = utf8::trimView(trimmed.substr(eq + 1));

        std::string val_str;
        bool val_bool = false;
        int val_int = 0;

        if (current_section == "trigger") {
            if (key == "enable_prefix" && parseBool(val_raw, val_bool)) {
                out.enable_prefix = val_bool;
            } else if (key == "prefix" && parseTomlValueString(val_raw, val_str)) {
                out.prefix = val_str;
            } else if (key == "prefix_only_in_cjk_im" && parseBool(val_raw, val_bool)) {
                out.prefix_only_in_cjk_im = val_bool;
            } else if (key == "prefix_timeout_ms" && parseInt(val_raw, val_int)) {
                out.prefix_timeout_ms = std::max(80, val_int);
            } else if (key == "toggle_hotkey" && parseTomlValueString(val_raw, val_str)) {
                out.toggle_hotkey = val_str;
            } else if (key == "cycle_lang_hotkey" && parseTomlValueString(val_raw, val_str)) {
                out.cycle_lang_hotkey = val_str;
            } else if (key == "cycle_preset_hotkey" && parseTomlValueString(val_raw, val_str)) {
                out.cycle_preset_hotkey = val_str;
            } else if (key == "oneshot_hotkey" && parseTomlValueString(val_raw, val_str)) {
                out.oneshot_hotkey = val_str;
            }
        } else if (current_section == "behavior") {
            if (key == "debounce_ms" && parseInt(val_raw, val_int)) {
                out.debounce_ms = std::max(0, val_int);
            } else if (key == "request_timeout_ms" && parseInt(val_raw, val_int)) {
                out.request_timeout_ms = std::max(500, val_int);
            } else if (key == "lock_on_punctuation" && parseBool(val_raw, val_bool)) {
                out.lock_on_punctuation = val_bool;
            } else if (key == "esc_clears_pinyin_first" && parseBool(val_raw, val_bool)) {
                out.esc_clears_pinyin_first = val_bool;
            } else if (key == "offline_dict_fallback" && parseBool(val_raw, val_bool)) {
                out.offline_dict_fallback = val_bool;
            } else if (key == "offline_dict_path" && parseTomlValueString(val_raw, val_str)) {
                out.offline_dict_path = val_str;
            } else if (key == "auto_detect_direction" && parseBool(val_raw, val_bool)) {
                out.auto_detect_direction = val_bool;
            } else if (key == "cache_size" && parseInt(val_raw, val_int)) {
                out.cache_size = static_cast<size_t>(std::max(16, val_int));
            }
        } else if (current_section == "translation") {
            std::vector<std::string> list_vals;
            if (key == "source_lang" && parseTomlValueString(val_raw, val_str)) {
                out.source_lang = normalizeLanguageCode(val_str);
            } else if (key == "target_lang" && parseTomlValueString(val_raw, val_str)) {
                out.target_lang = normalizeLanguageCode(val_str);
            } else if (key == "floating_source_lang" &&
                       parseTomlValueString(val_raw, val_str)) {
                out.floating_source_lang = normalizeLanguageCode(val_str);
            } else if (key == "cycle_languages" &&
                       parseTomlStringList(val_raw, list_vals)) {
                std::vector<std::string> cleaned;
                for (const auto &raw_code : list_vals) {
                    std::string norm = normalizeLanguageCode(raw_code);
                    if (!norm.empty() && norm != "auto" &&
                        std::find(cleaned.begin(), cleaned.end(), norm) == cleaned.end()) {
                        cleaned.push_back(std::move(norm));
                    }
                }
                if (!cleaned.empty()) {
                    out.cycle_languages = std::move(cleaned);
                }
            } else if (key == "active_preset" && parseTomlValueString(val_raw, val_str)) {
                out.active_preset = val_str;
            }
        } else if (current_section == "ui") {
            if (key == "show_source_line" && parseBool(val_raw, val_bool)) {
                out.show_source_line = val_bool;
            } else if (key == "collapse_candidates_when_idle" &&
                       parseBool(val_raw, val_bool)) {
                out.collapse_candidates_when_idle = val_bool;
            } else if (key == "expand_actions" && parseBool(val_raw, val_bool)) {
                out.expand_actions = val_bool;
            }
        } else if (current_section == "floating") {
            if (key == "enabled" && parseBool(val_raw, val_bool)) {
                out.floating_enabled = val_bool;
            } else if (key == "hotkey" && parseTomlValueString(val_raw, val_str)) {
                out.floating_hotkey = val_str;
            } else if (key == "auto_read_clipboard" &&
                       parseBool(val_raw, val_bool)) {
                out.floating_auto_read_clipboard = val_bool;
            } else if (key == "watch_clipboard" && parseBool(val_raw, val_bool)) {
                out.floating_watch_clipboard = val_bool;
            } else if (key == "show_orb" && parseBool(val_raw, val_bool)) {
                out.floating_show_orb = val_bool;
            } else if (key == "pinned" && parseBool(val_raw, val_bool)) {
                out.floating_pinned = val_bool;
            } else if (key == "result_only" && parseBool(val_raw, val_bool)) {
                out.floating_result_only = val_bool;
            } else if (key == "font_scale" && parseInt(val_raw, val_int)) {
                out.floating_font_scale = std::clamp(val_int, 80, 160);
            } else if (key == "segment_max_chars" && parseInt(val_raw, val_int)) {
                out.floating_segment_chars = std::clamp(val_int, 150, 4000);
            }
        } else if (current_section == "hymt") {
            if (key == "variant" && parseTomlValueString(val_raw, val_str)) {
                if (val_str == "custom") {
                    out.hymt_variant = "custom";
                } else if (val_str == "2bit" || val_str == "1.25bit") {
                    out.hymt_variant = "2bit";
                }
            } else if (key == "backend" && parseTomlValueString(val_raw, val_str)) {
                if (val_str == "auto" || val_str == "vulkan" || val_str == "cpu") {
                    out.hymt_backend = val_str;
                }
            } else if (key == "port" && parseInt(val_raw, val_int)) {
                out.hymt_port = std::clamp(val_int, 1024, 65535);
            } else if (key == "threads" && parseInt(val_raw, val_int)) {
                out.hymt_threads = std::clamp(val_int, 0, 256);
            } else if (key == "idle_unload_sec" && parseInt(val_raw, val_int)) {
                out.hymt_idle_unload_sec = std::clamp(val_int, 0, 86400);
            } else if (key == "mirror" && parseTomlValueString(val_raw, val_str)) {
                if (val_str == "huggingface" || val_str == "hf-mirror") {
                    out.hymt_mirror = val_str;
                }
            } else if (key == "server_path" && parseTomlValueString(val_raw, val_str)) {
                out.hymt_server_path = val_str;
            } else if (key == "model_125bit_path" && parseTomlValueString(val_raw, val_str)) {
                out.hymt_model_125bit_path = val_str;
            } else if (key == "model_2bit_path" && parseTomlValueString(val_raw, val_str)) {
                out.hymt_model_2bit_path = val_str;
            } else if (key == "custom_model_path" && parseTomlValueString(val_raw, val_str)) {
                out.hymt_custom_model_path = val_str;
            } else if (key == "auto_start" && parseBool(val_raw, val_bool)) {
                out.hymt_auto_start = val_bool;
            }
        } else if (current_section.rfind("presets.", 0) == 0) {
            std::string id = current_section.substr(8);
            if (id.empty()) {
                continue;
            }
            Preset &preset = find_or_create_preset(id);
            if (key == "label" && parseTomlValueString(val_raw, val_str)) {
                preset.label = val_str;
            } else if (key == "provider" && parseTomlValueString(val_raw, val_str)) {
                preset.provider = providerFromString(val_str);
            } else if (key == "endpoint" && parseTomlValueString(val_raw, val_str)) {
                preset.endpoint = val_str;
            } else if (key == "api_key" && parseTomlValueString(val_raw, val_str)) {
                preset.api_key = val_str;
            } else if (key == "model" && parseTomlValueString(val_raw, val_str)) {
                preset.model = val_str;
            } else if (key == "prompt" && parseTomlValueString(val_raw, val_str)) {
                preset.system_prompt = val_str;
            } else if (key == "target_lang" && parseTomlValueString(val_raw, val_str)) {
                preset.target_lang = val_str.empty() ? std::string() : normalizeLanguageCode(val_str);
            }
        } else if (current_section.rfind("dicts.", 0) == 0) {
            std::string id = current_section.substr(6);
            if (id.empty()) {
                continue;
            }
            ExternalDictSpec &spec = find_or_create_dict(id);
            if (key == "path" && parseTomlValueString(val_raw, val_str)) {
                spec.path = val_str;
            } else if (key == "source_lang" && parseTomlValueString(val_raw, val_str)) {
                spec.source_lang = normalizeLanguageCode(val_str);
            } else if (key == "target_lang" && parseTomlValueString(val_raw, val_str)) {
                spec.target_lang = normalizeLanguageCode(val_str);
            } else if (key == "enabled" && parseBool(val_raw, val_bool)) {
                spec.enabled = val_bool;
            } else if (key == "label" && parseTomlValueString(val_raw, val_str)) {
                spec.label = val_str;
            }
        }
    }

    if (out.presets.empty()) {
        out.presets = ShadowConfig::makeDefault().presets;
    }
    if (!out.findPreset(out.active_preset)) {
        out.active_preset = out.presets.front().id;
    }
    return true;
}

std::string ConfigManager::serializeToml(const ShadowConfig &cfg) {
    std::ostringstream out;
    out << "# ShadowBar 伴随式实时翻译输入配置（保存后自动热重载）\n\n";

    out << "[trigger]\n";
    out << "enable_prefix = " << (cfg.enable_prefix ? "true" : "false") << "\n";
    out << "prefix = \"" << escapeTomlString(cfg.prefix) << "\"\n";
    out << "prefix_only_in_cjk_im = "
        << (cfg.prefix_only_in_cjk_im ? "true" : "false") << "\n";
    out << "prefix_timeout_ms = " << cfg.prefix_timeout_ms << "\n";
    out << "toggle_hotkey = \"" << escapeTomlString(cfg.toggle_hotkey) << "\"\n";
    out << "cycle_lang_hotkey = \"" << escapeTomlString(cfg.cycle_lang_hotkey) << "\"\n";
    out << "cycle_preset_hotkey = \"" << escapeTomlString(cfg.cycle_preset_hotkey) << "\"\n";
    out << "oneshot_hotkey = \"" << escapeTomlString(cfg.oneshot_hotkey) << "\"\n\n";

    out << "[behavior]\n";
    out << "debounce_ms = " << cfg.debounce_ms << "\n";
    out << "request_timeout_ms = " << cfg.request_timeout_ms << "\n";
    out << "lock_on_punctuation = "
        << (cfg.lock_on_punctuation ? "true" : "false") << "\n";
    out << "esc_clears_pinyin_first = "
        << (cfg.esc_clears_pinyin_first ? "true" : "false") << "\n";
    out << "offline_dict_fallback = "
        << (cfg.offline_dict_fallback ? "true" : "false") << "\n";
    out << "offline_dict_path = \"" << escapeTomlString(cfg.offline_dict_path) << "\"\n";
    out << "auto_detect_direction = "
        << (cfg.auto_detect_direction ? "true" : "false") << "\n";
    out << "cache_size = " << cfg.cache_size << "\n\n";

    out << "[translation]\n";
    out << "source_lang = \"" << escapeTomlString(cfg.source_lang) << "\"\n";
    out << "target_lang = \"" << escapeTomlString(cfg.target_lang) << "\"\n";
    out << "floating_source_lang = \""
        << escapeTomlString(cfg.floating_source_lang.empty() ? "auto"
                                                             : cfg.floating_source_lang)
        << "\"\n";
    out << "cycle_languages = [";
    {
        std::vector<std::string> cycle = cfg.cycle_languages;
        if (cycle.empty()) {
            cycle = {"en", "zh", "ja"};
        }
        for (size_t i = 0; i < cycle.size(); ++i) {
            if (i > 0) {
                out << ", ";
            }
            out << "\"" << escapeTomlString(cycle[i]) << "\"";
        }
    }
    out << "]\n";
    out << "active_preset = \"" << escapeTomlString(cfg.active_preset) << "\"\n\n";

    out << "[ui]\n";
    out << "show_source_line = " << (cfg.show_source_line ? "true" : "false") << "\n";
    out << "collapse_candidates_when_idle = "
        << (cfg.collapse_candidates_when_idle ? "true" : "false") << "\n";
    out << "expand_actions = " << (cfg.expand_actions ? "true" : "false") << "\n\n";

    out << "[floating]\n";
    out << "enabled = " << (cfg.floating_enabled ? "true" : "false") << "\n";
    out << "hotkey = \"" << escapeTomlString(cfg.floating_hotkey) << "\"\n";
    out << "auto_read_clipboard = "
        << (cfg.floating_auto_read_clipboard ? "true" : "false") << "\n";
    out << "watch_clipboard = "
        << (cfg.floating_watch_clipboard ? "true" : "false") << "\n";
    out << "show_orb = " << (cfg.floating_show_orb ? "true" : "false") << "\n";
    out << "pinned = " << (cfg.floating_pinned ? "true" : "false") << "\n";
    out << "result_only = " << (cfg.floating_result_only ? "true" : "false")
        << "\n";
    out << "font_scale = " << std::clamp(cfg.floating_font_scale, 80, 160)
        << "\n";
    out << "segment_max_chars = "
        << std::clamp(cfg.floating_segment_chars, 150, 4000) << "\n\n";

    out << "[hymt]\n";
    out << "variant = \"" << escapeTomlString(cfg.hymt_variant) << "\"\n";
    out << "backend = \"" << escapeTomlString(cfg.hymt_backend) << "\"\n";
    out << "port = " << std::clamp(cfg.hymt_port, 1024, 65535) << "\n";
    out << "threads = " << std::clamp(cfg.hymt_threads, 0, 256) << "\n";
    out << "idle_unload_sec = " << std::clamp(cfg.hymt_idle_unload_sec, 0, 86400) << "\n";
    out << "mirror = \"" << escapeTomlString(cfg.hymt_mirror) << "\"\n";
    out << "server_path = \"" << escapeTomlString(cfg.hymt_server_path) << "\"\n";
    if (!cfg.hymt_model_125bit_path.empty()) {
        out << "model_125bit_path = \""
            << escapeTomlString(cfg.hymt_model_125bit_path) << "\"\n";
    }
    out << "model_2bit_path = \"" << escapeTomlString(cfg.hymt_model_2bit_path)
        << "\"\n";
    out << "custom_model_path = \"" << escapeTomlString(cfg.hymt_custom_model_path)
        << "\"\n";
    out << "auto_start = " << (cfg.hymt_auto_start ? "true" : "false") << "\n";

    for (const auto &p : cfg.presets) {
        out << "\n[presets." << p.id << "]\n";
        out << "label = \"" << escapeTomlString(p.label) << "\"\n";
        out << "provider = \"" << providerToString(p.provider) << "\"\n";
        if (!p.endpoint.empty()) {
            out << "endpoint = \"" << escapeTomlString(p.endpoint) << "\"\n";
        }
        if (!p.api_key.empty()) {
            out << "api_key = \"" << escapeTomlString(p.api_key) << "\"\n";
        }
        if (!p.model.empty()) {
            out << "model = \"" << escapeTomlString(p.model) << "\"\n";
        }
        if (!p.system_prompt.empty()) {
            out << "prompt = \"" << escapeTomlString(p.system_prompt) << "\"\n";
        }
        out << "target_lang = \"" << escapeTomlString(p.target_lang) << "\"\n";
    }

    int dict_idx = 1;
    std::vector<std::string> used_dict_ids;
    for (const auto &d : cfg.offline_dicts) {
        if (utf8::trimView(d.path).empty()) {
            continue;
        }
        std::string did = sanitizePresetId(d.id);
        if (did.empty()) {
            did = "dict_" + std::to_string(dict_idx);
        }
        while (std::find(used_dict_ids.begin(), used_dict_ids.end(), did) !=
               used_dict_ids.end()) {
            did += "_" + std::to_string(dict_idx);
        }
        used_dict_ids.push_back(did);
        ++dict_idx;

        out << "\n[dicts." << did << "]\n";
        if (!d.label.empty()) {
            out << "label = \"" << escapeTomlString(d.label) << "\"\n";
        }
        out << "path = \"" << escapeTomlString(d.path) << "\"\n";
        out << "source_lang = \""
            << escapeTomlString(d.source_lang.empty() ? "auto" : d.source_lang)
            << "\"\n";
        out << "target_lang = \""
            << escapeTomlString(d.target_lang.empty() ? "auto" : d.target_lang)
            << "\"\n";
        out << "enabled = " << (d.enabled ? "true" : "false") << "\n";
    }

    return out.str();
}

} // namespace shadowbar
