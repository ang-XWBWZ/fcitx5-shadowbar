#pragma once

#include "shadowbar/types.hpp"

#include <QString>
#include <QStringList>

namespace shadowbar::floating_ui {

/// Static metadata for a Hunyuan HY-MT1.5 local GGUF variant.
struct TyModelSpec {
    QString id;               // "1.25bit", "2bit", or "custom"
    QString display_name;     // Human-readable title
    QString size_hint;        // Approximate GGUF size string
    QString preset_id;        // Recommended preset id ("ty_125bit", "ty_2bit", "ty_custom")
    QString preset_label;     // Short label shown on pill selector
    QString model_name;       // Logical model identifier (triggers HY-MT prompts)
    QString default_filename; // Standard GGUF file name
    QString repo_subpath;     // HuggingFace `<owner>/<repo>/resolve/main/<file>`
};

/// Normalizes variant string to `"1.25bit"`, `"2bit"`, or `"custom"`.
QString normalizeTyVariant(const QString &variant);

/// Normalizes inference backend preference to `"auto"`, `"vulkan"`, or `"cpu"`.
QString normalizeTyBackend(const QString &backend);

/// Returns GPU offload layers (`99` for `"auto"`/`"vulkan"`, `0` for `"cpu"`).
int tyBackendGpuLayers(const QString &backend);

/// Returns static metadata for `"1.25bit"`, `"2bit"`, or `"custom"`.
TyModelSpec tyModelSpec(const QString &variant);

/// Builds download URL on either `"huggingface"` or `"hf-mirror"`.
QString tyDownloadUrl(const QString &variant, const QString &mirror);

/// Resolves `~/.local/share/shadowbar` (honoring `$XDG_DATA_HOME`).
QString defaultShadowbarShareDir();

/// Resolves default `.gguf` file path for `"1.25bit"` or `"2bit"`.
QString defaultTyModelPath(const QString &variant);

/// Resolves effective model file path from config or default share directory.
QString resolvedTyModelPath(const ShadowConfig &cfg, const QString &variant);

/// Resolves `llama-server` binary from config, `~/.local/share/shadowbar/bin`, or `PATH`.
QString resolvedTyServerBinary(const ShadowConfig &cfg);

/// Constructs validated CLI arguments for spawning local `llama-server`.
QStringList buildTyServerArgs(const QString &model_path,
                              int port,
                              int threads,
                              int ctx_size = 2048,
                              int gpu_layers = 0);

/// Builds loopback OpenAI-compatible chat completions endpoint URL.
QString tyChatCompletionsEndpoint(int port);

/// Builds loopback `/health` URL.
QString tyHealthEndpoint(int port);

/// Builds a ShadowBar Preset wired to local `llama-server` for the given variant.
Preset makeTyPreset(const QString &variant,
                    int port,
                    const QString &target_lang = QStringLiteral("en"));

/// Inserts or updates the variant preset in `cfg.presets` and optionally sets it active.
void upsertTyPreset(ShadowConfig &cfg,
                    const QString &variant,
                    bool set_active,
                    const QString &target_lang = QStringLiteral("en"));

/// Returns `"1.25bit"`, `"2bit"`, or `"custom"` if `preset` targets the local TY server; else empty.
QString inferTyVariantForPreset(const Preset &preset, int port);

/// Parsed Vulkan/offload diagnostics extracted from one `llama-server` log line.
struct TyServerLogSignal {
    bool vulkan_device_found = false;
    bool no_vulkan_device = false;
    bool has_layer_offload = false;
    int offloaded_layers = 0;
    int total_layers = 0;
};

/// Inspects a single `llama-server` stdout/stderr log line for Vulkan/offload status.
TyServerLogSignal inspectTyServerLogLine(const QString &line);

} // namespace shadowbar::floating_ui
