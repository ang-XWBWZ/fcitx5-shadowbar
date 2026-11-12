#pragma once

#include "clipboard_monitor.hpp"

#include "floating_orb.hpp"
#include "settings_view.hpp"
#include "shadowbar/config.hpp"
#include "shadowbar/translator.hpp"

#include <QElapsedTimer>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QSet>
#include <QSize>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

class QCloseEvent;
class QComboBox;
class QEvent;
class QFrame;
class QLabel;
class QMouseEvent;
class QMoveEvent;
class QPlainTextEdit;
class QPushButton;
class QShortcut;
class QShowEvent;
class QSizeGrip;
class QSplitter;
class QStackedWidget;
class QTimer;
class QToolButton;

namespace shadowbar::floating_ui {

class SettingsView;
class TyModelManager;

/** Native Qt6 floating translator card, clipboard watcher, orb controller & settings host. */
class FloatingWindow final : public QWidget {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.fcitx.ShadowBarFloat")

public:
    explicit FloatingWindow(QWidget *parent = nullptr);
    ~FloatingWindow() override;

    void applyTheme();
    void handleInitialAction(const QString &action);

public slots:
    Q_SCRIPTABLE void Toggle();
    Q_SCRIPTABLE void Show();
    Q_SCRIPTABLE void ShowSettings();
    Q_SCRIPTABLE void SyncOrb();
    Q_SCRIPTABLE void ResetWindow();
    Q_SCRIPTABLE void QuitApp();
    Q_SCRIPTABLE void ReportWindowGeometry(const QString &caption,
                                             const QString &geometry_csv);

protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    struct TranslationSegment {
        QString source;
        QString translation;
        bool translated = false;
        bool loading = false;
        bool failed = false;
        bool from_offline = false;
        QString error;
    };

    void buildUi();
    QWidget *buildCardPage();
    void populatePresetCombo();
    void applyConfigToUi(const ShadowConfig &cfg);
    void syncOrbVisibility();

    void showCard(bool force_read_clipboard);
    void showSettingsPage();
    void returnToCardPage();
    void hideCard();

    void readClipboardAndTranslate(bool force);
    void syncClipboardMonitorState();
    void applyClipboardText(const QString &text, bool force);
    void onClipboardAvoided(ClipboardAvoidReason reason, bool force);
    void scheduleTranslation(bool immediate);
    void dispatchTranslation(bool immediate, bool bypass_cache = false);
    void pumpNextSegmentTranslation(bool immediate);
    void maybeLoadNextSegmentOnScroll();
    void handleSourceScrollChanged();
    void renderSegmentedResult();
    int firstUntranslatedSegmentIndex() const;
    void retranslateNow();
    void setRetryHighlight(bool highlight);

    void swapDirection();
    void reverseTranslateResult();
    void copyTranslation(bool hide_after);
    void clearDraft();
    void updateSourceCountLabel(const QString &text);
    void applyPinnedState(bool pinned, bool persist);
    void applyResultOnlyState(bool result_only, bool persist, bool adjust_size);
    void adjustFontScale(int new_scale, bool persist);
    void saveConfigFromSettings(const ShadowConfig &cfg);
    void notifyFcitxReload();
    void updateFloatingToggleShortcuts(const std::string &hotkey_str);
    std::pair<std::string, std::string> currentEffectiveDirection() const;
    void updateDirectionBadge();

    QString uiStateFilePath() const;
    void loadUiState();
    void saveUiStateNow() const;
    void scheduleSaveUiState();
    void captureLiveWidgetGeometry();
    void captureSplitterSizes();
    void applyCurrentPageBounds(bool force_resize);
    void applySavedSplitterSizes();
    void resetWindowGeometry(bool show_card_after);
    QList<QWidget *> visibleSnapshotWidgets() const;
    void scheduleWaylandGeometrySnapshot(int delay_ms);
    void finishPendingHide(uint64_t snapshot_gen);
    void finishPendingQuit(uint64_t snapshot_gen);

