#pragma once

#include "zh_env_fixer.hpp"

#include "shadowbar/translator.hpp"
#include "shadowbar/types.hpp"

#include <QPointer>
#include <QWidget>

#include <memory>

class QCheckBox;
class QComboBox;
class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class QToolButton;

#include <utility>
#include <vector>

namespace shadowbar::floating_ui {

class TyModelManager;

/** Native tabbed Settings & API Preset Manager embedded in the floating window. */
class SettingsView final : public QWidget {
    Q_OBJECT

public:
    explicit SettingsView(QWidget *parent = nullptr);
    ~SettingsView() override;

    void setConfig(const ShadowConfig &config);
    void setCurrentTab(int index);
    void setTyModelManager(TyModelManager *manager);
    void applyTheme();

signals:
    void backRequested();
    void configSaved(const ShadowConfig &config);

private slots:
    void onPresetRowChanged(int row);
    void onAddPreset();
    void onDeletePreset();
    void onSetDefaultPreset();
    void onTestPreset();
    void onSaveClicked();
    void onResetDefaults();

private:
    void buildUi();
    QWidget *buildPresetsTab();
    QWidget *buildGeneralTab();
    QWidget *buildTyModelTab();
    QWidget *buildZhEnvTab();
    void applyZhEnvReport(const ZhEnvDiagReport &report);
    void loadTyFields();
    void collectTyFields(ShadowConfig &cfg) const;
    ShadowConfig currentTyEffectiveConfig() const;
    QString selectedTyVariant() const;
    void refreshTyFileStatus();
    void refreshTyServiceStatus();
    void onTyDownloadClicked();
    void onTyStartStopClicked(bool start);
    void onTyApplyPresetClicked(bool set_default);
    void onTyTrialTranslate();
    void runTyTrialTranslation(const QString &variant, const ShadowConfig &cfg);
    void refreshPresetList(int select_row);
    void loadPresetFields(int row);
    void storeCurrentPresetFields();
    void updateProviderFieldStates();
    void applyQuickTemplate(ProviderKind kind,
                            const QString &endpoint,
                            const QString &model,
                            const QString &api_key_hint);
    void loadGeneralFields();
    void updateOfflineDictStatus();
    void rebuildDictTable();
    void syncDictRowsFromTable();
    std::vector<ExternalDictSpec> collectedDictSpecs() const;
    void onImportDictionary();
    void onRemoveDictRow(int row);
    void openManagedDictDir();
    void updateCycleLangSummary();
    ShadowConfig collectConfig() const;

    ShadowConfig working_config_;
    int editing_preset_row_ = -1;
    bool updating_preset_ui_ = false;
    std::unique_ptr<AsyncTranslationEngine> test_engine_;

    QTabWidget *tabs_ = nullptr;
    QLabel *footer_status_ = nullptr;

    // Tab 1: Presets & API
    QListWidget *preset_list_ = nullptr;
    QPushButton *del_preset_btn_ = nullptr;
    QPushButton *default_preset_btn_ = nullptr;
    QLineEdit *preset_id_edit_ = nullptr;
    QLineEdit *preset_label_edit_ = nullptr;
    QComboBox *preset_provider_combo_ = nullptr;
    QComboBox *preset_lang_combo_ = nullptr;
    QLineEdit *preset_endpoint_edit_ = nullptr;
    QLineEdit *preset_model_edit_ = nullptr;
    QLineEdit *preset_apikey_edit_ = nullptr;
    QToolButton *apikey_reveal_btn_ = nullptr;
    QPlainTextEdit *preset_prompt_edit_ = nullptr;
    QPushButton *test_preset_btn_ = nullptr;
    QLabel *test_result_label_ = nullptr;

    // Tab 2: Shortcuts, Floating Window & IME behavior
    QKeySequenceEdit *toggle_hotkey_edit_ = nullptr;
    QKeySequenceEdit *cycle_lang_hotkey_edit_ = nullptr;
    QKeySequenceEdit *cycle_preset_hotkey_edit_ = nullptr;
    QKeySequenceEdit *floating_hotkey_edit_ = nullptr;
    QKeySequenceEdit *oneshot_hotkey_edit_ = nullptr;

