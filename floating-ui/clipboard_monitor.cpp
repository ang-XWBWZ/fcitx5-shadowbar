#include "clipboard_monitor.hpp"

#include <QClipboard>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QGuiApplication>
#include <QMimeData>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <algorithm>

namespace shadowbar::floating_ui {

namespace {

bool isPlainTextMime(const QString &lower_mime) {
    return lower_mime == "text/plain" ||
           lower_mime.startsWith("text/plain;") ||
           lower_mime == "utf8_string" ||
           lower_mime == "string" ||
           lower_mime == "text";
}

QString pickBestPlainTextMime(const QStringList &formats) {
    for (const char *preferred :
         {"text/plain;charset=utf-8", "text/plain;charset=UTF-8",
          "UTF8_STRING", "text/plain"}) {
        for (const QString &f : formats) {
            if (f.compare(QLatin1String(preferred), Qt::CaseInsensitive) == 0) {
                return f;
            }
        }
    }
    for (const QString &f : formats) {
        if (f.trimmed().toLower().startsWith("text/plain")) {
            return f.trimmed();
        }
    }
    return {};
}

bool isFileScheme(const QString &lower_line) {
    return lower_line.startsWith("file://") ||
           lower_line.startsWith("trash:/") ||
           lower_line.startsWith("smb://") ||
           lower_line.startsWith("sftp://") ||
           lower_line.startsWith("ftp://") ||
           lower_line.startsWith("ftps://") ||
           lower_line.startsWith("mtp://") ||
           lower_line.startsWith("afc://") ||
           lower_line.startsWith("dav://") ||
           lower_line.startsWith("davs://") ||
           lower_line.startsWith("applications:/") ||
           lower_line.startsWith("desktop:/");
}

bool isWebOrAppUrlScheme(const QString &lower_line) {
    return lower_line.startsWith("http://") ||
           lower_line.startsWith("https://") ||
           lower_line.startsWith("ws://") ||
           lower_line.startsWith("wss://") ||
           lower_line.startsWith("magnet:") ||
           lower_line.startsWith("mailto:") ||
           lower_line.startsWith("tg://") ||
           lower_line.startsWith("slack://") ||
           lower_line.startsWith("vscode://");
}

ClipboardAvoidReason classifySingleLineToken(const QString &raw_line) {
    QString line = raw_line.trimmed();
    if (line.isEmpty()) {
        return ClipboardAvoidReason::Empty;
    }

    // Unwrap matching <url>, "url", or 'url'
    if (line.size() >= 3 &&
        ((line.startsWith('<') && line.endsWith('>')) ||
         (line.startsWith('"') && line.endsWith('"')) ||
         (line.startsWith('\'') && line.endsWith('\'')))) {
        line = line.mid(1, line.size() - 2).trimmed();
    }

    // Markdown link [label](url)
    static const QRegularExpression kMdLinkRe(
        QStringLiteral(R"(^!?\[[^\]]*\]\(([^()\s]+)\)$)"));
    if (const auto m = kMdLinkRe.match(line); m.hasMatch()) {
        const QString inner_lower = m.captured(1).toLower();
        if (isFileScheme(inner_lower)) {
            return ClipboardAvoidReason::FileOrPath;
        }
        if (isWebOrAppUrlScheme(inner_lower) || inner_lower.startsWith('/')) {
            return isWebOrAppUrlScheme(inner_lower)
                       ? ClipboardAvoidReason::UrlOrLink
                       : ClipboardAvoidReason::FileOrPath;
        }
    }

    const QString lower = line.toLower();
    const bool has_whitespace = std::any_of(
        line.begin(), line.end(), [](QChar ch) { return ch.isSpace(); });

    if (isFileScheme(lower)) {
        return ClipboardAvoidReason::FileOrPath;
    }

    if (!has_whitespace && isWebOrAppUrlScheme(lower)) {
        return ClipboardAvoidReason::UrlOrLink;
    }

    // Home/relative paths & Windows drive paths
    if (line.startsWith("~/") || line == "~" ||
        line.startsWith("./") || line.startsWith("../")) {
        return ClipboardAvoidReason::FileOrPath;
    }
    static const QRegularExpression kWinDriveRe(
        QStringLiteral(R"(^[A-Za-z]:[/\\][^\r\n]+$)"));
    if (kWinDriveRe.match(line).hasMatch()) {
        return ClipboardAvoidReason::FileOrPath;
    }

    // POSIX absolute paths (avoid short UI tokens like "/zh" or prose ratios)
    if (line.startsWith('/')) {
        static const QRegularExpression kPosixPrefixRe(
            QStringLiteral(R"(^/(?:home|tmp|mnt|etc|usr|var|opt|root|dev|proc|sys|run|media|srv|Users|Applications)(?:/|$))"));
        const qsizetype slash_count = line.count('/');
        const bool has_ext = line.lastIndexOf('.') > line.lastIndexOf('/');
        if (kPosixPrefixRe.match(line).hasMatch() ||
            (!has_whitespace && (slash_count >= 2 || has_ext))) {
            return ClipboardAvoidReason::FileOrPath;
        }
    }

    // Bare web domain / URL without scheme (must contain no whitespace)
    if (!has_whitespace) {
        static const QRegularExpression kBareWwwRe(
            QStringLiteral(R"(^www\.[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+(?::\d+)?(?:[/?#].*)?$)"),
            QRegularExpression::CaseInsensitiveOption);
        if (kBareWwwRe.match(line).hasMatch()) {
            return ClipboardAvoidReason::UrlOrLink;
        }

        static const QRegularExpression kBareDomainRe(
            QStringLiteral(R"(^(?:[A-Za-z0-9-]+\.)+([A-Za-z]{2,10})(?::\d+)?([/?#][^\s]*)?$)"));
        if (const auto dm = kBareDomainRe.match(line); dm.hasMatch()) {
            const QString tld = dm.captured(1).toLower();
            const bool has_path_or_query = !dm.captured(2).isEmpty();
            static const QStringList kWebTlds = {
                QStringLiteral("com"),  QStringLiteral("org"),
                QStringLiteral("net"),  QStringLiteral("io"),
                QStringLiteral("cn"),   QStringLiteral("dev"),
                QStringLiteral("ai"),   QStringLiteral("app"),
                QStringLiteral("edu"),  QStringLiteral("gov"),
                QStringLiteral("me"),   QStringLiteral("co"),
                QStringLiteral("xyz"),  QStringLiteral("uk"),
                QStringLiteral("de"),   QStringLiteral("jp"),
                QStringLiteral("ru"),   QStringLiteral("fr"),
                QStringLiteral("info"), QStringLiteral("tv"),
                QStringLiteral("gg"),   QStringLiteral("cloud"),
                QStringLiteral("tech"), QStringLiteral("rs"),
                QStringLiteral("sh"),   QStringLiteral("cc"),
            };
            if (kWebTlds.contains(tld) ||
                (has_path_or_query && !line.endsWith('.'))) {
                return ClipboardAvoidReason::UrlOrLink;
            }
        }
    }

    return ClipboardAvoidReason::None;
}

} // namespace

ClipboardAvoidReason classifyAvoidedMimeTypes(const QStringList &formats) {
    if (formats.isEmpty()) {
        return ClipboardAvoidReason::Empty;
    }

    bool has_plain_text = false;
    bool has_non_meta_binary = false;
    for (const QString &raw : formats) {
        const QString lower = raw.trimmed().toLower();
        if (lower.isEmpty()) {
            continue;
        }
        if (lower == "x-kde-passwordmanagerhint" ||
            lower == "x-kde-lockscreen") {
            return ClipboardAvoidReason::Sensitive;
        }
        if (lower == "text/uri-list" ||
            lower == "text/x-kde-uri-list" ||
            lower == "x-special/gnome-copied-files" ||
            lower == "application/x-kde-cutselection" ||
            lower == "application/vnd.portal.filetransfer") {
            return ClipboardAvoidReason::FileOrPath;
        }
        if (lower == "text/x-moz-url" || lower == "text/x-moz-url-priv") {
            return ClipboardAvoidReason::UrlOrLink;
        }
        if (isPlainTextMime(lower)) {
            has_plain_text = true;
        } else if (lower.startsWith("image/") ||
                   lower.startsWith("audio/") ||
                   lower.startsWith("video/") ||
                   lower == "application/octet-stream" ||
                   lower == "application/pdf") {
            has_non_meta_binary = true;
        }
    }

    if (!has_plain_text && has_non_meta_binary) {
        return ClipboardAvoidReason::BinaryOrNonText;
    }
    return ClipboardAvoidReason::None;
}

ClipboardAvoidReason classifyAvoidedClipboardText(const QString &text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return ClipboardAvoidReason::Empty;
    }

