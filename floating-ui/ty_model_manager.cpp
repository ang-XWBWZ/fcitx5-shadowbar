#include "ty_model_manager.hpp"

#include "shadowbar/translator.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <atomic>
#include <limits>

namespace shadowbar::floating_ui {

namespace {

constexpr int kMaxLogLines = 80;
constexpr int kStartupTimeoutMs = 30000;
constexpr int kHealthPollMs = 150;
constexpr int kProbeWorkerIdleMs = 5000;

} // namespace

TyModelManager::TyModelManager(QObject *parent) : QObject(parent) {
    health_timer_ = new QTimer(this);
    health_timer_->setInterval(kHealthPollMs);
    connect(health_timer_, &QTimer::timeout, this,
            &TyModelManager::pollStartupHealth);

    startup_timeout_timer_ = new QTimer(this);
    startup_timeout_timer_->setSingleShot(true);
    startup_timeout_timer_->setInterval(kStartupTimeoutMs);
    connect(startup_timeout_timer_, &QTimer::timeout, this, [this]() {
        if (state_ == ServerState::Starting) {
            if (tryFallbackToCpu(QStringLiteral("Vulkan 启动超时"))) {
                return;
            }
            appendLog(QStringLiteral("[ty] 启动超时，未能连接到本地推理服务"));
            stopServer();
            setState(ServerState::Error, QStringLiteral("启动超时（30s 内未就绪）"));
        }
    });

    idle_timer_ = new QTimer(this);
    idle_timer_->setSingleShot(true);
    connect(idle_timer_, &QTimer::timeout, this,
            &TyModelManager::onIdleTimerFired);
}

TyModelManager::~TyModelManager() {
    shutdown();
}

void TyModelManager::shutdown() {
    shutting_down_.store(true, std::memory_order_release);
    if (health_timer_) {
        health_timer_->stop();
    }
    if (startup_timeout_timer_) {
        startup_timeout_timer_->stop();
    }
    if (idle_timer_) {
        idle_timer_->stop();
    }
    cancelDownload();
    stopServer();
    joinProbeThread();
}

void TyModelManager::joinProbeThread() {
    std::thread to_join;
    {
        std::lock_guard<std::mutex> lock(probe_mu_);
        probe_stop_ = true;
        has_pending_probe_ = false;
        probe_worker_running_ = false;
        to_join = std::move(probe_thread_);
    }
    probe_cv_.notify_all();
    if (to_join.joinable()) {
        to_join.join();
    }
}

bool TyModelManager::isDownloading() const {
    return download_proc_ && download_proc_->state() != QProcess::NotRunning;
}

bool TyModelManager::isManagedProcessRunning() const {
    return server_proc_ && server_proc_->state() != QProcess::NotRunning;
}

bool TyModelManager::isReadyFor(const QString &variant,
                                const QString &model_path,
                                int port,
                                const QString &backend_mode) const {
    if (port != active_port_) {
        return false;
    }
    if (state_ == ServerState::ExternalRunning) {
        return true;
    }
    if (state_ != ServerState::Running || !isManagedProcessRunning()) {
        return false;
    }
    if (!backend_mode.trimmed().isEmpty() &&
        active_backend_mode_ != normalizeTyBackend(backend_mode)) {
        return false;
    }
    const QString norm_v = normalizeTyVariant(variant);
    if (!variant.trimmed().isEmpty() && active_variant_ != norm_v) {
        return false;
    }
    if (!model_path.trimmed().isEmpty() &&
        QFileInfo(active_model_path_).canonicalFilePath() !=
            QFileInfo(model_path).canonicalFilePath()) {
        return false;
    }
    return true;
}

void TyModelManager::setState(ServerState next, const QString &msg) {
    state_ = next;
    status_message_ = msg;
    emit stateChanged(next, msg);
}

void TyModelManager::appendLog(const QString &line) {
    const QString clean = line.trimmed();
    if (clean.isEmpty()) {
        return;
    }
    log_lines_.append(clean);
    while (log_lines_.size() > kMaxLogLines) {
        log_lines_.removeFirst();
    }
    emit logAppended(clean);
}

bool TyModelManager::startServer(const QString &binary_path,
                                 const QString &variant,
                                 const QString &model_path,
                                 int port,
                                 int threads,
                                 const QString &backend_mode) {
    const QString norm_variant = normalizeTyVariant(variant);
    const QString norm_backend = normalizeTyBackend(backend_mode);
    const int clamped_port = std::clamp(port, 1024, 65535);
    const int clamped_threads = std::clamp(threads, 0, 256);

    const QFileInfo bin_info(binary_path.trimmed());
    if (binary_path.trimmed().isEmpty() || !bin_info.exists() ||
        !bin_info.isExecutable()) {
        setState(ServerState::Error,
                 QStringLiteral("未找到可执行 llama-server: %1").arg(binary_path));
        return false;
    }
    const QFileInfo model_info(model_path.trimmed());
    if (model_path.trimmed().isEmpty() || !model_info.exists() ||
        model_info.size() < (10LL * 1024 * 1024)) {
        setState(ServerState::Error,
                 QStringLiteral("模型文件不存在或未下载完成: %1").arg(model_path));
        return false;
    }

    if ((state_ == ServerState::Running || state_ == ServerState::Starting) &&
        isManagedProcessRunning() && active_port_ == clamped_port &&
        active_variant_ == norm_variant &&
        active_threads_ == clamped_threads &&
        active_backend_mode_ == norm_backend &&
        QFileInfo(active_model_path_).canonicalFilePath() ==
            model_info.canonicalFilePath() &&
        QFileInfo(active_binary_path_).canonicalFilePath() ==
            bin_info.canonicalFilePath()) {
        markActivity();
        return true;
    }

    stopServer();

    active_variant_ = norm_variant;
    active_model_path_ = model_info.absoluteFilePath();
    active_binary_path_ = bin_info.absoluteFilePath();
    active_backend_mode_ = norm_backend;
    active_port_ = clamped_port;
    active_threads_ = clamped_threads;
    fallback_attempted_ = false;
    spawnServerProcess(tyBackendGpuLayers(active_backend_mode_));
    return true;
}

void TyModelManager::spawnServerProcess(int gpu_layers) {
    stopServerProcessOnly();
    server_stdout_buf_.clear();
    active_gpu_layers_ = std::max(0, gpu_layers);
    vulkan_device_seen_ = false;
    using_vulkan_ = false;
    no_vulkan_seen_ = false;
    unsupported_format_seen_ = false;
    offloaded_layers_ = 0;
    total_layers_ = 0;
    ++probe_generation_;

    server_proc_ = new QProcess(this);
    server_proc_->setProcessChannelMode(QProcess::MergedChannels);

    connect(server_proc_, &QProcess::readyReadStandardOutput, this, [this]() {
        if (!server_proc_) {
            return;
        }
        server_stdout_buf_.append(server_proc_->readAllStandardOutput());
        if (server_stdout_buf_.size() > 65536) {
            server_stdout_buf_ = server_stdout_buf_.right(32768);
        }
        qsizetype pos = 0;
        QStringList lines;
        while (true) {
            const qsizetype nl = server_stdout_buf_.indexOf('\n', pos);
            if (nl == -1) {
                break;
            }
            qsizetype len = nl - pos;
            if (len > 0 && server_stdout_buf_[pos + len - 1] == '\r') {
                --len;
            }
            const QString decoded =
                QString::fromUtf8(server_stdout_buf_.constData() + pos, len)
                    .trimmed();
            if (!decoded.isEmpty()) {
                lines.append(decoded);
            }
            pos = nl + 1;
        }
        if (pos > 0) {
            server_stdout_buf_.remove(0, pos);
        }
        const bool check_startup_signals = (state_ == ServerState::Starting);
        QString fallback_reason;
        for (const QString &l : lines) {
            if (check_startup_signals) {
                const TyServerLogSignal sig = inspectTyServerLogLine(l);
                if (sig.vulkan_device_found) {
                    vulkan_device_seen_ = true;
                }
                if (sig.unsupported_model_format) {
                    unsupported_format_seen_ = true;
                }
                if (sig.no_vulkan_device) {
                    no_vulkan_seen_ = true;
                    if (active_gpu_layers_ > 0 && fallback_reason.isEmpty()) {
                        fallback_reason = QStringLiteral("未检测到可用 Vulkan GPU 设备");
                    }
                }
                if (sig.has_layer_offload) {
                    offloaded_layers_ = sig.offloaded_layers;
                    total_layers_ = sig.total_layers;
                    if (sig.offloaded_layers > 0) {
                        using_vulkan_ = true;
                    }
                    if (active_gpu_layers_ > 0 && sig.total_layers > 0 &&
                        sig.offloaded_layers < sig.total_layers &&
                        fallback_reason.isEmpty()) {
                        fallback_reason =
                            QStringLiteral("Vulkan 仅部分卸载 (%1/%2 层)")
                                .arg(sig.offloaded_layers)
                                .arg(sig.total_layers);
                    }
                }
            }
            appendLog(l);
        }
        if (!fallback_reason.isEmpty() &&
            active_backend_mode_ == QLatin1String("auto") &&
            active_gpu_layers_ > 0 && !fallback_attempted_) {
            if (tryFallbackToCpu(fallback_reason)) {
                return;
            }
        }
    });

    connect(server_proc_,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exit_code, QProcess::ExitStatus exit_status) {
                if (health_timer_) {
                    health_timer_->stop();
                }
                if (startup_timeout_timer_) {
                    startup_timeout_timer_->stop();
                }
                if (server_proc_) {
                    server_proc_->deleteLater();
                    server_proc_ = nullptr;
                }
                if (state_ == ServerState::Stopped) {
                    return;
                }
                if (unsupported_format_seen_) {
                    const QString msg = QStringLiteral(
                        "当前 llama-server 不支持 Hy-MT2 的 Q2_0C / hunyuan-dense 格式"
                        "（需 PR #22836 / commit 1e411d8），请点击「⬇ 下载引擎」"
                        "或运行 scripts/build_llama_server.sh 编译专版引擎");
                    appendLog(QStringLiteral("[ty] ") + msg);
                    setState(ServerState::Error, msg);
                    return;
                }
                if (state_ == ServerState::Starting &&
                    tryFallbackToCpu(QStringLiteral("进程退出 code %1").arg(exit_code))) {
                    return;
                }
                const QString msg =
                    (exit_status == QProcess::CrashExit)
                        ? QStringLiteral("llama-server 异常崩溃 (code %1)").arg(exit_code)
                        : QStringLiteral("llama-server 已退出 (code %1)").arg(exit_code);
                appendLog(QStringLiteral("[ty] ") + msg);
                setState(exit_code == 0 ? ServerState::Stopped : ServerState::Error,
                         msg);
            });

