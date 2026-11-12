#include "ty_model_helpers.hpp"

#include "shadowbar/translator.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

namespace shadowbar::floating_ui {

namespace {

int clampPort(int port) {
    return std::clamp(port, 1024, 65535);
}

bool isLoopbackHost(const QString &host) {
    const QString h = host.trimmed().toLower();
    return h == QLatin1String("127.0.0.1") ||
           h == QLatin1String("localhost") ||
           h == QLatin1String("::1");
}

} // namespace

QString normalizeTyVariant(const QString &variant) {
    const QString v = variant.trimmed().toLower();
    if (v == QLatin1String("custom")) {
        return QStringLiteral("custom");
    }
    return QStringLiteral("2bit");
}

QString normalizeTyBackend(const QString &backend) {
    const QString b = backend.trimmed().toLower();
    if (b == QLatin1String("vulkan") || b == QLatin1String("gpu")) {
        return QStringLiteral("vulkan");
    }
    if (b == QLatin1String("cpu")) {
        return QStringLiteral("cpu");
    }
    return QStringLiteral("auto");
}

int tyBackendGpuLayers(const QString &backend) {
    return (normalizeTyBackend(backend) == QLatin1String("cpu")) ? 0 : 99;
}

TyModelSpec tyModelSpec(const QString &variant) {
    const QString norm = normalizeTyVariant(variant);
    if (norm == QLatin1String("custom")) {
        return {
            .id = QStringLiteral("custom"),
            .display_name = QStringLiteral("混元 Hy-MT2 · 自定义本地 GGUF"),
            .size_hint = QStringLiteral("本地文件"),
            .preset_id = QStringLiteral("ty_custom"),
            .preset_label = QStringLiteral("混元 Hy-MT2 (自定义)"),
            .model_name = QStringLiteral("hy-mt2-custom"),
            .default_filename = {},
            .repo_subpath = {},
        };
    }
    return {
        .id = QStringLiteral("2bit"),
        .display_name = QStringLiteral("混元 Hy-MT2-1.8B · 2bit (Q2_0C)"),
        .size_hint = QStringLiteral("约 573 MB"),
        .preset_id = QStringLiteral("ty_2bit"),
        .preset_label = QStringLiteral("混元 Hy-MT2 (2bit)"),
        .model_name = QStringLiteral("hy-mt2-1.8b-2bit"),
        .default_filename = QStringLiteral("Hy-MT2-1.8B-2Bit.gguf"),
        .repo_subpath = QStringLiteral(
            "tencent/Hy-MT2-1.8B-2Bit-GGUF/resolve/main/"
            "Hy-MT2-1.8B-2Bit.gguf"),
    };
}

QString tyDownloadUrl(const QString &variant, const QString &mirror) {
    const TyModelSpec spec = tyModelSpec(variant);
    if (spec.repo_subpath.isEmpty()) {
        return {};
    }
    const QString m = mirror.trimmed().toLower();
    const QString host =
        (m == QLatin1String("hf-mirror") || m == QLatin1String("mirror"))
            ? QStringLiteral("https://hf-mirror.com/")
            : QStringLiteral("https://huggingface.co/");
    return host + spec.repo_subpath;
}

QString tyEngineDownloadUrl(const QString &) {
    const QByteArray override_url = qgetenv("SHADOWBAR_ENGINE_URL");
    if (!override_url.trimmed().isEmpty()) {
        return QString::fromUtf8(override_url.trimmed());
    }
    return QStringLiteral(
        "https://github.com/ang-XWBWZ/fcitx5-shadowbar/releases/latest/"
        "download/shadowbar-llama-server-linux-amd64.tar.gz");
}

QString tyEngineRequirementSummary() {
    return QStringLiteral(
        "专版引擎: sjl623/llama.cpp @ 1e411d8 (PR #22836，支持 Q2_0C + hunyuan-dense + Vulkan GPU)");
}

QString defaultShadowbarShareDir() {
    const QByteArray xdg_data = qgetenv("XDG_DATA_HOME");
    if (!xdg_data.trimmed().isEmpty()) {
        return QDir::cleanPath(QString::fromUtf8(xdg_data) + QStringLiteral("/shadowbar"));
    }
    return QDir::homePath() + QStringLiteral("/.local/share/shadowbar");
}

QString defaultTyServerInstallPath() {
    return defaultShadowbarShareDir() + QStringLiteral("/bin/llama-server");
}

QString defaultTyModelPath(const QString &variant) {
    const TyModelSpec spec = tyModelSpec(variant);
    if (spec.default_filename.isEmpty()) {
        return {};
    }
    return defaultShadowbarShareDir() + QStringLiteral("/models/") + spec.default_filename;
}

QString resolvedTyModelPath(const ShadowConfig &cfg, const QString &variant) {
    const QString norm = normalizeTyVariant(variant);
    if (norm == QLatin1String("custom")) {
        return QString::fromStdString(cfg.hymt_custom_model_path).trimmed();
    }
    const QString custom = QString::fromStdString(cfg.hymt_model_2bit_path).trimmed();
    return custom.isEmpty() ? defaultTyModelPath(norm) : custom;
}

QString resolvedTyServerBinary(const ShadowConfig &cfg) {
    const QString configured = QString::fromStdString(cfg.hymt_server_path).trimmed();
    if (!configured.isEmpty()) {
        return configured;
    }

    const QString share_bin =
        defaultShadowbarShareDir() + QStringLiteral("/bin/llama-server");
    const QString local_bin =
        QDir::homePath() + QStringLiteral("/.local/bin/shadowbar-llama-server");

    QStringList candidates;
    candidates << share_bin << local_bin;
    if (QCoreApplication::instance()) {
        candidates << (QCoreApplication::applicationDirPath() +
                       QStringLiteral("/llama-server"));
    }
    for (const QString &cand : candidates) {
        const QFileInfo fi(cand);
        if (fi.exists() && fi.isExecutable()) {
            return cand;
        }
    }

    for (const char *name : {"shadowbar-llama-server", "llama-server"}) {
        const QString found = QStandardPaths::findExecutable(QString::fromLatin1(name));
        if (!found.isEmpty()) {
            return found;
        }
    }
    return share_bin;
}

QStringList buildTyServerArgs(const QString &model_path,
                              int port,
                              int threads,
                              int ctx_size,
                              int gpu_layers) {
    QStringList args;
    args << QStringLiteral("-m") << model_path.trimmed()
         << QStringLiteral("--host") << QStringLiteral("127.0.0.1")
         << QStringLiteral("--port") << QString::number(clampPort(port))
         << QStringLiteral("-c") << QString::number(std::clamp(ctx_size, 512, 8192))
         << QStringLiteral("-np") << QStringLiteral("1")
         << QStringLiteral("-ngl") << QString::number(std::max(0, gpu_layers))
         << QStringLiteral("-lv") << QStringLiteral("4");
    if (threads > 0) {
        args << QStringLiteral("-t") << QString::number(std::clamp(threads, 1, 256));
    }
    return args;
}

QString tyChatCompletionsEndpoint(int port) {
    return QStringLiteral("http://127.0.0.1:%1/v1/chat/completions").arg(clampPort(port));
}

QString tyHealthEndpoint(int port) {
    return QStringLiteral("http://127.0.0.1:%1/health").arg(clampPort(port));
}

QString tySlotsEndpoint(int port) {
    return QStringLiteral("http://127.0.0.1:%1/slots").arg(clampPort(port));
}

bool shouldUnloadIdleTyModel(bool managed_running,
                             int idle_unload_sec,
                             qint64 elapsed_idle_ms,
                             int active_requests) {
    if (!managed_running || idle_unload_sec <= 0 || active_requests > 0) {
        return false;
    }
    return elapsed_idle_ms >= static_cast<qint64>(idle_unload_sec) * 1000LL;
}

bool parseLlamaSlotsBusy(const QByteArray &slots_json) {
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(slots_json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) {
        return false;
    }
    const QJsonArray arr = doc.array();
    for (const QJsonValue &val : arr) {
        if (!val.isObject()) {
            continue;
        }
        const QJsonObject obj = val.toObject();
        if (obj.value(QStringLiteral("is_processing")).toBool(false)) {
            return true;
        }
        if (obj.contains(QStringLiteral("state")) &&
            obj.value(QStringLiteral("state")).toInt(0) != 0) {
            return true;
        }
    }
    return false;
}

qint64 readExternalHyMtActivityAgeMs() {
    const std::string path = hyMtActivityStampPath();
    if (path.empty()) {
        return -1;
    }
    const QFileInfo fi(QString::fromStdString(path));
    if (!fi.exists()) {
        return -1;
    }
    const qint64 modified_ms = fi.lastModified().toMSecsSinceEpoch();
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    if (modified_ms <= 0 || now_ms < modified_ms) {
        return -1;
    }
    return now_ms - modified_ms;
}

Preset makeTyPreset(const QString &variant,
                    int port,
                    const QString &target_lang) {
    const TyModelSpec spec = tyModelSpec(variant);
    Preset p;
    p.id = spec.preset_id.toStdString();
    p.label = spec.preset_label.toStdString();
    p.provider = ProviderKind::OpenAICompatible;
    p.endpoint = tyChatCompletionsEndpoint(port).toStdString();
    p.api_key.clear();
    p.model = spec.model_name.toStdString();
    p.system_prompt.clear();
    p.target_lang = target_lang.trimmed().isEmpty()
                        ? std::string("en")
                        : target_lang.trimmed().toStdString();
    return p;
}

void upsertTyPreset(ShadowConfig &cfg,
                    const QString &variant,
                    bool set_active,
                    const QString &target_lang) {
    Preset next = makeTyPreset(variant, cfg.hymt_port, target_lang);
    Preset *existing = cfg.findPresetMutable(next.id);
    if (existing) {
        if (!existing->target_lang.empty()) {
            next.target_lang = existing->target_lang;
        }
        *existing = next;
    } else {
        cfg.presets.push_back(next);
    }
    if (set_active) {
        cfg.active_preset = next.id;
    }
}

QString inferTyVariantForPreset(const Preset &preset, int port) {
    if (preset.provider != ProviderKind::OpenAICompatible) {
        return {};
    }
    const QUrl url = QUrl::fromUserInput(QString::fromStdString(preset.endpoint));
    if (!url.isValid() || !isLoopbackHost(url.host())) {
        return {};
    }
    const int url_port = url.port(url.scheme() == QLatin1String("https") ? 443 : 80);
    if (url_port != clampPort(port)) {
        return {};
    }
    if (!isHyMtModelName(preset.model)) {
        return {};
    }
    const QString model_lower = QString::fromStdString(preset.model).trimmed().toLower();
    if (model_lower.contains(QLatin1String("custom"))) {
        return QStringLiteral("custom");
    }
    if (model_lower.contains(QLatin1String("2bit")) ||
        model_lower.contains(QLatin1String("1.25bit")) ||
        model_lower.contains(QLatin1String("125bit")) ||
        model_lower.contains(QLatin1String("stq1")) ||
        model_lower.contains(QLatin1String("q2_0c")) ||
        model_lower.contains(QLatin1String("hy-mt"))) {
        return QStringLiteral("2bit");
    }
    return {};
}

TyServerLogSignal inspectTyServerLogLine(const QString &line) {
    TyServerLogSignal sig;
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty()) {
        return sig;
    }

