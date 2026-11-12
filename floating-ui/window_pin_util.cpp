#include "window_pin_util.hpp"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QPointer>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <atomic>
#include <cstdint>
#include <utility>

namespace shadowbar::floating_ui {
namespace {

constexpr const char *kKWinService = "org.kde.KWin";
constexpr const char *kScriptingPath = "/Scripting";
constexpr const char *kScriptingIface = "org.kde.kwin.Scripting";
constexpr const char *kScriptIface = "org.kde.kwin.Script";

QSet<QString> &pendingTempScripts() {
    static QSet<QString> paths;
    return paths;
}

void ensureQuitCleanupHook() {
    static bool hooked = false;
    if (!hooked && QCoreApplication::instance()) {
        hooked = true;
        QObject::connect(QCoreApplication::instance(),
                         &QCoreApplication::aboutToQuit, []() {
                             const QSet<QString> copy = pendingTempScripts();
                             for (const QString &p : copy) {
                                 QFile::remove(p);
                             }
                             pendingTempScripts().clear();
                         });
    }
}

void removeTempScript(const QString &path) {
    if (path.isEmpty()) {
        return;
    }
    QFile::remove(path);
    pendingTempScripts().remove(path);
}

QString escapeJsString(const QString &text) {
    QString out;
    out.reserve(text.size() + 8);
    for (const QChar ch : text) {
        switch (ch.unicode()) {
        case '\\':
            out += QStringLiteral("\\\\");
            break;
        case '"':
            out += QStringLiteral("\\\"");
            break;
        case '\n':
            out += QStringLiteral("\\n");
            break;
        case '\r':
            out += QStringLiteral("\\r");
            break;
        case '\t':
            out += QStringLiteral("\\t");
            break;
        case 0x2028:
            out += QStringLiteral("\\u2028");
            break;
        case 0x2029:
            out += QStringLiteral("\\u2029");
            break;
        default:
            out += ch;
            break;
        }
    }
    return out;
}

bool isWaylandSession() {
    return QGuiApplication::platformName().startsWith(QStringLiteral("wayland"));
}

bool isKWinAvailable(QDBusConnection &bus) {
    return bus.isConnected() && bus.interface() &&
           bus.interface()->isServiceRegistered(QString::fromLatin1(kKWinService));
}

void runEphemeralKWinScript(const QString &file_prefix,
                            const QString &plugin_prefix,
                            const QString &js_source,
                            std::function<bool()> should_run,
                            std::function<void(bool dispatched)> on_finished) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!isKWinAvailable(bus)) {
        if (on_finished) {
            on_finished(false);
        }
        return;
    }

    ensureQuitCleanupHook();

    static std::atomic<uint64_t> s_script_seq{0};
    const qint64 pid = QCoreApplication::applicationPid();
    const uint64_t script_seq = ++s_script_seq;

    QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty()) {
        dir = QDir::tempPath();
    }
    QDir().mkpath(dir);

    const QString script_path =
        QStringLiteral("%1/%2-%3-%4.js").arg(dir, file_prefix).arg(pid).arg(script_seq);
    const QString plugin_name =
        QStringLiteral("%1_%2_%3").arg(plugin_prefix).arg(pid).arg(script_seq);

    QFile file(script_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (on_finished) {
            on_finished(false);
        }
        return;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(js_source.toUtf8());
    file.close();
    pendingTempScripts().insert(script_path);

    QDBusInterface scripting(QString::fromLatin1(kKWinService),
                             QString::fromLatin1(kScriptingPath),
                             QString::fromLatin1(kScriptingIface), bus);
    QDBusPendingCall load_call =
        scripting.asyncCall(QStringLiteral("loadScript"), script_path, plugin_name);

    auto *load_watcher = new QDBusPendingCallWatcher(load_call);
    QObject::connect(
        load_watcher, &QDBusPendingCallWatcher::finished, load_watcher,
        [script_path, plugin_name, bus, should_run = std::move(should_run),
         on_finished = std::move(on_finished)](QDBusPendingCallWatcher *w1) mutable {
            w1->deleteLater();

            QDBusPendingReply<int> load_reply(*w1);
            if (load_reply.isError() || load_reply.value() < 0) {
                removeTempScript(script_path);
                if (on_finished) {
                    on_finished(false);
                }
                return;
            }
            const int script_id = load_reply.value();
            if (should_run && !should_run()) {
                removeTempScript(script_path);
                QDBusInterface unload_iface(
                    QString::fromLatin1(kKWinService),
                    QString::fromLatin1(kScriptingPath),
                    QString::fromLatin1(kScriptingIface), bus);
                unload_iface.asyncCall(QStringLiteral("unloadScript"),
                                       plugin_name);
                if (on_finished) {
                    on_finished(false);
                }
                return;
            }

            QDBusInterface script_obj(
                QString::fromLatin1(kKWinService),
                QStringLiteral("/Scripting/Script%1").arg(script_id),
                QString::fromLatin1(kScriptIface), bus);
            QDBusPendingCall run_call = script_obj.asyncCall(QStringLiteral("run"));
            auto *run_watcher = new QDBusPendingCallWatcher(run_call);
            QObject::connect(
                run_watcher, &QDBusPendingCallWatcher::finished, run_watcher,
                [script_path, plugin_name, bus,
                 on_finished = std::move(on_finished)](
                    QDBusPendingCallWatcher *w2) mutable {
                    const bool ok = !w2->isError();
                    w2->deleteLater();
                    removeTempScript(script_path);
                    QDBusInterface cleanup(QString::fromLatin1(kKWinService),
                                            QString::fromLatin1(kScriptingPath),
                                            QString::fromLatin1(kScriptingIface),
                                            bus);
                    cleanup.asyncCall(QStringLiteral("unloadScript"), plugin_name);
                    if (on_finished) {
                        on_finished(ok);
                    }
                });
        });
}