    connect(server_proc_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError err) {
                if (state_ == ServerState::Stopped) {
                    return;
                }
                if (err == QProcess::FailedToStart) {
                    if (health_timer_) {
                        health_timer_->stop();
                    }
                    if (startup_timeout_timer_) {
                        startup_timeout_timer_->stop();
                    }
                    const QString msg = QStringLiteral("无法启动 llama-server");
                    appendLog(QStringLiteral("[ty] ") + msg);
                    setState(ServerState::Error, msg);
                }
            });

    const QStringList args = buildTyServerArgs(
        active_model_path_, active_port_, active_threads_, 2048,
        active_gpu_layers_);
    const QString mode_tag =
        (active_gpu_layers_ > 0)
            ? QStringLiteral("首选 Vulkan")
            : (fallback_attempted_ ? QStringLiteral("CPU 回退")
                                   : QStringLiteral("CPU"));
    appendLog(QStringLiteral("[ty] 启动 (%1): %2 %3")
                  .arg(mode_tag, active_binary_path_,
                       args.join(QLatin1Char(' '))));
    setState(ServerState::Starting,
             QStringLiteral("正在加载 %1 模型 (%2 · %3)...")
                 .arg(active_variant_, mode_tag)
                 .arg(active_port_));

    server_proc_->start(active_binary_path_, args);
    health_timer_->start();
    startup_timeout_timer_->start();
    pollStartupHealth();
}

