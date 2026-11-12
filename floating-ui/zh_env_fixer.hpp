#pragma once

#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>

class QProcess;
class QThread;

namespace shadowbar::floating_ui {

enum class DistroFamily {
    Arch,
    DebianUbuntu,
    FedoraRhel,
    OpenSuse,
    Unknown,
};

struct ImeEnvPlanOptions {
    bool include_zh_locale = false;
    QString addon_dirs;
};

struct ZhEnvDiagItem {
    QString id;
    QString title;
    bool ok = false;
    bool warning = false;
    QString summary;
    QString detail;
};

struct ZhEnvDiagReport {
    DistroFamily distro = DistroFamily::Unknown;
    QString distro_name;
    QString session_type;
    QString desktop_name;
    QStringList missing_packages;
    bool has_zh_locale = false;
    QString current_lang;
    bool font_ok = false;
    QString mono_family;
    QString mono_cjk_family;
    bool profile_ok = false;
    bool env_ok = false;
    bool fcitx_running = false;
    QList<ZhEnvDiagItem> items;

    bool allOk() const;
};

/// Parses `/etc/os-release` key/value pairs to classify the Linux package family.
DistroFamily detectDistroFamily(const QString &os_release_text);

/// Human-readable distro family label.
QString distroFamilyDisplayName(DistroFamily family);

/// Returns core Fcitx5 + CJK fonts + Wayland clipboard packages for the distro family.
QStringList requiredPackagesForDistro(DistroFamily family);

/// Computes missing packages given raw installed-package list output (`pacman -Qq`, `dpkg-query`, or `rpm -qa`).
QStringList missingPackagesFromOutput(DistroFamily family,
                                        const QString &installed_query_output,
                                        const QStringList &required);

/// Returns true if `locale -a` output already contains `zh_CN.utf8` / `zh_CN.UTF-8`.
bool hasZhCnLocale(const QString &locale_a_output);

/// Evaluates `fc-match monospace` and `fc-match monospace:charset=4e2d` outputs to reject CJK JP/KR fallbacks.
bool isMonospaceFontChineseFriendly(const QString &mono_family,
                                    const QString &mono_zh_family);

/// Generates fontconfig XML prioritizing Latin monospace plus Simplified Chinese CJK fallback.
QString generateFontconfigXml();

/// Merges `~/.config/fcitx5/profile` INI to guarantee `keyboard-us`, `pinyin`, and `shadowbar`.
QString mergeFcitx5ProfileIni(const QString &existing_ini);

/// Canonical ordered `(KEY, VALUE)` pairs shared across systemd `environment.d`, X11 `.xprofile`, and D-Bus activation.
QList<QPair<QString, QString>> buildImeEnvPairs(const ImeEnvPlanOptions &opts);

/// Renders systemd `environment.d` `.conf` contents from `buildImeEnvPairs`.
QString generateEnvironmentDContent(const ImeEnvPlanOptions &opts);

/// Renders POSIX shell `export KEY="VALUE"` lines for X11 `.xprofile` from `buildImeEnvPairs`.
QString generateXprofileExportBlock(const ImeEnvPlanOptions &opts);

/// Inserts or replaces a bounded block inside a config file cleanly, preserving surrounding user content.
QString upsertManagedBlock(const QString &existing_text,
                           const QString &begin_marker,
                           const QString &end_marker,
                           const QString &body);

/// Builds a single pkexec shell script that installs missing packages and enables `zh_CN.UTF-8` locale if missing.
QString buildPrivilegedSetupScript(DistroFamily family,
                                   const QStringList &missing_pkgs,
                                   bool need_zh_locale,
                                   bool set_system_locale);

/**
 * Asynchronous detector and one-click fixer for Chinese IME environment:
 * packages/locale, monospace CJK SC fontconfig, Fcitx5 Chinese profile, and Wayland + X11 env propagation.
 */
class ZhEnvFixer final : public QObject {
    Q_OBJECT

public:
    explicit ZhEnvFixer(QObject *parent = nullptr);
    ~ZhEnvFixer() override;

    bool isBusy() const { return busy_; }
    const ZhEnvDiagReport &lastReport() const { return last_report_; }

    void runDiagnose();
    void runFix(bool install_missing_packages, bool apply_zh_locale);

    /// Synchronous helper usable by CLI/tests using a custom config home root.
    static ZhEnvDiagReport inspectEnvironment(const QString &config_home = QString());

signals:
    void diagnoseFinished(const ZhEnvDiagReport &report);
    void logLine(const QString &line);
    void fixFinished(bool success, const QString &summary);

private:
    void continueFixAfterPrivilegedStep(bool privileged_ok);
    void applyEnvVarsToCurrentProcess(const ImeEnvPlanOptions &opts);
    void startWorkerTask(bool fix_mode);
    bool busy_ = false;
    bool pending_apply_zh_locale_ = false;
    QString privileged_script_path_;
    QProcess *active_proc_ = nullptr;
    QThread *worker_thread_ = nullptr;
    ZhEnvDiagReport last_report_;
};

} // namespace shadowbar::floating_ui