void applyKWinWaylandState(QWidget *widget,
                           const PinWindowOptions &opts,
                           uint64_t seq) {
    if (!widget || !widget->isVisible()) {
        return;
    }

    const qint64 pid = QCoreApplication::applicationPid();
    const QString title = widget->windowTitle();
    if (title.isEmpty()) {
        return;
    }

    QString extra_props;
    if (opts.on_all_desktops) {
        extra_props += QStringLiteral("w.onAllDesktops = true; ");
    }
    if (opts.skip_taskbar) {
        extra_props += QStringLiteral("w.skipTaskbar = true; ");
    }
    if (opts.skip_switcher) {
        extra_props += QStringLiteral("w.skipSwitcher = true; ");
    }

    QString geo_stmt;
    if (opts.apply_position || (opts.apply_size && opts.target_size.isValid())) {
        const QString x_val = opts.apply_position
                                  ? QStringLiteral("w.moveable ? %1 : g.x")
                                        .arg(opts.target_pos.x())
                                  : QStringLiteral("g.x");
        const QString y_val = opts.apply_position
                                  ? QStringLiteral("w.moveable ? %1 : g.y")
                                        .arg(opts.target_pos.y())
                                  : QStringLiteral("g.y");
        const QString w_val =
            (opts.apply_size && opts.target_size.isValid())
                ? QString::number(opts.target_size.width())
                : QStringLiteral("g.width");
        const QString h_val =
            (opts.apply_size && opts.target_size.isValid())
                ? QString::number(opts.target_size.height())
                : QStringLiteral("g.height");
        geo_stmt = QStringLiteral(
                       "const g = w.frameGeometry;\n"
                       "            w.frameGeometry = {\n"
                       "                x: %1,\n"
                       "                y: %2,\n"
                       "                width: %3,\n"
                       "                height: %4\n"
                       "            };")
                       .arg(x_val, y_val, w_val, h_val);
    }

    const QString js =
        QStringLiteral(
            "(function () {\n"
            "    const targetPid = %1;\n"
            "    const targetTitle = \"%2\";\n"
            "    const wins = workspace.windowList();\n"
            "    for (let i = 0; i < wins.length; ++i) {\n"
            "        const w = wins[i];\n"
            "        if (w.pid === targetPid && w.caption === targetTitle) {\n"
            "            w.keepAbove = %3;\n"
            "            %4\n"
            "            %5\n"
            "        }\n"
            "    }\n"
            "})();\n")
            .arg(pid)
            .arg(escapeJsString(title))
            .arg(opts.keep_above ? QStringLiteral("true")
                                 : QStringLiteral("false"))
            .arg(extra_props, geo_stmt);

    QPointer<QWidget> weak_widget(widget);
    runEphemeralKWinScript(
        QStringLiteral("shadowbar-kwin-pin"),
        QStringLiteral("shadowbar_pin"),
        js,
        [weak_widget, seq]() {
            return weak_widget && weak_widget->isVisible() &&
                   weak_widget->property("_shadowbar_pin_seq").toULongLong() == seq;
        },
        {});
}