bool TyModelManager::tryFallbackToCpu(const QString &reason) {
    if (active_backend_mode_ != QLatin1String("auto") ||
        active_gpu_layers_ <= 0 || fallback_attempted_) {
        return false;
    }
    fallback_attempted_ = true;
    appendLog(
        QStringLiteral("[ty] Vulkan 不可用或启动失败 (%1)，自动回退 CPU 推理 (-ngl 0)")
            .arg(reason));
    spawnServerProcess(0);
    return true;
}

void TyModelManager::stopServerProcessOnly() {
    if (health_timer_) {
        health_timer_->stop();
    }
    if (startup_timeout_timer_) {
        startup_timeout_timer_->stop();
    }
    if (idle_timer_) {
        idle_timer_->stop();
    }
    ++probe_generation_;

    if (server_proc_) {
        QProcess *proc = server_proc_;
        server_proc_ = nullptr;
        proc->disconnect(this);
        if (proc->state() != QProcess::NotRunning) {
            proc->terminate();
            if (!proc->waitForFinished(2500)) {
                proc->kill();
                proc->waitForFinished(1000);
            }
        }
        proc->deleteLater();
    }
}

void TyModelManager::stopServer() {
    const bool had_proc = (server_proc_ != nullptr);
    stopServerProcessOnly();
    if (had_proc) {
        appendLog(QStringLiteral("[ty] 已停止本地 llama-server"));
    }
    if (state_ != ServerState::Stopped) {
        setState(ServerState::Stopped, QStringLiteral("未运行"));
    }
}