    const QStringList raw_lines = trimmed.split('\n', Qt::SkipEmptyParts);
    if (raw_lines.isEmpty()) {
        return ClipboardAvoidReason::Empty;
    }

    // Handle GNOME/GTK `copy\nfile:///...` or `cut\nfile:///...` multi-line transfer payload.
    qsizetype start_idx = 0;
    if (raw_lines.size() >= 2) {
        const QString first = raw_lines[0].trimmed().toLower();
        if ((first == "copy" || first == "cut") &&
            classifySingleLineToken(raw_lines[1]) == ClipboardAvoidReason::FileOrPath) {
            start_idx = 1;
        }
    }

    bool saw_file = false;
    bool saw_url = false;
    for (qsizetype i = start_idx; i < raw_lines.size(); ++i) {
        const QString line = raw_lines[i].trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const ClipboardAvoidReason r = classifySingleLineToken(line);
        if (r == ClipboardAvoidReason::None) {
            return ClipboardAvoidReason::None; // Contains prose -> allow translation.
        }
        if (r == ClipboardAvoidReason::FileOrPath) {
            saw_file = true;
        } else if (r == ClipboardAvoidReason::UrlOrLink) {
            saw_url = true;
        }
    }

    if (saw_file && !saw_url) {
        return ClipboardAvoidReason::FileOrPath;
    }
    if (saw_url && !saw_file) {
        return ClipboardAvoidReason::UrlOrLink;
    }
    if (saw_file || saw_url) {
        return ClipboardAvoidReason::FileOrPath;
    }
    return ClipboardAvoidReason::None;
}