uint64_t bumpScheduleGen(QWidget *widget) {
    static std::atomic<uint64_t> s_sched_gen{0};
    const uint64_t sched_gen = ++s_sched_gen;
    if (widget) {
        widget->setProperty("_shadowbar_pin_sched",
                            static_cast<qulonglong>(sched_gen));
    }
    return sched_gen;
}

void applyPinStateNow(QWidget *widget, const PinWindowOptions &opts) {
    if (!widget) {
        return;
    }

    static std::atomic<uint64_t> s_pin_seq{0};
    const uint64_t seq = ++s_pin_seq;
    widget->setProperty("_shadowbar_pin_seq", static_cast<qulonglong>(seq));

    if (!isWaylandSession()) {
        if (QWindow *win = widget->windowHandle()) {
            if (win->flags().testFlag(Qt::WindowStaysOnTopHint) !=
                opts.keep_above) {
                win->setFlag(Qt::WindowStaysOnTopHint, opts.keep_above);
            }
        } else if (widget->windowFlags().testFlag(Qt::WindowStaysOnTopHint) !=
                   opts.keep_above) {
            widget->setWindowFlag(Qt::WindowStaysOnTopHint, opts.keep_above);
        }
        if (widget->isVisible()) {
            widget->show();
            if (opts.keep_above) {
                widget->raise();
            }
        }
        return;
    }

    if (widget->isVisible()) {
        applyKWinWaylandState(widget, opts, seq);
    }
}

} // namespace

bool isWaylandPlatform() {
    return isWaylandSession();
}

void syncNativeWindowPinState(QWidget *widget, const PinWindowOptions &opts) {
    bumpScheduleGen(widget);
    applyPinStateNow(widget, opts);
}

void scheduleNativeWindowPinSync(QWidget *widget, const PinWindowOptions &opts) {
    if (!widget) {
        return;
    }

    const uint64_t sched_gen = bumpScheduleGen(widget);

    if (!isWaylandSession()) {
        applyPinStateNow(widget, opts);
        return;
    }

    QPointer<QWidget> weak_widget(widget);
    for (const int delay_ms : {25, 110}) {
        QTimer::singleShot(delay_ms, widget, [weak_widget, opts, sched_gen]() {
            if (!weak_widget || !weak_widget->isVisible() ||
                weak_widget->property("_shadowbar_pin_sched").toULongLong() !=
                    sched_gen) {
                return;
            }
            applyPinStateNow(weak_widget.data(), opts);
        });
    }
}

void requestKWinWindowGeometrySnapshots(
    const QList<QWidget *> &widgets,
    const QString &dbus_service,
    const QString &dbus_path,
    const QString &dbus_iface,
    const QString &dbus_method,
    std::function<void(bool dispatched)> on_dispatched) {
    if (!isWaylandSession()) {
        if (on_dispatched) {
            on_dispatched(false);
        }
        return;
    }

    QStringList wanted_js_literals;
    for (QWidget *w : widgets) {
        if (w && w->isVisible() && !w->windowTitle().isEmpty()) {
            wanted_js_literals.append(
                QStringLiteral("\"%1\"").arg(escapeJsString(w->windowTitle())));
        }
    }
    if (wanted_js_literals.isEmpty()) {
        if (on_dispatched) {
            on_dispatched(false);
        }
        return;
    }

    const QString js =
        QStringLiteral(
            "(function () {\n"
            "    const targetPid = %1;\n"
            "    const wanted = [%2];\n"
            "    const wins = workspace.windowList();\n"
            "    for (let i = 0; i < wins.length; ++i) {\n"
            "        const w = wins[i];\n"
            "        if (w.pid === targetPid && wanted.indexOf(w.caption) !== -1) {\n"
            "            callDBus(\"%3\", \"%4\", \"%5\", \"%6\",\n"
            "                     w.caption,\n"
            "                     Math.round(w.x) + \",\" + Math.round(w.y) + \",\" +\n"
            "                     Math.round(w.width) + \",\" + Math.round(w.height));\n"
            "        }\n"
            "    }\n"
            "})();\n")
            .arg(QCoreApplication::applicationPid())
            .arg(wanted_js_literals.join(QStringLiteral(", ")))
            .arg(escapeJsString(dbus_service),
                 escapeJsString(dbus_path),
                 escapeJsString(dbus_iface),
                 escapeJsString(dbus_method));

    runEphemeralKWinScript(
        QStringLiteral("shadowbar-kwin-geom"),
        QStringLiteral("shadowbar_geom"),
        js,
        {},
        std::move(on_dispatched));
}

} // namespace shadowbar::floating_ui