void TyModelManager::pollStartupHealth() {
    if (state_ != ServerState::Starting || !isManagedProcessRunning()) {
        if (health_timer_) {
            health_timer_->stop();
        }
        return;
    }
    enqueueProbe(ProbeKind::Health, probe_generation_, active_port_);
}

void TyModelManager::probeHealthOnce(int port) {
    active_port_ = std::clamp(port, 1024, 65535);
    enqueueProbe(ProbeKind::Health, probe_generation_, active_port_);
}

void TyModelManager::enqueueProbe(ProbeKind kind,
                                  uint64_t generation,
                                  int port) {
    if (shutting_down_.load(std::memory_order_acquire)) {
        return;
    }
    std::thread retired_thread;
    {
        std::lock_guard<std::mutex> lock(probe_mu_);
        if (probe_stop_) {
            return;
        }
        pending_probe_kind_ = kind;
        pending_probe_gen_ = generation;
        pending_probe_port_ = port;
        has_pending_probe_ = true;
        if (!probe_worker_running_) {
            if (probe_thread_.joinable()) {
                retired_thread = std::move(probe_thread_);
            }
            probe_worker_running_ = true;
            probe_thread_ = std::thread([this]() { probeWorkerLoop(); });
        }
    }
    if (retired_thread.joinable()) {
        retired_thread.join();
    }
    probe_cv_.notify_one();
}

void TyModelManager::probeWorkerLoop() {
    std::unique_lock<std::mutex> lock(probe_mu_);
    while (!probe_stop_ && !shutting_down_.load(std::memory_order_acquire)) {
        if (!has_pending_probe_) {
            const bool woke = probe_cv_.wait_for(
                lock, std::chrono::milliseconds(kProbeWorkerIdleMs),
                [this]() {
                    return probe_stop_ || has_pending_probe_ ||
                           shutting_down_.load(std::memory_order_acquire);
                });
            if (!woke && !has_pending_probe_) {
                probe_worker_running_ = false;
                return;
            }
            continue;
        }

        const ProbeKind kind = pending_probe_kind_;
        const uint64_t generation = pending_probe_gen_;
        const int port = pending_probe_port_;
        has_pending_probe_ = false;
        lock.unlock();

        bool result_flag = false;
        if (!shutting_down_.load(std::memory_order_acquire)) {
            std::atomic<bool> cancel{false};
            HttpRequest req;
            req.method = "GET";
            req.url = (kind == ProbeKind::Slots)
                          ? tySlotsEndpoint(port).toStdString()
                          : tyHealthEndpoint(port).toStdString();
            req.timeout_ms = 550;
            const HttpResponse resp = curlHttpTransport(req, cancel, {});
            if (kind == ProbeKind::Slots) {
                result_flag = (resp.status_code == 200) &&
                              parseLlamaSlotsBusy(QByteArray::fromStdString(resp.body));
            } else {
                result_flag = (resp.status_code == 200);
            }
        }

        if (!shutting_down_.load(std::memory_order_acquire)) {
            QMetaObject::invokeMethod(
                this,
                [this, kind, generation, port, result_flag]() {
                    if (kind == ProbeKind::Slots) {
                        handleSlotsProbeResult(generation, port, result_flag);
                    } else {
                        handleHealthProbeResult(generation, port, result_flag);
                    }
                },
                Qt::QueuedConnection);
        }

        lock.lock();
    }
    probe_worker_running_ = false;
}

