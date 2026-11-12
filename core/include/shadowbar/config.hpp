#pragma once

#include "shadowbar/types.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace shadowbar {

/// Parses and writes `config.toml` and supports file-mtime hot reload.
class ConfigManager {
public:
    explicit ConfigManager(std::filesystem::path config_path = defaultConfigPath());

    /// Resolves `$XDG_CONFIG_HOME/shadowbar/config.toml` or `~/.config/shadowbar/config.toml`.
    static std::filesystem::path defaultConfigPath();

    /// Ensures default config exists on disk and loads it.
    bool ensureAndLoad(std::string *error = nullptr);

    /// Loads config from `config_path_`.
    bool load(std::string *error = nullptr);

    /// Checks file mtime and reloads if changed on disk. Returns true if reloaded.
    bool reloadIfModified(std::string *error = nullptr);

    /// Saves current `config_` back to `config_path_`.
    bool save(std::string *error = nullptr);

    const ShadowConfig &config() const { return config_; }
    ShadowConfig &mutableConfig() { return config_; }
    const std::filesystem::path &path() const { return config_path_; }

    /// Parses TOML subset directly from text (used by tests and load()).
    static bool parseToml(std::string_view content, ShadowConfig &out,
                          std::string *error = nullptr);

    /// Serializes `ShadowConfig` into a clean, commented TOML document.
    static std::string serializeToml(const ShadowConfig &cfg);

    /// Resolves `env:VAR_NAME` or `${VAR_NAME}` in API key fields.
    static std::string resolveSecretValue(std::string_view raw);

    static std::string providerToString(ProviderKind kind);
    static ProviderKind providerFromString(std::string_view s);
    static std::string sanitizePresetId(std::string_view raw);

private:
    std::filesystem::path config_path_;
    ShadowConfig config_;
    std::filesystem::file_time_type last_mtime_{};
    bool has_mtime_ = false;
};

} // namespace shadowbar
