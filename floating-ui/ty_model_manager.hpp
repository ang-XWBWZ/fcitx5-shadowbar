#pragma once

#include "ty_model_helpers.hpp"

#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace shadowbar::floating_ui {

/// Manages local `llama-server` process lifecycle and GGUF model/engine downloads for Hy-MT2.
class TyModelManager final : public QObject {
    Q_OBJECT

public:
    enum class ServerState {
        Stopped,
        Starting,
        Running,
        ExternalRunning,
        Error,
    };
    Q_ENUM(ServerState)

    explicit TyModelManager(QObject *parent = nullptr);
    ~TyModelManager() override;

    ServerState state() const { return state_; }
    QString activeVariant() const { return active_variant_; }
    QString activeModelPath() const { return active_model_path_; }
    int activePort() const { return active_port_; }
    QString statusMessage() const { return status_message_; }
    QStringList recentLogs() const { return log_lines_; }

    bool isDownloading() const;
    QString downloadingVariant() const { return downloading_variant_; }
    bool isManagedProcessRunning() const;
    bool isReadyFor(const QString &variant,
                    const QString &model_path,
                    int port,
                    const QString &backend_mode = {}) const;

    /// Starts or switches managed `llama-server` unless already running identical config.
    bool startServer(const QString &binary_path,
                     const QString &variant,
                     const QString &model_path,
                     int port,
                     int threads,
                     const QString &backend_mode = QStringLiteral("auto"));

    /// Stops managed `llama-server` process cleanly.
    void stopServer();

    /// Triggers one asynchronous `/health` probe on `port`.
    void probeHealthOnce(int port);

    /// Configures idle auto-unload timeout in seconds (`0` disables auto-unload).
    void setIdleUnloadSeconds(int seconds);
    int idleUnloadSeconds() const { return idle_unload_sec_; }

    /// Marks model usage activity so the idle-unload timer resets.
    void markActivity();

    /// Tracks active in-flight translations so idle-unload never stops mid-generation.
    void beginTranslationActivity();
    void endTranslationActivity();
    int activeRequests() const { return active_requests_; }

    /// Downloads official Hy-MT2-1.8B GGUF variant using `curl` into `dest_path`.
    bool startDownload(const QString &variant,
                       const QString &mirror,
                       const QString &dest_path);

    /// Downloads prebuilt `llama-server` archive from GitHub Releases and installs to `dest_bin_path`.
    bool startEngineDownload(const QString &mirror,
                             const QString &dest_bin_path);

    /// Cancels an active GGUF download and removes partial files.
    void cancelDownload();

    /// Shuts down server and active downloads cleanly on app exit.
    void shutdown();

signals:
    void stateChanged(shadowbar::floating_ui::TyModelManager::ServerState state,
                      const QString &message);
    void logAppended(const QString &line);
    void downloadProgress(const QString &variant, int percent, const QString &detail);
    void downloadFinished(const QString &variant, bool ok, const QString &message);

private:
    enum class ProbeKind {
        Health,
        Slots,
    };

    void setState(ServerState next, const QString &msg);
    void appendLog(const QString &line);
    void spawnServerProcess(int gpu_layers);
    bool tryFallbackToCpu(const QString &reason);
    void stopServerProcessOnly();
    void pollStartupHealth();
    void handleHealthProbeResult(uint64_t generation, int port, bool healthy);
    void handleSlotsProbeResult(uint64_t generation, int port, bool busy);
    void enqueueProbe(ProbeKind kind, uint64_t generation, int port);
    void probeWorkerLoop();
    void joinProbeThread();
    void scheduleIdleUnloadTimer(qint64 delay_ms = -1);
    void onIdleTimerFired();
    void unloadForIdle();
    bool startCurlDownload(const QString &tag,
                           const QString &url,
                           const QString &dest_path,
                           bool extract_engine_tar);

    QProcess *server_proc_ = nullptr;
    QByteArray server_stdout_buf_;
    QTimer *health_timer_ = nullptr;
    QTimer *startup_timeout_timer_ = nullptr;
    QTimer *idle_timer_ = nullptr;
    QElapsedTimer last_activity_timer_;
    int idle_unload_sec_ = 300;
    int active_requests_ = 0;

    ServerState state_ = ServerState::Stopped;
    QString active_variant_;
    QString active_model_path_;
    QString active_binary_path_;
    QString active_backend_mode_ = QStringLiteral("auto");
    int active_port_ = 18081;
    int active_threads_ = 0;
    int active_gpu_layers_ = 99;
    int offloaded_layers_ = 0;
    int total_layers_ = 0;
    bool vulkan_device_seen_ = false;
    bool using_vulkan_ = false;
    bool no_vulkan_seen_ = false;
    bool unsupported_format_seen_ = false;
    bool fallback_attempted_ = false;
    QString status_message_ = QStringLiteral("未运行");
    QStringList log_lines_;
    uint64_t probe_generation_ = 0;

    std::atomic<bool> shutting_down_{false};
    std::mutex probe_mu_;
    std::condition_variable probe_cv_;
    bool probe_stop_ = false;
    bool probe_worker_running_ = false;
    bool has_pending_probe_ = false;
    ProbeKind pending_probe_kind_ = ProbeKind::Health;
    uint64_t pending_probe_gen_ = 0;
    int pending_probe_port_ = 18081;
    std::thread probe_thread_;

    QProcess *download_proc_ = nullptr;
    QString downloading_variant_;
    QString download_dest_path_;
    QString download_part_path_;
    QByteArray download_stderr_buf_;
    bool download_cancelled_ = false;
    bool download_is_tar_ = false;
};

} // namespace shadowbar::floating_ui