void TyModelManager::handleHealthProbeResult(uint64_t generation,
                                             int port,
                                             bool healthy) {
    if (generation != probe_generation_ || port != active_port_) {
        return;
    }
    if (state_ == ServerState::Starting && isManagedProcessRunning()) {
        if (healthy) {
            if (health_timer_) {
                health_timer_->stop();
            }
            if (startup_timeout_timer_) {
                startup_timeout_timer_->stop();
            }
            QString backend_tag;
            if (active_gpu_layers_ > 0 && using_vulkan_) {
                if (total_layers_ > 0 && offloaded_layers_ == total_layers_) {
                    backend_tag = QStringLiteral("Vulkan (%1/%2层)")
                                      .arg(offloaded_layers_)
                                      .arg(total_layers_);
                } else if (total_layers_ > 0) {
                    backend_tag = QStringLiteral("Vulkan 部分卸载 (%1/%2层) + CPU")
                                      .arg(offloaded_layers_)
                                      .arg(total_layers_);
                } else {
                    backend_tag = QStringLiteral("Vulkan GPU");
                }
            } else if (active_gpu_layers_ > 0 && vulkan_device_seen_) {
                backend_tag = QStringLiteral("Vulkan 已发现 / CPU 算子回退");
            } else if (active_gpu_layers_ > 0 &&
                       active_backend_mode_ == QLatin1String("vulkan") &&
                       no_vulkan_seen_) {
                backend_tag = QStringLiteral("Vulkan 不可用 · CPU");
            } else if (active_gpu_layers_ == 0 && fallback_attempted_) {
                backend_tag = QStringLiteral("CPU 回退");
            } else if (active_gpu_layers_ == 0) {
                backend_tag = QStringLiteral("CPU");
            } else {
                backend_tag = QStringLiteral("自动后端");
            }
            const QString msg = QStringLiteral("运行中 · %1 · %2 @ 127.0.0.1:%3")
                                    .arg(active_variant_, backend_tag)
                                    .arg(active_port_);
            appendLog(QStringLiteral("[ty] 就绪: ") + msg);
            last_activity_timer_.restart();
            setState(ServerState::Running, msg);
            scheduleIdleUnloadTimer();
        }
        return;
    }
    if (!isManagedProcessRunning()) {
        if (healthy) {
            setState(ServerState::ExternalRunning,
                     QStringLiteral("检测到外部服务正在 127.0.0.1:%1 运行")
                         .arg(port));
        } else if (state_ == ServerState::ExternalRunning) {
            setState(ServerState::Stopped, QStringLiteral("未运行"));
        }
    }
}

void TyModelManager::setIdleUnloadSeconds(int seconds) {
    const int clamped = std::clamp(seconds, 0, 86400);
    if (idle_unload_sec_ == clamped) {
        return;
    }
    idle_unload_sec_ = clamped;
    if (idle_unload_sec_ <= 0) {
        if (idle_timer_) {
            idle_timer_->stop();
        }
        return;
    }
    if (state_ == ServerState::Running && isManagedProcessRunning() &&
        active_requests_ == 0) {
        const qint64 elapsed_ms =
            last_activity_timer_.isValid() ? last_activity_timer_.elapsed() : 0;
        const qint64 threshold_ms =
            static_cast<qint64>(idle_unload_sec_) * 1000LL;
        const qint64 remaining = std::max<qint64>(200, threshold_ms - elapsed_ms);
        scheduleIdleUnloadTimer(remaining);
    }
}