    Qt::Edges detectResizeEdges(const QPoint &window_pos) const;
    void updateHoverResizeCursor(const QPoint &global_pos);
    bool tryStartResizeAtGlobalPos(const QPoint &global_pos);

    ConfigManager config_mgr_;
    std::unique_ptr<AsyncTranslationEngine> translator_;
    std::unique_ptr<TyModelManager> ty_mgr_;
    bool waiting_ty_server_ = false;
    std::unique_ptr<FloatingOrb> orb_;
    std::unique_ptr<ClipboardMonitor> clip_monitor_;

    QFrame *card_root_ = nullptr;
    QStackedWidget *stack_ = nullptr;
    QWidget *card_page_ = nullptr;
    SettingsView *settings_view_ = nullptr;

    QWidget *header_bar_ = nullptr;
    QComboBox *source_combo_ = nullptr;
    QToolButton *swap_btn_ = nullptr;
    QComboBox *target_combo_ = nullptr;
    QComboBox *preset_combo_ = nullptr;
    QToolButton *read_clip_btn_ = nullptr;
    QToolButton *watch_clip_btn_ = nullptr;
    QToolButton *result_only_btn_ = nullptr;
    QToolButton *pin_btn_ = nullptr;
    QToolButton *settings_btn_ = nullptr;
    QToolButton *close_btn_ = nullptr;

    QSplitter *splitter_ = nullptr;
    QPlainTextEdit *source_edit_ = nullptr;
    QLabel *dir_badge_ = nullptr;
    QLabel *segment_badge_ = nullptr;
    QLabel *char_count_label_ = nullptr;
    QToolButton *clear_btn_ = nullptr;
    QPushButton *next_seg_btn_ = nullptr;
    QPushButton *retranslate_btn_ = nullptr;

    QPlainTextEdit *result_edit_ = nullptr;
    QLabel *status_label_ = nullptr;
    QPushButton *reverse_btn_ = nullptr;
    QPushButton *copy_btn_ = nullptr;
    QPushButton *copy_close_btn_ = nullptr;
    QToolButton *reset_win_btn_ = nullptr;
    QSizeGrip *size_grip_ = nullptr;

    QTimer *copy_flash_timer_ = nullptr;
    QTimer *save_state_timer_ = nullptr;
    QTimer *wayland_snapshot_timer_ = nullptr;
    QElapsedTimer wheel_load_cooldown_;
    QElapsedTimer last_shown_timer_;
    QElapsedTimer last_internal_toggle_timer_;
    std::vector<QShortcut *> toggle_shortcuts_;

    QSize saved_card_size_{476, 324};
    QSize saved_result_only_size_{440, 210};
    QSize saved_settings_size_{610, 520};
    QList<int> saved_splitter_sizes_{108, 176};
    QPoint saved_pos_;
    bool has_saved_pos_ = false;
    QPoint saved_orb_pos_;
    bool has_saved_orb_pos_ = false;
    bool restoring_geometry_ = false;
    bool hiding_with_snapshot_ = false;
    bool quitting_with_snapshot_ = false;
    uint64_t wayland_snapshot_gen_ = 0;
    uint64_t waiting_card_geom_gen_ = 0;
    uint64_t waiting_quit_gen_ = 0;
    QSet<QString> pending_quit_captions_;

    uint64_t active_revision_ = 0;
    bool pinned_ = false;
    bool watch_clipboard_ = false;
    bool result_only_ = false;
    int font_scale_ = 100;
    bool updating_combos_ = false;
    bool last_translation_failed_ = false;

    std::vector<TranslationSegment> segments_;
    std::deque<size_t> segment_queue_;
    int active_segment_idx_ = -1;
    bool queue_bypass_cache_ = false;
    std::string last_split_context_key_;

    Qt::Edges manual_resize_edges_{};
    QPoint resize_start_global_;
    QRect resize_start_geo_;

    std::string last_effective_source_ = "zh";
    std::string last_effective_target_ = "en";
    std::string current_translation_;
    QString last_copied_text_;
    QString last_auto_loaded_clip_;
};

} // namespace shadowbar::floating_ui
