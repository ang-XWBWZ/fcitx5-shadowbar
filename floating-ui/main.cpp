#include "floating_window.hpp"
#include "shadowbar/config.hpp"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QGuiApplication>
#include <QIcon>
#include <QStringList>

#include <iostream>

namespace {

constexpr const char *kServiceName = "org.fcitx.ShadowBarFloat";
constexpr const char *kObjectPath = "/Floating";

QString parseActionFlag(const QStringList &args) {
    for (int i = 1; i < args.size(); ++i) {
        const QString &a = args.at(i);
        if (a == "--toggle" || a == "--show" || a == "--settings" ||
            a == "--sync-orb" || a == "--reset" || a == "--quit") {
            return a;
        }
        if (a == "--help" || a == "-h") {
            return "--help";
        }
    }
    return "--toggle";
}

QString dbusMethodForAction(const QString &action) {
    if (action == "--show") {
        return "Show";
    }
    if (action == "--settings") {
        return "ShowSettings";
    }
    if (action == "--sync-orb") {
        return "SyncOrb";
    }
    if (action == "--reset") {
        return "ResetWindow";
    }
    if (action == "--quit") {
        return "QuitApp";
    }
    return "Toggle";
}

} // namespace

int main(int argc, char **argv) {
    QStringList raw_args;
    raw_args.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        raw_args.append(QString::fromLocal8Bit(argv[i]));
    }
    const QString action = parseActionFlag(raw_args);
    if (action == "--help") {
        std::cout
            << "Usage: shadowbar-float [--toggle|--show|--settings|--sync-orb|--reset|--quit]\n"
            << "  --toggle     Toggle floating translation window (default)\n"
            << "  --show       Show floating window and read clipboard if enabled\n"
            << "  --settings   Open tabbed Settings & API preset manager\n"
            << "  --sync-orb   Sync desktop floating orb visibility from config.toml\n"
            << "  --reset      Reset floating window position & size to screen center\n"
            << "  --quit       Quit background floating UI service\n";
        return 0;
    }

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.isConnected() && bus.interface() &&
        bus.interface()->isServiceRegistered(kServiceName)) {
        QDBusInterface iface(kServiceName, kObjectPath, kServiceName, bus);
        if (iface.isValid()) {
            iface.call(dbusMethodForAction(action));
            return 0;
        }
    }

    if (action == "--quit") {
        return 0;
    }

    if (action == "--sync-orb") {
        shadowbar::ConfigManager cfg_mgr;
        cfg_mgr.ensureAndLoad();
        if (!(cfg_mgr.config().floating_enabled &&
              cfg_mgr.config().floating_show_orb)) {
            return 0;
        }
    }

    // When spawned from fcitx5-wayland-launcher, discard any closed inherited FD.
    qunsetenv("WAYLAND_SOCKET");

    QApplication app(argc, argv);
    app.setApplicationName("shadowbar-float");
    app.setOrganizationName("shadowbar");
    app.setApplicationDisplayName("ShadowBar 翻译悬浮窗");
    QGuiApplication::setDesktopFileName("fcitx5-shadowbar-float");
    app.setWindowIcon(QIcon::fromTheme("accessories-dictionary"));
    app.setQuitOnLastWindowClosed(false);

    shadowbar::floating_ui::FloatingWindow window;
    if (bus.isConnected()) {
        bus.registerService(kServiceName);
        bus.registerObject(kObjectPath, &window,
                           QDBusConnection::ExportScriptableSlots);
    }

    window.handleInitialAction(action);
    return app.exec();
}