QString clipboardAvoidReasonText(ClipboardAvoidReason reason) {
    switch (reason) {
    case ClipboardAvoidReason::Empty:
        return QStringLiteral("剪贴板为空，请直接输入文本");
    case ClipboardAvoidReason::Sensitive:
        return QStringLiteral("已避让剪贴板内容（检测到密码或锁屏受保护内容）");
    case ClipboardAvoidReason::FileOrPath:
        return QStringLiteral("已避让剪贴板内容（检测到文件或本地路径）");
    case ClipboardAvoidReason::UrlOrLink:
        return QStringLiteral("已避让剪贴板内容（检测到链接 / URL）");
    case ClipboardAvoidReason::BinaryOrNonText:
        return QStringLiteral("已避让剪贴板内容（检测到图片或非文本数据）");
    case ClipboardAvoidReason::None:
        break;
    }
    return {};
}

ClipboardMonitor::ClipboardMonitor(QObject *parent) : QObject(parent) {
    is_wayland_ = QGuiApplication::platformName().contains(
        QStringLiteral("wayland"), Qt::CaseInsensitive);
    wl_paste_bin_ = QStandardPaths::findExecutable(QStringLiteral("wl-paste"));

    debounce_timer_ = new QTimer(this);
    debounce_timer_->setSingleShot(true);
    debounce_timer_->setInterval(45);
    connect(debounce_timer_, &QTimer::timeout, this,
            &ClipboardMonitor::executePendingProbe);

    if (QClipboard *cb = QGuiApplication::clipboard()) {
        connect(cb, &QClipboard::dataChanged, this,
                &ClipboardMonitor::onQtClipboardChanged);
    }

    if (is_wayland_) {
        klipper_connected_ = QDBusConnection::sessionBus().connect(
            QStringLiteral("org.kde.klipper"),
            QStringLiteral("/klipper"),
            QStringLiteral("org.kde.klipper.klipper"),
            QStringLiteral("clipboardHistoryUpdated"),
            this,
            SLOT(onExternalChangeSignal()));
    }
}