void TyModelManager::markActivity() {
    last_activity_timer_.restart();
    if (state_ == ServerState::Running && isManagedProcessRunning() &&
        idle_unload_sec_ > 0 && active_requests_ == 0) {
        scheduleIdleUnloadTimer();
    }
}

void TyModelManager::beginTranslationActivity() {
    ++active_requests_;
    last_activity_timer_.restart();
    if (idle_timer_) {
        idle_timer_->stop();
    }
}

void TyModelManager::endTranslationActivity() {
    if (active_requests_ > 0) {
        --active_requests_;
    }
    markActivity();
}

void TyModelManager::scheduleIdleUnloadTimer(qint64 delay_ms) {
    if (!idle_timer_) {
        return;
    }
    if (idle_unload_sec_ <= 0 || state_ != ServerState::Running ||
        !isManagedProcessRunning() || active_requests_ > 0) {
        idle_timer_->stop();
        return;
    }
    const qint64 threshold_ms = static_cast<qint64>(idle_unload_sec_) * 1000LL;
    const qint64 wait_ms =
        (delay_ms >= 0) ? std::max<qint64>(100, delay_ms) : threshold_ms;
    idle_timer_->start(static_cast<int>(
        std::min<qint64>(wait_ms, std::numeric_limits<int>::max())));
}

void TyModelManager::onIdleTimerFired() {
    if (state_ != ServerState::Running || !isManagedProcessRunning() ||
        idle_unload_sec_ <= 0 || active_requests_ > 0) {
        return;
    }
    const qint64 threshold_ms = static_cast<qint64>(idle_unload_sec_) * 1000LL;
    const qint64 local_elapsed =
        last_activity_timer_.isValid() ? last_activity_timer_.elapsed() : 0;
    if (local_elapsed < threshold_ms) {
        scheduleIdleUnloadTimer(threshold_ms - local_elapsed + 100);
        return;
    }
    const qint64 ext_age_ms = readExternalHyMtActivityAgeMs();
    if (ext_age_ms >= 0 && ext_age_ms < threshold_ms) {
        scheduleIdleUnloadTimer(threshold_ms - ext_age_ms + 500);
        return;
    }
    enqueueProbe(ProbeKind::Slots, probe_generation_, active_port_);
}

void TyModelManager::handleSlotsProbeResult(uint64_t generation,
                                            int port,
                                            bool busy) {
    if (generation != probe_generation_ || port != active_port_ ||
        state_ != ServerState::Running || !isManagedProcessRunning() ||
        idle_unload_sec_ <= 0 || active_requests_ > 0) {
        return;
    }
    if (busy) {
        markActivity();
        scheduleIdleUnloadTimer(
            std::min<qint64>(static_cast<qint64>(idle_unload_sec_) * 1000LL, 30000));
        return;
    }
    const qint64 threshold_ms = static_cast<qint64>(idle_unload_sec_) * 1000LL;
    const qint64 local_elapsed =
        last_activity_timer_.isValid() ? last_activity_timer_.elapsed() : 0;
    const qint64 ext_age_ms = readExternalHyMtActivityAgeMs();
    if (!shouldUnloadIdleTyModel(isManagedProcessRunning(), idle_unload_sec_,
                                 local_elapsed, active_requests_) ||
        (ext_age_ms >= 0 && ext_age_ms < threshold_ms)) {
        scheduleIdleUnloadTimer();
        return;
    }
    unloadForIdle();
}

void TyModelManager::unloadForIdle() {
    if (state_ != ServerState::Running || !isManagedProcessRunning()) {
        return;
    }
    const QString variant = active_variant_;
    stopServerProcessOnly();
    const int mins = std::max(1, (idle_unload_sec_ + 30) / 60);
    appendLog(QStringLiteral("[ty] 连续 %1 分钟未激活，已自动卸载 %2 模型并释放内存/显存")
                  .arg(mins)
                  .arg(variant));
    setState(ServerState::Stopped,
             QStringLiteral("空闲已卸载 (%1 · 下次使用自动重装)").arg(variant));
}