    if (trimmed.contains(QLatin1String("no usable GPU found"), Qt::CaseInsensitive) ||
        trimmed.contains(QLatin1String("Found 0 Vulkan"), Qt::CaseInsensitive) ||
        trimmed.contains(QLatin1String("No Vulkan devices found"), Qt::CaseInsensitive) ||
        trimmed.contains(QLatin1String("Vulkan initialization failed"), Qt::CaseInsensitive)) {
        sig.no_vulkan_device = true;
    }

    if (trimmed.contains(QLatin1String("unknown tensor type"), Qt::CaseInsensitive) ||
        trimmed.contains(QLatin1String("invalid tensor type"), Qt::CaseInsensitive) ||
        trimmed.contains(QLatin1String("unsupported tensor type"), Qt::CaseInsensitive) ||
        trimmed.contains(QLatin1String("unknown model architecture"), Qt::CaseInsensitive) ||
        trimmed.contains(QLatin1String("unknown architecture"), Qt::CaseInsensitive)) {
        sig.unsupported_model_format = true;
    }

    if ((trimmed.contains(QLatin1String("ggml_vulkan: Found")) &&
         !trimmed.contains(QLatin1String("Found 0"), Qt::CaseInsensitive)) ||
        trimmed.contains(QLatin1String("- Vulkan")) ||
        trimmed.contains(QLatin1String("using device Vulkan"))) {
        sig.vulkan_device_found = true;
    }

    static const QRegularExpression kOffloadRe(
        QStringLiteral("\\boffloaded\\s+(\\d+)\\s*/\\s*(\\d+)\\s+layers?\\b"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = kOffloadRe.match(trimmed);
    if (match.hasMatch()) {
        sig.has_layer_offload = true;
        sig.offloaded_layers = match.captured(1).toInt();
        sig.total_layers = match.captured(2).toInt();
    }

    return sig;
}

} // namespace shadowbar::floating_ui