ClipboardMonitor::~ClipboardMonitor() {
    stopWaylandWatch();
    if (wl_probe_proc_) {
        wl_probe_proc_->kill();
        wl_probe_proc_->waitForFinished(100);
        wl_probe_proc_ = nullptr;
    }
}

void ClipboardMonitor::setWatchEnabled(bool enabled) {
    if (watch_enabled_ == enabled) {
        return;
    }
    watch_enabled_ = enabled;
    if (watch_enabled_) {
        if (is_wayland_ && !wl_paste_bin_.isEmpty()) {
            startWaylandWatch();
        }
    } else {
        stopWaylandWatch();
    }
}

void ClipboardMonitor::markSelfCopiedText(const QString &text) {
    last_self_copied_ = text.trimmed();
}

void ClipboardMonitor::requestProbe(bool force) {
    if (force) {
        pending_force_ = true;
    }
    debounce_timer_->start(force ? 0 : 45);
}

void ClipboardMonitor::onQtClipboardChanged() {
    if (!watch_enabled_) {
        return;
    }
    debounce_timer_->start(30);
}

void ClipboardMonitor::onExternalChangeSignal() {
    if (!watch_enabled_) {
        return;
    }
    debounce_timer_->start(45);
}

void ClipboardMonitor::startWaylandWatch() {
    if (wl_watch_proc_ || wl_paste_bin_.isEmpty()) {
        return;
    }
    wl_watch_buf_.clear();
    watch_started_timer_.restart();

    wl_watch_proc_ = new QProcess(this);
    connect(wl_watch_proc_, &QProcess::readyReadStandardOutput, this, [this]() {
        if (!wl_watch_proc_) {
            return;
        }
        wl_watch_buf_.append(wl_watch_proc_->readAllStandardOutput());
        if (!wl_watch_buf_.contains('\n')) {
            return;
        }
        wl_watch_buf_.clear();
        // `wl-paste --watch` emits an initial tick on startup for pre-existing clipboard contents;
        // suppress only the immediate startup tick so toggling ⚡ waits for subsequent changes.
        if (watch_started_timer_.isValid() && watch_started_timer_.elapsed() < 220) {
            return;
        }
        onExternalChangeSignal();
    });
    connect(wl_watch_proc_,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int, QProcess::ExitStatus) {
                if (wl_watch_proc_) {
                    wl_watch_proc_->deleteLater();
                    wl_watch_proc_ = nullptr;
                }
            });
    wl_watch_proc_->start(wl_paste_bin_,
                          {QStringLiteral("--watch"), QStringLiteral("echo"),
                           QStringLiteral("1")});
}

void ClipboardMonitor::stopWaylandWatch() {
    if (!wl_watch_proc_) {
        return;
    }
    QProcess *proc = wl_watch_proc_;
    wl_watch_proc_ = nullptr;
    proc->disconnect(this);
    proc->terminate();
    if (!proc->waitForFinished(120)) {
        proc->kill();
        proc->waitForFinished(100);
    }
    proc->deleteLater();
}

void ClipboardMonitor::executePendingProbe() {
    const bool force = pending_force_;
    pending_force_ = false;
    if (!force && !watch_enabled_) {
        return;
    }

    // Prefer direct Qt clipboard read when it already carries offers (X11 or focused Wayland window).
    if (tryReadFromQtClipboard(force)) {
        return;
    }

    if (probe_in_flight_) {
        rerun_after_flight_ = true;
        if (force) {
            pending_force_ = true;
        }
        return;
    }

    if (is_wayland_ && !wl_paste_bin_.isEmpty()) {
        probe_in_flight_ = true;
        startAsyncWlPasteListTypes(force);
    } else if (klipper_connected_ || is_wayland_) {
        probe_in_flight_ = true;
        startAsyncKlipperFallback(force);
    } else if (force) {
        emit clipboardAvoided(ClipboardAvoidReason::Empty, true);
    }
}