bool TyModelManager::startDownload(const QString &variant,
                                   const QString &mirror,
                                   const QString &dest_path) {
    const QString norm_variant = normalizeTyVariant(variant);
    const QString url = tyDownloadUrl(norm_variant, mirror);
    if (url.isEmpty() || dest_path.trimmed().isEmpty()) {
        emit downloadFinished(norm_variant, false,
                              QStringLiteral("该变体不支持内置在线下载"));
        return false;
    }
    return startCurlDownload(norm_variant, url, dest_path, false);
}

bool TyModelManager::startEngineDownload(const QString &mirror,
                                         const QString &dest_bin_path) {
    const QString dest = dest_bin_path.trimmed().isEmpty()
                             ? defaultTyServerInstallPath()
                             : dest_bin_path.trimmed();
    const QString url = tyEngineDownloadUrl(mirror);
    return startCurlDownload(QStringLiteral("llama-server"), url, dest, true);
}

bool TyModelManager::startCurlDownload(const QString &tag,
                                       const QString &url,
                                       const QString &dest_path,
                                       bool extract_engine_tar) {
    if (isDownloading()) {
        return false;
    }

    const QString curl_bin =
        QStandardPaths::findExecutable(QStringLiteral("curl"));
    if (curl_bin.isEmpty()) {
        emit downloadFinished(tag, false, QStringLiteral("系统未找到 curl 命令"));
        return false;
    }

    const QFileInfo dest_info(dest_path.trimmed());
    QDir().mkpath(dest_info.absolutePath());

    downloading_variant_ = tag;
    download_dest_path_ = dest_info.absoluteFilePath();
    download_part_path_ = download_dest_path_ +
                          (extract_engine_tar ? QStringLiteral(".tar.gz.part")
                                              : QStringLiteral(".part"));
    download_stderr_buf_.clear();
    download_cancelled_ = false;
    download_is_tar_ = extract_engine_tar;
    QFile::remove(download_part_path_);

    download_proc_ = new QProcess(this);
    connect(download_proc_, &QProcess::readyReadStandardError, this, [this]() {
        if (!download_proc_) {
            return;
        }
        download_stderr_buf_ += download_proc_->readAllStandardError();
        static const QRegularExpression kPctRe(QStringLiteral("([0-9]+(?:\\.[0-9]+)?)%"));
        const QString text = QString::fromUtf8(download_stderr_buf_);
        auto it = kPctRe.globalMatch(text);
        int last_pct = -1;
        while (it.hasNext()) {
            const auto match = it.next();
            bool ok = false;
            const double val = match.captured(1).toDouble(&ok);
            if (ok) {
                last_pct = std::clamp(static_cast<int>(val + 0.5), 0, 100);
            }
        }
        if (last_pct >= 0) {
            emit downloadProgress(
                downloading_variant_, last_pct,
                QStringLiteral("正在下载 %1 (%2%)...").arg(downloading_variant_).arg(last_pct));
        }
        if (download_stderr_buf_.size() > 2048) {
            download_stderr_buf_ = download_stderr_buf_.right(512);
        }
    });

    connect(download_proc_,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exit_code, QProcess::ExitStatus) {
                QProcess *proc = download_proc_;
                download_proc_ = nullptr;
                if (proc) {
                    proc->deleteLater();
                }

                const QString variant_done = downloading_variant_;
                const bool is_tar = download_is_tar_;
                downloading_variant_.clear();
                download_is_tar_ = false;

                if (download_cancelled_) {
                    QFile::remove(download_part_path_);
                    emit downloadFinished(variant_done, false,
                                          QStringLiteral("已取消下载"));
                    return;
                }
                if (exit_code != 0) {
                    const QString err_tail =
                        QString::fromUtf8(download_stderr_buf_).trimmed();
                    QFile::remove(download_part_path_);
                    const QString hint =
                        is_tar
                            ? QStringLiteral("下载引擎失败 (curl %1；若未联网或 GitHub 受阻，可运行 scripts/build_llama_server.sh 本地编译): %2")
                            : QStringLiteral("下载失败 (curl %1): %2");
                    emit downloadFinished(variant_done, false,
                                          hint.arg(exit_code).arg(err_tail.right(180)));
                    return;
                }

                if (is_tar) {
                    const QString unpack_dir =
                        QFileInfo(download_dest_path_).absolutePath() +
                        QStringLiteral("/.engine_unpack_tmp");
                    QDir(unpack_dir).removeRecursively();
                    QDir().mkpath(unpack_dir);

                    QProcess tar;
                    tar.start(QStringLiteral("tar"),
                              {QStringLiteral("-xzf"), download_part_path_,
                               QStringLiteral("-C"), unpack_dir});
                    const bool tar_ok = tar.waitForFinished(30000) &&
                                        tar.exitStatus() == QProcess::NormalExit &&
                                        tar.exitCode() == 0;
                    QFile::remove(download_part_path_);
                    if (!tar_ok) {
                        QDir(unpack_dir).removeRecursively();
                        emit downloadFinished(
                            variant_done, false,
                            QStringLiteral("解压引擎压缩包失败"));
                        return;
                    }

                    QString extracted_bin;
                    QDirIterator it(unpack_dir,
                                    {QStringLiteral("llama-server")},
                                    QDir::Files,
                                    QDirIterator::Subdirectories);
                    if (it.hasNext()) {
                        extracted_bin = it.next();
                    }
                    if (extracted_bin.isEmpty()) {
                        QDir(unpack_dir).removeRecursively();
                        emit downloadFinished(
                            variant_done, false,
                            QStringLiteral("压缩包内未找到 llama-server 可执行文件"));
                        return;
                    }
                    if (QFile::exists(download_dest_path_)) {
                        QFile::remove(download_dest_path_);
                    }
                    const bool moved =
                        QFile::rename(extracted_bin, download_dest_path_) ||
                        QFile::copy(extracted_bin, download_dest_path_);
                    QDir(unpack_dir).removeRecursively();
                    if (!moved) {
                        emit downloadFinished(
                            variant_done, false,
                            QStringLiteral("安装 llama-server 到目标目录失败"));
                        return;
                    }
                    QFile::setPermissions(
                        download_dest_path_,
                        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                            QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                            QFileDevice::ExeGroup | QFileDevice::ReadOther |
                            QFileDevice::ExeOther);
                } else {
                    if (QFile::exists(download_dest_path_)) {
                        QFile::remove(download_dest_path_);
                    }
                    if (!QFile::rename(download_part_path_, download_dest_path_)) {
                        emit downloadFinished(variant_done, false,
                                              QStringLiteral("保存模型文件失败"));
                        return;
                    }
                }

                const QFileInfo fi(download_dest_path_);
                emit downloadProgress(variant_done, 100, QStringLiteral("安装完成"));
                emit downloadFinished(
                    variant_done, true,
                    QStringLiteral("%1完成 (%2 MB)")
                        .arg(is_tar ? QStringLiteral("引擎安装") : QStringLiteral("下载"))
                        .arg(static_cast<double>(fi.size()) / (1024.0 * 1024.0),
                             0, 'f', 1));
            });

    emit downloadProgress(downloading_variant_, 0,
                          QStringLiteral("连接下载源..."));
    appendLog(QStringLiteral("[ty] 开始下载 %1 -> %2")
                  .arg(url, download_dest_path_));
    download_proc_->start(
        curl_bin,
        {QStringLiteral("-L"), QStringLiteral("--progress-bar"),
         QStringLiteral("--fail"), QStringLiteral("--connect-timeout"),
         QStringLiteral("15"), QStringLiteral("-o"), download_part_path_, url});
    return true;
}

void TyModelManager::cancelDownload() {
    if (!download_proc_) {
        return;
    }
    download_cancelled_ = true;
    QProcess *proc = download_proc_;
    download_proc_ = nullptr;
    proc->disconnect(this);
    if (proc->state() != QProcess::NotRunning) {
        proc->terminate();
        if (!proc->waitForFinished(1500)) {
            proc->kill();
            proc->waitForFinished(1000);
        }
    }
    proc->deleteLater();
    QFile::remove(download_part_path_);
    const QString cancelled_variant = downloading_variant_;
    downloading_variant_.clear();
    if (!shutting_down_.load(std::memory_order_acquire) &&
        !cancelled_variant.isEmpty()) {
        emit downloadFinished(cancelled_variant, false,
                              QStringLiteral("已取消下载"));
    }
}

} // namespace shadowbar::floating_ui