    QCheckBox *floating_enabled_check_ = nullptr;
    QCheckBox *floating_autoread_check_ = nullptr;
    QCheckBox *floating_watch_check_ = nullptr;
    QCheckBox *floating_orb_check_ = nullptr;
    QCheckBox *floating_pinned_check_ = nullptr;
    QCheckBox *floating_result_only_check_ = nullptr;
    QSpinBox *floating_font_scale_spin_ = nullptr;
    QSpinBox *floating_segment_chars_spin_ = nullptr;

    QCheckBox *enable_prefix_check_ = nullptr;
    QLineEdit *prefix_edit_ = nullptr;
    QCheckBox *prefix_cjk_only_check_ = nullptr;
    QSpinBox *prefix_timeout_spin_ = nullptr;

    QComboBox *default_target_lang_combo_ = nullptr;
    QComboBox *floating_source_lang_combo_ = nullptr;
    std::vector<std::pair<std::string, QCheckBox *>> cycle_lang_checks_;
    QLabel *cycle_lang_summary_label_ = nullptr;
    QSpinBox *debounce_spin_ = nullptr;
    QSpinBox *timeout_spin_ = nullptr;
    QSpinBox *cache_size_spin_ = nullptr;

    QCheckBox *auto_detect_check_ = nullptr;
    QCheckBox *lock_punct_check_ = nullptr;
    QCheckBox *esc_pinyin_check_ = nullptr;
    QCheckBox *offline_fallback_check_ = nullptr;
    QTableWidget *dict_table_ = nullptr;
    QCheckBox *dict_copy_managed_check_ = nullptr;
    QLabel *offline_dict_status_label_ = nullptr;
    std::vector<ExternalDictSpec> dict_rows_;
    QCheckBox *show_source_line_check_ = nullptr;
    QCheckBox *collapse_idle_check_ = nullptr;
    QCheckBox *expand_actions_check_ = nullptr;

    // Tab 4: One-click Chinese Environment & IME Fixer
    ZhEnvFixer *zh_fixer_ = nullptr;
    QLabel *zh_env_overview_label_ = nullptr;
    QList<QLabel *> zh_diag_status_labels_;
    QList<QLabel *> zh_diag_detail_labels_;
    QCheckBox *zh_install_pkg_check_ = nullptr;
    QCheckBox *zh_set_locale_check_ = nullptr;
    QPushButton *zh_diagnose_btn_ = nullptr;
    QPushButton *zh_fix_btn_ = nullptr;
    QPlainTextEdit *zh_fix_log_edit_ = nullptr;

    // Tab 3: Local Hunyuan HY-MT1.5 (1.25bit / 2bit)
    QPointer<TyModelManager> ty_mgr_;
    QRadioButton *ty_variant_125_ = nullptr;
    QRadioButton *ty_variant_2_ = nullptr;
    QRadioButton *ty_variant_custom_ = nullptr;
    QLabel *ty_files_status_ = nullptr;
    QComboBox *ty_mirror_combo_ = nullptr;
    QComboBox *ty_backend_combo_ = nullptr;
    QLineEdit *ty_server_path_edit_ = nullptr;
    QLineEdit *ty_model_125_edit_ = nullptr;
    QLineEdit *ty_model_2_edit_ = nullptr;
    QLineEdit *ty_model_custom_edit_ = nullptr;
    QSpinBox *ty_port_spin_ = nullptr;
    QSpinBox *ty_threads_spin_ = nullptr;
    QCheckBox *ty_auto_start_check_ = nullptr;
    QPushButton *ty_download_btn_ = nullptr;
    QPushButton *ty_cancel_dl_btn_ = nullptr;
    QProgressBar *ty_dl_progress_ = nullptr;
    QLabel *ty_service_badge_ = nullptr;
    QPushButton *ty_start_btn_ = nullptr;
    QPushButton *ty_stop_btn_ = nullptr;
    QPlainTextEdit *ty_log_view_ = nullptr;
    QComboBox *ty_trial_sample_combo_ = nullptr;
    QComboBox *ty_trial_dir_combo_ = nullptr;
    QLineEdit *ty_trial_input_ = nullptr;
    QPlainTextEdit *ty_trial_output_ = nullptr;
    QLabel *ty_trial_status_ = nullptr;
    QPushButton *ty_trial_btn_ = nullptr;
    bool ty_trial_waiting_server_ = false;
    QString ty_trial_waiting_variant_;
};

} // namespace shadowbar::floating_ui
