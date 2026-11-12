#pragma once

#include "shadowbar/types.hpp"

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace shadowbar::floating_ui {

/// Static metadata for a Hunyuan Hy-MT2-1.8B local GGUF variant.
struct TyModelSpec {
    QString id;               // "2bit" or "custom"
    QString display_name;     // Human-readable title
    QString size_hint;        // Approximate GGUF size string
    QString preset_id;        // Recommended preset id ("ty_2bit" or "ty_custom")
    QString preset_label;     // Short label shown on pill selector
    QString model_name;       // Logical model identifier (triggers Hy-MT prompts)
    QString default_filename; // Standard GGUF file name
    QString repo_subpath;     // HuggingFace `<owner>/<repo>/resolve/main/<file>`
};

/// Normalizes variant string to `"2bit"` (default, including legacy `"1.25bit"`) or `"custom"`.
QString normalizeTyVariant(const QString &variant);

/// Normalizes inference backend preference to `"auto"`, `"vulkan"`, or `"cpu"`.
QString normalizeTyBackend(const QString &backend);

/// Returns GPU offload layers (`99` for `"auto"`/`"vulkan"`, `0` for `"cpu"`).
int tyBackendGpuLayers(const QString &backend);

/// Returns static metadata for `"2bit"` or `"custom"`.
TyModelSpec tyModelSpec(const QString &variant);

/// Builds model download URL on either `"huggingface"` or `"hf-mirror"`.
QString tyDownloadUrl(const QString &variant, const QString &mirror);

/// Returns the prebuilt `shadowbar-llama-server` release archive URL (`x86_64`).
QString tyEngineDownloadUrl(const QString &mirror);

/// Returns human-readable summary of the pinned `llama-server` engine requirements.
QString tyEngineRequirementSummary();

/// Resolves `~/.local/share/shadowbar` (honoring `$XDG_DATA_HOME`).
QString defaultShadowbarShareDir();

/// Resolves default managed `llama-server` install destination (`~/.local/share/shadowbar/bin/llama-server`).
QString defaultTyServerInstallPath();

/// Resolves default `.gguf` file path for `"2bit"`.
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

/// Builds loopback `/slots` URL.
QString tySlotsEndpoint(int port);

/// Returns true when a managed local server has exceeded the idle unload threshold with no active requests.
bool shouldUnloadIdleTyModel(bool managed_running,
                             int idle_unload_sec,
                             qint64 elapsed_idle_ms,
                             int active_requests);

/// Parses `llama-server` `/slots` JSON array and returns true if any slot is currently processing.
bool parseLlamaSlotsBusy(const QByteArray &slots_json);

/// Returns milliseconds since the last cross-process local HY-MT inference stamp update, or -1 if none.
qint64 readExternalHyMtActivityAgeMs();

/// Builds a ShadowBar Preset wired to local `llama-server` for the given variant.
Preset makeTyPreset(const QString &variant,
                    int port,
                    const QString &target_lang = QStringLiteral("en"));

/// Inserts or updates the variant preset in `cfg.presets` and optionally sets it active.
void upsertTyPreset(ShadowConfig &cfg,
                    const QString &variant,
                    bool set_active,
                    const QString &target_lang = QStringLiteral("en"));

/// Returns `"2bit"` or `"custom"` if `preset` targets the local TY server; else empty.
QString inferTyVariantForPreset(const Preset &preset, int port);

/// Parsed Vulkan/offload/format diagnostics extracted from one `llama-server` log line.
struct TyServerLogSignal {
    bool vulkan_device_found = false;
    bool no_vulkan_device = false;
    bool unsupported_model_format = false;
    bool has_layer_offload = false;
    int offloaded_layers = 0;
    int total_layers = 0;
};

/// Inspects a single `llama-server` stdout/stderr log line for Vulkan/offload status.
TyServerLogSignal inspectTyServerLogLine(const QString &line);

} // namespace shadowbar::floating_ui
