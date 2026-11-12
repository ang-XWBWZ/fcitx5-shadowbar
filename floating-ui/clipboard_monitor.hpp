#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;
class QTimer;

namespace shadowbar::floating_ui {

enum class ClipboardAvoidReason {
    None,
    Empty,
    Sensitive,
    FileOrPath,
    UrlOrLink,
    BinaryOrNonText,
};

/// Evaluates MIME types reported by X11/Wayland to avoid password hints, file transfers, link payloads, or non-text binary blobs.
ClipboardAvoidReason classifyAvoidedMimeTypes(const QStringList &formats);

/// Evaluates plain clipboard text to avoid file URIs, local filesystem paths, URLs, or markdown links while allowing normal prose.
ClipboardAvoidReason classifyAvoidedClipboardText(const QString &text);

/// Human-readable status hint explaining why clipboard content was skipped.
QString clipboardAvoidReasonText(ClipboardAvoidReason reason);

/**
 * Watches system clipboard changes even while the window lacks focus on Wayland.
 * Combines `QClipboard::dataChanged` with `wl-paste --watch` (`ext_data_control` / `zwlr_data_control`)
 * and KDE Klipper D-Bus `clipboardHistoryUpdated` notifications, using non-blocking async reads.
 */
class ClipboardMonitor final : public QObject {
    Q_OBJECT

public:
    explicit ClipboardMonitor(QObject *parent = nullptr);
    ~ClipboardMonitor() override;

    void setWatchEnabled(bool enabled);
    bool isWatchEnabled() const { return watch_enabled_; }

    /// Triggers a one-shot async clipboard probe (e.g. when card opens or manual read button clicked).
    void requestProbe(bool force);

    /// Records text recently copied by ShadowBar itself so background watch does not loop back on it.
    void markSelfCopiedText(const QString &text);

signals:
    void textReady(const QString &text, bool forced);
    void clipboardAvoided(ClipboardAvoidReason reason, bool forced);

private slots:
    void onQtClipboardChanged();
    void onExternalChangeSignal();
    void executePendingProbe();

private:
    void startWaylandWatch();
    void stopWaylandWatch();
    bool tryReadFromQtClipboard(bool force);
    void startAsyncWlPasteListTypes(bool force);
    void startAsyncWlPasteReadText(const QString &mime_type, bool force);
    void startAsyncKlipperFallback(bool force);
    void finishProbeFlight();

    bool is_wayland_ = false;
    bool watch_enabled_ = false;
    QString wl_paste_bin_;
    QProcess *wl_watch_proc_ = nullptr;
    QProcess *wl_probe_proc_ = nullptr;
    QByteArray wl_watch_buf_;
    QElapsedTimer watch_started_timer_;
    QTimer *debounce_timer_ = nullptr;
    bool pending_force_ = false;
    bool probe_in_flight_ = false;
    bool rerun_after_flight_ = false;
    QString last_self_copied_;
    bool klipper_connected_ = false;
};

} // namespace shadowbar::floating_ui