bool ClipboardMonitor::tryReadFromQtClipboard(bool force) {
    QClipboard *cb = QGuiApplication::clipboard();
    if (!cb) {
        return false;
    }
    const QMimeData *mime = cb->mimeData(QClipboard::Clipboard);
    if (mime && !mime->formats().isEmpty()) {
        if (mime->hasUrls()) {
            const auto urls = mime->urls();
            bool has_file_url = std::any_of(urls.begin(), urls.end(), [](const QUrl &u) {
                return u.isLocalFile() || u.scheme() == "file" || u.scheme() == "trash";
            });
            emit clipboardAvoided(has_file_url ? ClipboardAvoidReason::FileOrPath
                                               : ClipboardAvoidReason::UrlOrLink,
                                  force);
            return true;
        }
        const ClipboardAvoidReason mime_reason = classifyAvoidedMimeTypes(mime->formats());
        if (mime_reason != ClipboardAvoidReason::None &&
            mime_reason != ClipboardAvoidReason::Empty) {
            emit clipboardAvoided(mime_reason, force);
            return true;
        }
        QString text = mime->hasText() ? mime->text().trimmed()
                                       : cb->text(QClipboard::Clipboard).trimmed();
        if (!text.isEmpty()) {
            if (!force && !last_self_copied_.isEmpty() && text == last_self_copied_) {
                return true;
            }
            const ClipboardAvoidReason text_reason = classifyAvoidedClipboardText(text);
            if (text_reason != ClipboardAvoidReason::None &&
                text_reason != ClipboardAvoidReason::Empty) {
                emit clipboardAvoided(text_reason, force);
                return true;
            }
            emit textReady(text, force);
            return true;
        }
    }

    QString clip = cb->text(QClipboard::Clipboard).trimmed();
    if (clip.isEmpty() && cb->supportsSelection()) {
        clip = cb->text(QClipboard::Selection).trimmed();
    }
    if (clip.isEmpty()) {
        // On Wayland when unfocused, Qt's QClipboard is often empty; let wl-paste/Klipper handle it.
        if (is_wayland_) {
            return false;
        }
        if (force) {
            emit clipboardAvoided(ClipboardAvoidReason::Empty, true);
        }
        return true;
    }

    if (!force && !last_self_copied_.isEmpty() && clip == last_self_copied_) {
        return true;
    }
    const ClipboardAvoidReason text_reason = classifyAvoidedClipboardText(clip);
    if (text_reason != ClipboardAvoidReason::None &&
        text_reason != ClipboardAvoidReason::Empty) {
        emit clipboardAvoided(text_reason, force);
        return true;
    }
    emit textReady(clip, force);
    return true;
}

void ClipboardMonitor::finishProbeFlight() {
    probe_in_flight_ = false;
    if (rerun_after_flight_) {
        rerun_after_flight_ = false;
        debounce_timer_->start(30);
    }
}

void ClipboardMonitor::startAsyncWlPasteListTypes(bool force) {
    if (wl_probe_proc_) {
        wl_probe_proc_->deleteLater();
        wl_probe_proc_ = nullptr;
    }
    auto *proc = new QProcess(this);
    wl_probe_proc_ = proc;

    connect(proc, &QProcess::errorOccurred, this,
            [this, proc, force](QProcess::ProcessError) {
                if (wl_probe_proc_ == proc) {
                    wl_probe_proc_ = nullptr;
                }
                proc->deleteLater();
                startAsyncKlipperFallback(force);
            });

    connect(proc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, proc, force](int exit_code, QProcess::ExitStatus status) {
                const QByteArray raw = proc->readAllStandardOutput();
                if (wl_probe_proc_ == proc) {
                    wl_probe_proc_ = nullptr;
                }
                proc->deleteLater();

                if (status != QProcess::NormalExit || exit_code != 0) {
                    startAsyncKlipperFallback(force);
                    return;
                }

                QStringList formats;
                const QString out = QString::fromUtf8(raw);
                for (const QString &line : out.split('\n', Qt::SkipEmptyParts)) {
                    formats.push_back(line.trimmed());
                }
                const ClipboardAvoidReason mime_reason = classifyAvoidedMimeTypes(formats);
                if (mime_reason != ClipboardAvoidReason::None &&
                    mime_reason != ClipboardAvoidReason::Empty) {
                    emit clipboardAvoided(mime_reason, force);
                    finishProbeFlight();
                    return;
                }
                if (mime_reason == ClipboardAvoidReason::Empty) {
                    if (force) {
                        emit clipboardAvoided(ClipboardAvoidReason::Empty, true);
                    }
                    finishProbeFlight();
                    return;
                }

                const QString chosen_mime = pickBestPlainTextMime(formats);
                startAsyncWlPasteReadText(chosen_mime, force);
            });

    proc->start(wl_paste_bin_, {QStringLiteral("--list-types")});
}

void ClipboardMonitor::startAsyncWlPasteReadText(const QString &mime_type, bool force) {
    auto *proc = new QProcess(this);
    wl_probe_proc_ = proc;

    connect(proc, &QProcess::errorOccurred, this,
            [this, proc, force](QProcess::ProcessError) {
                if (wl_probe_proc_ == proc) {
                    wl_probe_proc_ = nullptr;
                }
                proc->deleteLater();
                startAsyncKlipperFallback(force);
            });

    connect(proc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, proc, force](int exit_code, QProcess::ExitStatus status) {
                const QByteArray raw = proc->readAllStandardOutput();
                if (wl_probe_proc_ == proc) {
                    wl_probe_proc_ = nullptr;
                }
                proc->deleteLater();

                if (status != QProcess::NormalExit || exit_code != 0) {
                    startAsyncKlipperFallback(force);
                    return;
                }

                const QString text = QString::fromUtf8(raw).trimmed();
                if (text.isEmpty()) {
                    if (force) {
                        emit clipboardAvoided(ClipboardAvoidReason::Empty, true);
                    }
                    finishProbeFlight();
                    return;
                }
                if (!force && !last_self_copied_.isEmpty() && text == last_self_copied_) {
                    finishProbeFlight();
                    return;
                }
                const ClipboardAvoidReason text_reason = classifyAvoidedClipboardText(text);
                if (text_reason != ClipboardAvoidReason::None &&
                    text_reason != ClipboardAvoidReason::Empty) {
                    emit clipboardAvoided(text_reason, force);
                } else {
                    emit textReady(text, force);
                }
                finishProbeFlight();
            });

    QStringList args = {QStringLiteral("--no-newline")};
    if (!mime_type.isEmpty()) {
        args << QStringLiteral("--type") << mime_type;
    }
    proc->start(wl_paste_bin_, args);
}

void ClipboardMonitor::startAsyncKlipperFallback(bool force) {
    QDBusInterface klipper(QStringLiteral("org.kde.klipper"),
                           QStringLiteral("/klipper"),
                           QStringLiteral("org.kde.klipper.klipper"),
                           QDBusConnection::sessionBus());
    if (!klipper.isValid()) {
        if (force) {
            emit clipboardAvoided(ClipboardAvoidReason::Empty, true);
        }
        finishProbeFlight();
        return;
    }

    QDBusPendingCall call = klipper.asyncCall(QStringLiteral("getClipboardContents"));
    auto *watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, force](QDBusPendingCallWatcher *w) {
                QDBusPendingReply<QString> reply = *w;
                w->deleteLater();
                if (!reply.isValid()) {
                    if (force) {
                        emit clipboardAvoided(ClipboardAvoidReason::Empty, true);
                    }
                    finishProbeFlight();
                    return;
                }
                const QString text = reply.value().trimmed();
                if (text.isEmpty()) {
                    if (force) {
                        emit clipboardAvoided(ClipboardAvoidReason::Empty, true);
                    }
                    finishProbeFlight();
                    return;
                }
                if (!force && !last_self_copied_.isEmpty() && text == last_self_copied_) {
                    finishProbeFlight();
                    return;
                }
                const ClipboardAvoidReason reason = classifyAvoidedClipboardText(text);
                if (reason != ClipboardAvoidReason::None &&
                    reason != ClipboardAvoidReason::Empty) {
                    emit clipboardAvoided(reason, force);
                } else {
                    emit textReady(text, force);
                }
                finishProbeFlight();
            });
}

} // namespace shadowbar::floating_ui
