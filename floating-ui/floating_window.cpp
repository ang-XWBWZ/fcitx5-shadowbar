#include "floating_window.hpp"
#include "clipboard_monitor.hpp"
#include "hotkey_util.hpp"
#include "shadowbar/sentence_splitter.hpp"
#include "shadowbar/session.hpp"
#include "theme_util.hpp"
#include "ty_model_helpers.hpp"
#include "ty_model_manager.hpp"
#include "ui_util.hpp"
#include "window_pin_util.hpp"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCursor>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QMetaObject>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QShowEvent>
#include <QSizeGrip>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleHints>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>
#include <utility>

namespace shadowbar::floating_ui {

namespace {

QToolButton *makeIconToolBtn(const QString &text,
                             const QString &tooltip,
                             bool checkable,
                             QWidget *parent) {
    auto *btn = new QToolButton(parent);
    btn->setObjectName("HeaderToolBtn");
    btn->setText(text);
    btn->setToolTip(tooltip);
    btn->setCheckable(checkable);
    btn->setAutoRaise(true);
    btn->setFixedSize(24, 24);
    btn->setCursor(Qt::PointingHandCursor);
    return btn;
}

Qt::CursorShape cursorForEdges(Qt::Edges edges) {
    if ((edges & Qt::TopEdge && edges & Qt::LeftEdge) ||
        (edges & Qt::BottomEdge && edges & Qt::RightEdge)) {
        return Qt::SizeFDiagCursor;
    }
    if ((edges & Qt::TopEdge && edges & Qt::RightEdge) ||
        (edges & Qt::BottomEdge && edges & Qt::LeftEdge)) {
        return Qt::SizeBDiagCursor;
    }
    if (edges & Qt::LeftEdge || edges & Qt::RightEdge) {
        return Qt::SizeHorCursor;
    }
    if (edges & Qt::TopEdge || edges & Qt::BottomEdge) {
        return Qt::SizeVerCursor;
    }
    return Qt::ArrowCursor;
}

bool isFocusInHotkeyEditor(const QWidget *focus_widget) {
    for (const QWidget *w = focus_widget; w; w = w->parentWidget()) {
        if (qobject_cast<const QKeySequenceEdit *>(w) != nullptr) {
            return true;
        }
    }
    return false;
}

QSize clampWindowSizeToScreen(const QSize &sz, const QSize &min_sz) {
    const QScreen *screen = QGuiApplication::primaryScreen();
    const QRect avail =
        screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
    const int max_w = std::max(min_sz.width(), avail.width() - 24);
    const int max_h = std::max(min_sz.height(), avail.height() - 24);
    return QSize(std::clamp(sz.width(), min_sz.width(), max_w),
                 std::clamp(sz.height(), min_sz.height(), max_h));
}

QPoint clampWindowPosToScreens(const QPoint &pos, const QSize &win_size) {
    const auto screens = QGuiApplication::screens();
    if (screens.isEmpty()) {
        return pos;
    }

    const QSize effective_size(std::max(win_size.width(), 44),
                               std::max(win_size.height(), 44));
    const QRect candidate(pos, effective_size);
    const int min_overlap_x = std::min(80, effective_size.width());

    for (QScreen *scr : screens) {
        if (!scr) {
            continue;
        }
        const QRect avail = scr->availableGeometry();
        if (candidate.top() >= avail.top() &&
            candidate.top() <= avail.bottom() - 36 &&
            candidate.right() >= avail.left() + min_overlap_x &&
            candidate.left() <= avail.right() - min_overlap_x) {
            return pos;
        }
    }

    QScreen *best = QGuiApplication::screenAt(candidate.center());
    if (!best) {
        best = QGuiApplication::primaryScreen();
    }
    if (!best) {
        best = screens.front();
    }
    const QRect avail = best->availableGeometry();
    const int max_x = std::max(avail.left(), avail.right() - effective_size.width() + 1);
    const int max_y = std::max(avail.top(), avail.bottom() - effective_size.height() + 1);
    return QPoint(std::clamp(pos.x(), avail.left(), max_x),
                  std::clamp(pos.y(), avail.top(), max_y));
}

constexpr QSize kDefaultCardSize(496, 356);
constexpr QSize kDefaultResultOnlySize(460, 232);
constexpr QSize kDefaultSettingsSize(630, 540);
constexpr QSize kMinCardSize(380, 250);
constexpr QSize kMinResultOnlySize(360, 168);
constexpr QSize kMinSettingsSize(520, 440);
constexpr int kDefaultSplitterTop = 114;
constexpr int kDefaultSplitterBottom = 180;

QPoint centeredWindowPosOnActiveScreen(const QWidget *anchor,
                                       const QSize &win_size) {
    QScreen *scr = QGuiApplication::screenAt(QCursor::pos());
    if (!scr && anchor) {
        scr = anchor->screen();
    }
    if (!scr) {
        scr = QGuiApplication::primaryScreen();
    }
    const QRect avail =
        scr ? scr->availableGeometry() : QRect(0, 0, 1920, 1080);
    return QPoint(
        avail.left() + std::max(0, (avail.width() - win_size.width()) / 2),
        avail.top() + std::max(0, (avail.height() - win_size.height()) / 2));
}

} // namespace

FloatingWindow::FloatingWindow(QWidget *parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint),
      config_mgr_(ConfigManager::defaultConfigPath()) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_Hover, true);
    setMouseTracking(true);
    setWindowTitle("ShadowBar 翻译悬浮窗");

    config_mgr_.ensureAndLoad();
    font_scale_ = std::clamp(config_mgr_.config().floating_font_scale, 80, 160);
    result_only_ = config_mgr_.config().floating_result_only;
    translator_ = std::make_unique<AsyncTranslationEngine>(
        curlHttpTransport, config_mgr_.config().cache_size);
    translator_->reloadOfflineDictionaries(
        config_mgr_.config().effectiveOfflineDicts());
    ty_mgr_ = std::make_unique<TyModelManager>(this);
    connect(ty_mgr_.get(), &TyModelManager::stateChanged, this,
            [this](TyModelManager::ServerState state, const QString &msg) {
                if (!waiting_ty_server_) {
                    return;
                }
                if (state == TyModelManager::ServerState::Running ||
                    state == TyModelManager::ServerState::ExternalRunning) {
                    waiting_ty_server_ = false;
                    if (!segment_queue_.empty()) {
                        pumpNextSegmentTranslation(true);
                    } else if (!source_edit_->toPlainText().trimmed().isEmpty()) {
                        dispatchTranslation(true, queue_bypass_cache_);
                    }
                } else if (state == TyModelManager::ServerState::Error ||
                           state == TyModelManager::ServerState::Stopped) {
                    waiting_ty_server_ = false;
                    if (status_label_) {
                        status_label_->setText(
                            QString("⚠ 本地混元 HY-MT1.5 启动失败: %1").arg(msg));
                    }
                }
            });
    wheel_load_cooldown_.start();

    copy_flash_timer_ = new QTimer(this);
    copy_flash_timer_->setSingleShot(true);
    connect(copy_flash_timer_, &QTimer::timeout, this, [this]() {
        copy_btn_->setText("📋 复制");
    });

    save_state_timer_ = new QTimer(this);
    save_state_timer_->setSingleShot(true);
    connect(save_state_timer_, &QTimer::timeout, this,
            [this]() { saveUiStateNow(); });

    wayland_snapshot_timer_ = new QTimer(this);
    wayland_snapshot_timer_->setSingleShot(true);
    connect(wayland_snapshot_timer_, &QTimer::timeout, this, [this]() {
        requestKWinWindowGeometrySnapshots(
            visibleSnapshotWidgets(),
            QStringLiteral("org.fcitx.ShadowBarFloat"),
            QStringLiteral("/Floating"),
            QStringLiteral("org.fcitx.ShadowBarFloat"),
            QStringLiteral("ReportWindowGeometry"));
    });

    buildUi();
    clip_monitor_ = std::make_unique<ClipboardMonitor>(this);
    connect(clip_monitor_.get(), &ClipboardMonitor::textReady, this,
            &FloatingWindow::applyClipboardText);
    connect(clip_monitor_.get(), &ClipboardMonitor::clipboardAvoided, this,
            &FloatingWindow::onClipboardAvoided);

    loadUiState();
    applyConfigToUi(config_mgr_.config());
    applyTheme();
    applyCurrentPageBounds(true);

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (QGuiApplication::styleHints()) {
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                this, [this](Qt::ColorScheme) { applyTheme(); });
    }
#endif

    connect(qApp, &QApplication::focusChanged, this,
            [this](QWidget *, QWidget *now) {
                const bool recording = isFocusInHotkeyEditor(now);
                for (QShortcut *sc : toggle_shortcuts_) {
                    if (sc) {
                        sc->setEnabled(!recording);
                    }
                }
            });

    // Register internal card shortcuts cleanly
    auto *sc_copy = new QShortcut(QKeySequence("Ctrl+Shift+C"), this);
    connect(sc_copy, &QShortcut::activated, this,
            [this]() { copyTranslation(false); });

    auto *sc_paste = new QShortcut(QKeySequence("Alt+V"), this);
    connect(sc_paste, &QShortcut::activated, this,
            [this]() { readClipboardAndTranslate(true); });

    auto *sc_rev = new QShortcut(QKeySequence("Alt+R"), this);
    connect(sc_rev, &QShortcut::activated, this,
            [this]() { reverseTranslateResult(); });

    auto *sc_swap = new QShortcut(QKeySequence("Alt+S"), this);
    connect(sc_swap, &QShortcut::activated, this,
            [this]() { swapDirection(); });

    auto *sc_retry = new QShortcut(QKeySequence("Ctrl+R"), this);
    connect(sc_retry, &QShortcut::activated, this,
            &FloatingWindow::retranslateNow);

    auto *sc_retry_f5 = new QShortcut(QKeySequence(Qt::Key_F5), this);
    connect(sc_retry_f5, &QShortcut::activated, this,
            &FloatingWindow::retranslateNow);

    auto *sc_result_only = new QShortcut(QKeySequence("Alt+M"), this);
    connect(sc_result_only, &QShortcut::activated, this, [this]() {
        if (result_only_btn_) {
            result_only_btn_->toggle();
        }
    });
}

FloatingWindow::~FloatingWindow() {
    saveUiStateNow();
    if (ty_mgr_) {
        ty_mgr_->shutdown();
    }
    if (translator_) {
        translator_->shutdown();
    }
}

void FloatingWindow::buildUi() {
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(6, 6, 6, 6);

    card_root_ = new QFrame(this);
    card_root_->setObjectName("CardRoot");
    card_root_->setMouseTracking(true);
    card_root_->installEventFilter(this);
    outer->addWidget(card_root_);

    auto *root_lay = new QVBoxLayout(card_root_);
    root_lay->setContentsMargins(0, 0, 0, 0);

    stack_ = new QStackedWidget(card_root_);
    card_page_ = buildCardPage();
    card_page_->setMouseTracking(true);
    card_page_->installEventFilter(this);
    settings_view_ = new SettingsView(stack_);
    settings_view_->setTyModelManager(ty_mgr_.get());
    connect(settings_view_, &SettingsView::backRequested, this,
            &FloatingWindow::returnToCardPage);
    connect(settings_view_, &SettingsView::configSaved, this,
            &FloatingWindow::saveConfigFromSettings);

    stack_->addWidget(card_page_);
    stack_->addWidget(settings_view_);
    root_lay->addWidget(stack_);
}

QWidget *FloatingWindow::buildCardPage() {
    auto *page = new QWidget(stack_);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(8);

    // Row 1: Top title & window chrome bar (spacious draggable band)
    header_bar_ = new QWidget(page);
    header_bar_->setObjectName("HeaderBar");
    header_bar_->setMouseTracking(true);
    header_bar_->installEventFilter(this);
    header_bar_->setCursor(Qt::SizeAllCursor);

    auto *hdr = new QHBoxLayout(header_bar_);
    hdr->setContentsMargins(2, 0, 0, 0);
    hdr->setSpacing(6);

    auto *brand = new QLabel("✦", header_bar_);
    brand->setObjectName("BrandGlyph");
    brand->installEventFilter(this);
    QFont bf = brand->font();
    bf.setBold(true);
    bf.setPointSize(12);
    brand->setFont(bf);

    auto *brand_title = new QLabel("ShadowBar", header_bar_);
    brand_title->setObjectName("BrandTitle");
    brand_title->installEventFilter(this);

    dir_badge_ = new QLabel("自动 → EN", header_bar_);
    dir_badge_->setObjectName("DirectionBadge");
    dir_badge_->installEventFilter(this);

    hdr->addWidget(brand);
    hdr->addWidget(brand_title);
    hdr->addWidget(dir_badge_);
    hdr->addStretch(1);

    read_clip_btn_ = makeIconToolBtn("📋", "读取剪贴板并翻译 (Alt+V)",
                                     false, header_bar_);
    read_clip_btn_->setFixedSize(26, 26);
    connect(read_clip_btn_, &QToolButton::clicked, this,
            [this]() { readClipboardAndTranslate(true); });

    watch_clip_btn_ = makeIconToolBtn(
        "⚡",
        "仅在弹窗打开时自动监听剪贴板变化并翻译（收起即停用监听，自动避让文件/链接，可搭配 ◫ 仅译文模式）",
        true, header_bar_);
    watch_clip_btn_->setFixedSize(26, 26);
    connect(watch_clip_btn_, &QToolButton::toggled, this, [this](bool on) {
        watch_clipboard_ = on;
        config_mgr_.mutableConfig().floating_watch_clipboard = on;
        config_mgr_.save();
        syncClipboardMonitorState();
        status_label_->setText(
            on ? "已开启弹窗剪贴板实时监听（仅弹窗打开时生效，收起即停用 · 自动避让文件/链接）"
               : "已关闭剪贴板实时监听");
    });

    result_only_btn_ = makeIconToolBtn(
        "◫", "仅译文模式：隐藏原文编辑框，配合剪贴板联动更紧凑 (Alt+M)", true,
        header_bar_);
    result_only_btn_->setFixedSize(26, 26);
    connect(result_only_btn_, &QToolButton::toggled, this,
            [this](bool on) { applyResultOnlyState(on, true, true); });

    auto *hdr_sep = new QFrame(header_bar_);
    hdr_sep->setObjectName("HeaderSep");
    hdr_sep->setFixedSize(1, 14);
    hdr_sep->installEventFilter(this);

    pin_btn_ =
        makeIconToolBtn("📌", "置顶钉住悬浮窗 (置顶后 Esc 不关闭)", true, header_bar_);
    pin_btn_->setFixedSize(26, 26);
    connect(pin_btn_, &QToolButton::toggled, this,
            [this](bool on) { applyPinnedState(on, true); });

    settings_btn_ =
        makeIconToolBtn("⚙", "打开 ShadowBar 设置与 API 管理", false, header_bar_);
    settings_btn_->setFixedSize(26, 26);
    connect(settings_btn_, &QToolButton::clicked, this,
            &FloatingWindow::showSettingsPage);

    close_btn_ = makeIconToolBtn("✕", "收起悬浮窗 (Esc)", false, header_bar_);
    close_btn_->setObjectName("CloseBtn");
    close_btn_->setFixedSize(26, 26);
    connect(close_btn_, &QToolButton::clicked, this, &FloatingWindow::hideCard);

    hdr->addWidget(read_clip_btn_);
    hdr->addWidget(watch_clip_btn_);
    hdr->addWidget(result_only_btn_);
    hdr->addWidget(hdr_sep);
    hdr->addWidget(pin_btn_);
    hdr->addWidget(settings_btn_);
    hdr->addWidget(close_btn_);
    lay->addWidget(header_bar_);

    // Row 2: Dedicated language direction & translation preset strip
    control_bar_ = new QFrame(page);
    control_bar_->setObjectName("ControlBar");
    auto *ctrl = new QHBoxLayout(control_bar_);
    ctrl->setContentsMargins(8, 5, 8, 5);
    ctrl->setSpacing(6);

    source_combo_ = new QComboBox(control_bar_);
    source_combo_->setObjectName("HeaderCombo");
    source_combo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    source_combo_->setMinimumContentsLength(4);
    source_combo_->setMaximumWidth(114);
    source_combo_->setCursor(Qt::PointingHandCursor);
    populateSourceLangCombo(source_combo_, true);
    setupOpaqueComboPopup(source_combo_);
    source_combo_->setToolTip(
        "源语言（选「自动」可智能识别中/英/日/韩/俄/阿等 20 种语言并自动切换译向）");
    connect(source_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!updating_combos_) {
            const std::string src =
                source_combo_->currentData().toString().toStdString();
            config_mgr_.mutableConfig().floating_source_lang =
                src.empty() ? std::string("auto") : normalizeLanguageCode(src);
            config_mgr_.save();
            updateDirectionBadge();
            scheduleTranslation(true);
        }
    });

    swap_btn_ = makeIconToolBtn("⇄", "对调源语言与目标语言 (Alt+S)", false, control_bar_);
    swap_btn_->setObjectName("SwapBtn");
    swap_btn_->setFixedSize(26, 24);
    connect(swap_btn_, &QToolButton::clicked, this, &FloatingWindow::swapDirection);

    target_combo_ = new QComboBox(control_bar_);
    target_combo_->setObjectName("HeaderCombo");
    target_combo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    target_combo_->setMinimumContentsLength(4);
    target_combo_->setMaximumWidth(120);
    target_combo_->setCursor(Qt::PointingHandCursor);
    populateTargetLangCombo(target_combo_, true);
    setupOpaqueComboPopup(target_combo_);
    target_combo_->setToolTip("目标语言（支持 20 种语言）");
    connect(target_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!updating_combos_) {
            updateDirectionBadge();
            scheduleTranslation(true);
        }
    });

    preset_combo_ = new QComboBox(control_bar_);
    preset_combo_->setObjectName("PresetCombo");
    preset_combo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    preset_combo_->setMinimumContentsLength(9);
    preset_combo_->setMaximumWidth(210);
    preset_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    preset_combo_->setCursor(Qt::PointingHandCursor);
    setupOpaqueComboPopup(preset_combo_);
    preset_combo_->setToolTip("切换翻译模型 / API 预设");
    connect(preset_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!updating_combos_) {
            const std::string chosen =
                preset_combo_->currentData().toString().toStdString();
            if (!chosen.empty()) {
                config_mgr_.mutableConfig().active_preset = chosen;
                config_mgr_.save();
                if (const Preset *p = config_mgr_.config().findPreset(chosen)) {
                    const std::string effective_tgt =
                        !p->target_lang.empty()
                            ? normalizeLanguageCode(p->target_lang)
                            : (config_mgr_.config().target_lang.empty()
                                   ? std::string("en")
                                   : normalizeLanguageCode(
                                         config_mgr_.config().target_lang));
                    updating_combos_ = true;
                    selectComboByData(target_combo_,
                                      QString::fromStdString(effective_tgt));
                    updating_combos_ = false;
                }
                notifyFcitxReload();
                updateDirectionBadge();
                maybePrewarmLocalTyServer();
            }
            scheduleTranslation(true);
        }
    });

    ctrl->addWidget(source_combo_);
    ctrl->addWidget(swap_btn_);
    ctrl->addWidget(target_combo_);
    ctrl->addSpacing(4);
    ctrl->addWidget(preset_combo_, 1);
    lay->addWidget(control_bar_);

    // Stretchable splitter separating source editor and result pane
    splitter_ = new QSplitter(Qt::Vertical, page);
    splitter_->setObjectName("CardSplitter");
    splitter_->setHandleWidth(8);
    splitter_->setChildrenCollapsible(false);
    connect(splitter_, &QSplitter::splitterMoved, this, [this](int, int) {
        captureSplitterSizes();
        scheduleSaveUiState();
    });

    // Source draft editor
    source_edit_ = new QPlainTextEdit(splitter_);
    source_edit_->setObjectName("CardEditor");
    source_edit_->setPlaceholderText(
        "输入文本进行翻译或反向核对，或按 Alt+V 读取剪贴板…  (Ctrl+Enter 复制并收起 · Ctrl+滚轮缩放)");
    source_edit_->setMinimumHeight(54);
    source_edit_->installEventFilter(this);
    source_edit_->viewport()->installEventFilter(this);
    connect(source_edit_, &QPlainTextEdit::textChanged, this, [this]() {
        updateSourceCountLabel(source_edit_->toPlainText());
        scheduleTranslation(false);
    });
    connect(source_edit_->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int) { handleSourceScrollChanged(); });
    splitter_->addWidget(source_edit_);

    auto *bottom_pane = new QWidget(splitter_);
    auto *bottom_lay = new QVBoxLayout(bottom_pane);
    bottom_lay->setContentsMargins(0, 2, 0, 0);
    bottom_lay->setSpacing(6);

    // Compact result toolbar (stats + inline actions)
    auto *mid_row = new QHBoxLayout();
    mid_row->setContentsMargins(2, 0, 2, 0);
    mid_row->setSpacing(6);
    segment_badge_ = new QLabel(bottom_pane);
    segment_badge_->setObjectName("SegmentBadge");
    segment_badge_->hide();

    char_count_label_ = new QLabel("0 字", bottom_pane);
    char_count_label_->setObjectName("CountPill");
    clear_btn_ = makeIconToolBtn("⌫", "清空输入内容", false, bottom_pane);
    clear_btn_->setFixedSize(24, 24);
    connect(clear_btn_, &QToolButton::clicked, this, &FloatingWindow::clearDraft);

    next_seg_btn_ = new QPushButton("⇣ 译下段", bottom_pane);
    next_seg_btn_->setObjectName("CompactButton");
    next_seg_btn_->setToolTip("立即翻译下一段长文本（也可在结果框向下滚动触发）");
    next_seg_btn_->setCursor(Qt::PointingHandCursor);
    next_seg_btn_->hide();
    connect(next_seg_btn_, &QPushButton::clicked, this, [this]() {
        maybeLoadNextSegmentOnScroll();
        if (auto *sb = result_edit_->verticalScrollBar()) {
            sb->setValue(sb->maximum());
        }
    });

    retranslate_btn_ = new QPushButton("↻ 再次翻译", bottom_pane);
    retranslate_btn_->setObjectName("RetryButton");
    retranslate_btn_->setToolTip("跳过缓存并重新翻译当前文本 (Ctrl+R / F5)");
    retranslate_btn_->setCursor(Qt::PointingHandCursor);
    retranslate_btn_->setEnabled(false);
    connect(retranslate_btn_, &QPushButton::clicked, this,
            &FloatingWindow::retranslateNow);

    mid_row->addWidget(segment_badge_);
    mid_row->addWidget(char_count_label_);
    mid_row->addStretch(1);
    mid_row->addWidget(clear_btn_);
    mid_row->addWidget(next_seg_btn_);
    mid_row->addWidget(retranslate_btn_);
    bottom_lay->addLayout(mid_row);

    // Translation result box
    result_edit_ = new QPlainTextEdit(bottom_pane);
    result_edit_->setObjectName("CardResult");
    result_edit_->setReadOnly(true);
    result_edit_->setPlaceholderText("实时译文将显示在这里…");
    result_edit_->setMinimumHeight(56);
    result_edit_->installEventFilter(this);
    result_edit_->viewport()->installEventFilter(this);
    connect(result_edit_->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int val) {
                auto *sb = result_edit_->verticalScrollBar();
                if (sb && sb->maximum() > 0 && val >= sb->maximum() - 36 &&
                    isVisible() && stack_->currentWidget() == card_page_) {
                    maybeLoadNextSegmentOnScroll();
                }
            });
    bottom_lay->addWidget(result_edit_, 1);

    splitter_->addWidget(bottom_pane);
    splitter_->setStretchFactor(0, 4);
    splitter_->setStretchFactor(1, 5);
    lay->addWidget(splitter_, 1);

    // Footer actions + resize grip
    auto *footer = new QHBoxLayout();
    footer->setContentsMargins(0, 0, 0, 0);
    footer->setSpacing(6);

    status_label_ = new QLabel("按 Esc 收起 · Ctrl+Enter 复制并收起", page);
    status_label_->setObjectName("SubtleStatus");
    status_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    reverse_btn_ = new QPushButton("⇅ 反向重译", page);
    reverse_btn_->setObjectName("SecondaryButton");
    reverse_btn_->setToolTip(
        "将下方译文放回原文框并反向翻译回原语种，用来核对语义 (Alt+R)");
    reverse_btn_->setCursor(Qt::PointingHandCursor);
    reverse_btn_->setEnabled(false);
    connect(reverse_btn_, &QPushButton::clicked, this,
            &FloatingWindow::reverseTranslateResult);

    copy_btn_ = new QPushButton("📋 复制", page);
    copy_btn_->setObjectName("SecondaryButton");
    copy_btn_->setToolTip("复制译文到剪贴板 (Ctrl+Shift+C)");
    copy_btn_->setCursor(Qt::PointingHandCursor);
    copy_btn_->setEnabled(false);
    connect(copy_btn_, &QPushButton::clicked, this,
            [this]() { copyTranslation(false); });

    copy_close_btn_ = new QPushButton("✓ 复制并收起", page);
    copy_close_btn_->setObjectName("PrimaryButton");
    copy_close_btn_->setToolTip("复制译文并隐藏悬浮窗 (Ctrl+Enter)");
    copy_close_btn_->setCursor(Qt::PointingHandCursor);
    copy_close_btn_->setEnabled(false);
    connect(copy_close_btn_, &QPushButton::clicked, this,
            [this]() { copyTranslation(true); });

    reset_win_btn_ = makeIconToolBtn(
        "↺", "重置窗口位置与默认大小（回到屏幕居中，避免窗口跑偏）", false, page);
    reset_win_btn_->setFixedSize(24, 24);
    connect(reset_win_btn_, &QToolButton::clicked, this,
            [this]() { resetWindowGeometry(false); });

    size_grip_ = new QSizeGrip(page);
    size_grip_->setFixedSize(14, 14);

    footer->addWidget(status_label_, 1);
    footer->addWidget(reverse_btn_);
    footer->addWidget(copy_btn_);
    footer->addWidget(copy_close_btn_);
    footer->addWidget(reset_win_btn_);
    footer->addWidget(size_grip_, 0, Qt::AlignRight | Qt::AlignBottom);
    lay->addLayout(footer);

    return page;
}

void FloatingWindow::applyTheme() {
    const auto t = resolveThemeTokens();
    const int editor_px = std::max(10, qRound(13.0 * font_scale_ / 100.0));
    const int meta_px = std::max(10, qRound(11.0 * font_scale_ / 100.0));
    setStyleSheet(QString(
        "#CardRoot {"
        "  background-color: %1;"
        "  border: 1px solid %2;"
        "  border-radius: 14px;"
        "}"
        "#ControlBar {"
        "  background-color: %14;"
        "  border: 1px solid %2;"
        "  border-radius: 10px;"
        "}"
        "QFrame#HeaderSep {"
        "  background-color: %2;"
        "  border: none;"
        "}"
        "QLabel { color: %3; background: transparent; }"
        "QLabel#BrandGlyph { color: %4; }"
        "QLabel#BrandTitle {"
        "  color: %3;"
        "  font-size: 12.5px;"
        "  font-weight: 700;"
        "  letter-spacing: 0.2px;"
        "}"
        "QLabel#SubtleStatus { color: %5; font-size: %13px; }"
        "QLabel#DirectionBadge {"
        "  background: %6;"
        "  color: %4;"
        "  border: 1px solid %2;"
        "  border-radius: 6px;"
        "  padding: 2px 8px;"
        "  font-size: 11px;"
        "  font-weight: 600;"
        "}"
        "QLabel#SegmentBadge, QLabel#CountPill {"
        "  background: %7;"
        "  color: %5;"
        "  border: 1px solid %2;"
        "  border-radius: 6px;"
        "  padding: 1px 7px;"
        "  font-size: 11px;"
        "}"
        "QSplitter#CardSplitter::handle:vertical {"
        "  background: %2;"
        "  height: 2px;"
        "  margin: 3px 44px;"
        "  border-radius: 1px;"
        "}"
        "QSplitter#CardSplitter::handle:vertical:hover {"
        "  background: %4;"
        "  height: 3px;"
        "}"
        "QComboBox#HeaderCombo, QComboBox#PresetCombo {"
        "  background: %15;"
        "  color: %3;"
        "  border: 1px solid %2;"
        "  border-radius: 7px;"
        "  padding: 3px 22px 3px 9px;"
        "  min-height: 22px;"
        "  font-size: 11.5px;"
        "}"
        "QComboBox#HeaderCombo:hover, QComboBox#PresetCombo:hover {"
        "  border: 1px solid %4;"
        "}"
        "QComboBox#HeaderCombo::drop-down, QComboBox#PresetCombo::drop-down {"
        "  subcontrol-origin: padding;"
        "  subcontrol-position: center right;"
        "  width: 18px;"
        "  border: none;"
        "}"
        "QToolButton#HeaderToolBtn, QToolButton#SwapBtn, QToolButton#CloseBtn {"
        "  background: transparent;"
        "  color: %3;"
        "  border: 1px solid transparent;"
        "  border-radius: 7px;"
        "  font-size: 12.5px;"
        "}"
        "QToolButton#HeaderToolBtn:hover, QToolButton#SwapBtn:hover { background: %8; }"
        "QToolButton#SwapBtn {"
        "  background: %7;"
        "  border: 1px solid %2;"
        "}"
        "QToolButton#HeaderToolBtn:checked {"
        "  background: %6;"
        "  border: 1px solid %4;"
        "  color: %4;"
        "}"
        "QToolButton#CloseBtn:hover {"
        "  background: rgba(239,68,68,0.88);"
        "  color: #ffffff;"
        "}"
        "QPlainTextEdit#CardEditor {"
        "  background: %9;"
        "  color: %3;"
        "  border: 1px solid %2;"
        "  border-radius: 10px;"
        "  padding: 7px 9px;"
        "  font-size: %12px;"
        "}"
        "QPlainTextEdit#CardEditor:focus {"
        "  border: 1.5px solid %4;"
        "}"
        "QPlainTextEdit#CardResult {"
        "  background: %15;"
        "  color: %3;"
        "  border: 1px solid %2;"
        "  border-radius: 10px;"
        "  padding: 7px 9px;"
        "  font-size: %12px;"
        "}"
        "QScrollBar:vertical {"
        "  background: transparent;"
        "  width: 7px;"
        "  margin: 4px 1px;"
        "}"
        "QScrollBar::handle:vertical {"
        "  background: %2;"
        "  border-radius: 3px;"
        "  min-height: 20px;"
        "}"
        "QScrollBar::handle:vertical:hover { background: %5; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }"
        "QPushButton#SecondaryButton {"
        "  background: %7;"
        "  color: %3;"
        "  border: 1px solid %2;"
        "  border-radius: 8px;"
        "  padding: 5px 11px;"
        "  font-size: 12px;"
        "}"
        "QPushButton#SecondaryButton:hover:!disabled { background: %8; border: 1px solid %4; }"
        "QPushButton#SecondaryButton:disabled { color: %5; }"
        "QPushButton#RetryButton, QPushButton#CompactButton {"
        "  background: %7;"
        "  color: %3;"
        "  border: 1px solid %2;"
        "  border-radius: 7px;"
        "  padding: 3px 9px;"
        "  font-size: 11.5px;"
        "}"
        "QPushButton#RetryButton:hover:!disabled, QPushButton#CompactButton:hover:!disabled { background: %8; }"
        "QPushButton#RetryButton:disabled, QPushButton#CompactButton:disabled { color: %5; }"
        "QPushButton#RetryButton[retryNeeded=\"true\"]:!disabled {"
        "  background: %6;"
        "  color: %4;"
        "  border: 1px solid %4;"
        "  font-weight: 600;"
        "}"
        "QPushButton#RetryButton[retryNeeded=\"true\"]:hover:!disabled {"
        "  background: %8;"
        "}"
        "QPushButton#PrimaryButton {"
        "  background: %4;"
        "  color: %10;"
        "  border: none;"
        "  border-radius: 8px;"
        "  padding: 5px 13px;"
        "  font-size: 12px;"
        "  font-weight: 600;"
        "}"
        "QPushButton#PrimaryButton:hover:!disabled { background: %11; }"
        "QPushButton#PrimaryButton:disabled { background: %7; color: %5; }"
        "QToolTip {"
        "  background-color: %16;"
        "  color: %3;"
        "  border: 1px solid %17;"
        "  border-radius: 6px;"
        "  padding: 4px 8px;"
        "}")
                      .arg(cssRgba(t.surface), cssRgba(t.border),
                           t.text_primary.name(), t.accent.name(),
                           t.text_secondary.name(), cssRgba(t.accent_soft),
                           cssRgba(t.pill_bg), cssRgba(t.hover_bg),
                           cssRgba(t.surface_input), t.accent_text.name(),
                           t.accent_hover.name(),
                           QString::number(editor_px),
                           QString::number(meta_px),
                           cssRgba(t.card_strip),
                           cssRgba(t.surface_elevated),
                           t.popup_bg.name(),
                           t.popup_border.name()));
    applyOpaqueComboTheme(source_combo_, t);
    applyOpaqueComboTheme(target_combo_, t);
    applyOpaqueComboTheme(preset_combo_, t);
    if (settings_view_) {
        settings_view_->applyTheme();
    }
    if (orb_) {
        orb_->applyTheme();
    }
}

void FloatingWindow::populatePresetCombo() {
    updating_combos_ = true;
    preset_combo_->clear();
    for (const auto &p : config_mgr_.config().presets) {
        preset_combo_->addItem(QString::fromStdString(p.label),
                               QString::fromStdString(p.id));
    }
    selectComboByData(preset_combo_,
                      QString::fromStdString(config_mgr_.config().active_preset));
    updating_combos_ = false;
}

void FloatingWindow::applyConfigToUi(const ShadowConfig &cfg) {
    const Preset *active = cfg.findPreset(cfg.active_preset);
    const std::string src_lang =
        cfg.floating_source_lang.empty()
            ? std::string("auto")
            : normalizeLanguageCode(cfg.floating_source_lang);
    const std::string tgt_lang =
        (active && !active->target_lang.empty())
            ? normalizeLanguageCode(active->target_lang)
            : (cfg.target_lang.empty() ? std::string("en")
                                       : normalizeLanguageCode(cfg.target_lang));

    updating_combos_ = true;
    selectComboByData(source_combo_, QString::fromStdString(src_lang));
    selectComboByData(target_combo_, QString::fromStdString(tgt_lang));
    updating_combos_ = false;
    populatePresetCombo();

    watch_clipboard_ = cfg.floating_watch_clipboard;
    watch_clip_btn_->blockSignals(true);
    watch_clip_btn_->setChecked(watch_clipboard_);
    watch_clip_btn_->blockSignals(false);
    syncClipboardMonitorState();

    const int desired_scale = std::clamp(cfg.floating_font_scale, 80, 160);
    if (desired_scale != font_scale_) {
        adjustFontScale(desired_scale, false);
    }
    applyResultOnlyState(cfg.floating_result_only, false,
                         isVisible() && stack_->currentWidget() == card_page_);
    applyPinnedState(cfg.floating_pinned, false);
    updateFloatingToggleShortcuts(cfg.floating_hotkey);
    settings_view_->setConfig(cfg);
    translator_->setCacheCapacity(cfg.cache_size);
    translator_->reloadOfflineDictionaries(cfg.effectiveOfflineDicts());
    if (ty_mgr_) {
        ty_mgr_->setIdleUnloadSeconds(cfg.hymt_idle_unload_sec);
        if (isPopupActiveOnScreen()) {
            maybePrewarmLocalTyServer();
        }
    }
    syncOrbVisibility();
    updateDirectionBadge();
}

void FloatingWindow::syncOrbVisibility() {
    const auto &cfg = config_mgr_.config();
    const bool want_orb = cfg.floating_enabled && cfg.floating_show_orb;
    if (want_orb) {
        if (!orb_) {
            orb_ = std::make_unique<FloatingOrb>();
            connect(orb_.get(), &FloatingOrb::openCardRequested, this,
                    &FloatingWindow::Toggle);
            connect(orb_.get(), &FloatingOrb::readClipboardRequested, this,
                    [this]() { showCard(true); });
            connect(orb_.get(), &FloatingOrb::openSettingsRequested, this,
                    &FloatingWindow::ShowSettings);
            connect(orb_.get(), &FloatingOrb::resetWindowRequested, this,
                    [this]() { resetWindowGeometry(true); });
            connect(orb_.get(), &FloatingOrb::hideOrbRequested, this, [this]() {
                config_mgr_.mutableConfig().floating_show_orb = false;
                config_mgr_.save();
                notifyFcitxReload();
                if (orb_ && orb_->isVisible() && isWaylandPlatform()) {
                    requestKWinWindowGeometrySnapshots(
                        {orb_.get()},
                        QStringLiteral("org.fcitx.ShadowBarFloat"),
                        QStringLiteral("/Floating"),
                        QStringLiteral("org.fcitx.ShadowBarFloat"),
                        QStringLiteral("ReportWindowGeometry"),
                        [this](bool) {
                            QTimer::singleShot(25, this, [this]() {
                                saveUiStateNow();
                                syncOrbVisibility();
                            });
                        });
                    return;
                }
                if (orb_ && orb_->isVisible() && !isWaylandPlatform()) {
                    saved_orb_pos_ = orb_->pos();
                    has_saved_orb_pos_ = true;
                    saveUiStateNow();
                }
                syncOrbVisibility();
            });
            connect(orb_.get(), &FloatingOrb::quitRequested, this,
                    &FloatingWindow::QuitApp);
            connect(orb_.get(), &FloatingOrb::positionChanged, this,
                    [this](const QPoint &p) {
                        saved_orb_pos_ = p;
                        has_saved_orb_pos_ = true;
                        scheduleSaveUiState();
                    });
            connect(orb_.get(), &FloatingOrb::dragCompleted, this, [this]() {
                if (isWaylandPlatform()) {
                    scheduleWaylandGeometrySnapshot(220);
                }
            });
        }
        if (has_saved_orb_pos_) {
            orb_->setRememberedPosition(
                true, clampWindowPosToScreens(saved_orb_pos_, orb_->size()));
        } else {
            orb_->setRememberedPosition(false, QPoint{});
        }
        orb_->show();
    } else if (orb_) {
        if (orb_->isVisible() && !isWaylandPlatform()) {
            saved_orb_pos_ = orb_->pos();
            has_saved_orb_pos_ = true;
            saveUiStateNow();
        }
        orb_->hide();
    }
}

void FloatingWindow::handleInitialAction(const QString &action) {
    if (action == "--settings") {
        ShowSettings();
    } else if (action == "--sync-orb") {
        SyncOrb();
    } else if (action == "--wake-ty") {
        WakeTy();
    } else if (action == "--reset") {
        ResetWindow();
    } else if (action == "--show") {
        Show();
    } else {
        Toggle();
    }
}

void FloatingWindow::Toggle() {
    if (!isVisible() && last_internal_toggle_timer_.isValid() &&
        last_internal_toggle_timer_.elapsed() < 350) {
        return;
    }
    if (isVisible()) {
        hideCard();
        return;
    }
    Show();
}

void FloatingWindow::Show() {
    showCard(false);
}

void FloatingWindow::ShowSettings() {
    hiding_with_snapshot_ = false;
    ++wayland_snapshot_gen_;
    last_shown_timer_.restart();

    config_mgr_.reloadIfModified();
    applyConfigToUi(config_mgr_.config());
    showSettingsPage();
    if (has_saved_pos_ && !isWaylandPlatform()) {
        restoring_geometry_ = true;
        move(clampWindowPosToScreens(saved_pos_, size()));
        restoring_geometry_ = false;
    }
    show();
    raise();
    activateWindow();
}

void FloatingWindow::SyncOrb() {
    config_mgr_.reloadIfModified();
    applyConfigToUi(config_mgr_.config());
    const auto &cfg = config_mgr_.config();
    if (!isVisible() && !(cfg.floating_enabled && cfg.floating_show_orb)) {
        qApp->quit();
    }
}

namespace {

bool ensureTyServerWarm(const ShadowConfig &cfg, TyModelManager *ty_mgr,
                        const QString &ty_v) {
    if (!ty_mgr || ty_v.isEmpty()) {
        return false;
    }
    const QString bin = resolvedTyServerBinary(cfg);
    const QString mdl = resolvedTyModelPath(cfg, ty_v);
    if (!QFileInfo::exists(bin) || !QFileInfo::exists(mdl)) {
        return false;
    }
    if (ty_mgr->isReadyFor(ty_v, mdl, cfg.hymt_port,
                           QString::fromStdString(cfg.hymt_backend))) {
        ty_mgr->markActivity();
        return true;
    }
    return ty_mgr->startServer(bin, ty_v, mdl, cfg.hymt_port, cfg.hymt_threads,
                               QString::fromStdString(cfg.hymt_backend));
}

} // namespace

void FloatingWindow::WakeTy() {
    config_mgr_.reloadIfModified();
    applyConfigToUi(config_mgr_.config());
    const auto &cfg = config_mgr_.config();
    if (ty_mgr_ && cfg.hymt_auto_start) {
        const Preset *target_preset = cfg.findPreset(cfg.active_preset);
        QString ty_v =
            target_preset ? inferTyVariantForPreset(*target_preset, cfg.hymt_port)
                          : QString();
        if (ty_v.isEmpty()) {
            for (const auto &p : cfg.presets) {
                ty_v = inferTyVariantForPreset(p, cfg.hymt_port);
                if (!ty_v.isEmpty()) {
                    break;
                }
            }
        }
        ensureTyServerWarm(cfg, ty_mgr_.get(), ty_v);
    }
    const bool ty_active =
        ty_mgr_ &&
        (ty_mgr_->state() == TyModelManager::ServerState::Starting ||
         ty_mgr_->state() == TyModelManager::ServerState::Running ||
         ty_mgr_->state() == TyModelManager::ServerState::ExternalRunning);
    if (!ty_active && !isVisible() &&
        !(cfg.floating_enabled && cfg.floating_show_orb)) {
        qApp->quit();
    }
}

void FloatingWindow::ResetWindow() {
    resetWindowGeometry(true);
}

void FloatingWindow::QuitApp() {
    if (quitting_with_snapshot_) {
        return;
    }
    captureLiveWidgetGeometry();
    captureSplitterSizes();

    const QList<QWidget *> visible = visibleSnapshotWidgets();
    if (isWaylandPlatform() && !visible.isEmpty()) {
        quitting_with_snapshot_ = true;
        cancelPopupTranslation();
        syncClipboardMonitorState();
        const uint64_t snap_gen = ++wayland_snapshot_gen_;
        waiting_quit_gen_ = snap_gen;
        pending_quit_captions_.clear();
        for (QWidget *w : visible) {
            pending_quit_captions_.insert(w->windowTitle());
        }
        QTimer::singleShot(90, this, [this, snap_gen]() {
            finishPendingQuit(snap_gen);
        });
        requestKWinWindowGeometrySnapshots(
            visible,
            QStringLiteral("org.fcitx.ShadowBarFloat"),
            QStringLiteral("/Floating"),
            QStringLiteral("org.fcitx.ShadowBarFloat"),
            QStringLiteral("ReportWindowGeometry"),
            [this, snap_gen](bool dispatched) {
                if (!dispatched) {
                    finishPendingQuit(snap_gen);
                }
            });
        return;
    }

    saveUiStateNow();
    cancelPopupTranslation();
    if (clip_monitor_) {
        clip_monitor_->setWatchEnabled(false);
    }
    if (orb_) {
        orb_->hide();
    }
    qApp->quit();
}

void FloatingWindow::ReportWindowGeometry(const QString &caption,
                                          const QString &geometry_csv) {
    const QStringList parts = geometry_csv.split(',');
    if (parts.size() < 2) {
        return;
    }
    bool ok_x = false;
    bool ok_y = false;
    const int x = parts[0].trimmed().toInt(&ok_x);
    const int y = parts[1].trimmed().toInt(&ok_y);
    if (!ok_x || !ok_y) {
        return;
    }

    if (caption == windowTitle()) {
        saved_pos_ = QPoint(x, y);
        has_saved_pos_ = true;
        scheduleSaveUiState();
        if (hiding_with_snapshot_) {
            finishPendingHide(waiting_card_geom_gen_);
        }
    } else if (orb_ && caption == orb_->windowTitle()) {
        saved_orb_pos_ = QPoint(x, y);
        has_saved_orb_pos_ = true;
        scheduleSaveUiState();
    }

    if (quitting_with_snapshot_) {
        pending_quit_captions_.remove(caption);
        if (pending_quit_captions_.isEmpty()) {
            finishPendingQuit(waiting_quit_gen_);
        }
    }
}

void FloatingWindow::showCard(bool force_read_clipboard) {
    hiding_with_snapshot_ = false;
    ++wayland_snapshot_gen_;
    last_shown_timer_.restart();

    config_mgr_.reloadIfModified();
    applyConfigToUi(config_mgr_.config());
    returnToCardPage();
    applyCurrentPageBounds(true);

    if (has_saved_pos_ && !isWaylandPlatform()) {
        restoring_geometry_ = true;
        move(clampWindowPosToScreens(saved_pos_, size()));
        restoring_geometry_ = false;
    }

    show();
    raise();
    activateWindow();
    if (result_only_) {
        result_edit_->setFocus(Qt::ShortcutFocusReason);
    } else {
        source_edit_->setFocus(Qt::ShortcutFocusReason);
    }
    syncClipboardMonitorState();
    maybePrewarmLocalTyServer();

    const bool auto_read = config_mgr_.config().floating_auto_read_clipboard;
    if (force_read_clipboard || auto_read) {
        readClipboardAndTranslate(force_read_clipboard);
    }
}

void FloatingWindow::showSettingsPage() {
    if (stack_->currentWidget() != settings_view_) {
        captureLiveWidgetGeometry();
        captureSplitterSizes();
        settings_view_->setConfig(config_mgr_.config());
        stack_->setCurrentWidget(settings_view_);
        applyCurrentPageBounds(true);
        syncClipboardMonitorState();
        scheduleSaveUiState();
    }
}

void FloatingWindow::returnToCardPage() {
    if (stack_->currentWidget() != card_page_) {
        captureLiveWidgetGeometry();
        stack_->setCurrentWidget(card_page_);
        applyCurrentPageBounds(true);
        syncClipboardMonitorState();
        scheduleSaveUiState();
    }
}

void FloatingWindow::hideCard() {
    if (!isVisible()) {
        return;
    }
    captureLiveWidgetGeometry();
    captureSplitterSizes();
    cancelPopupTranslation();

    if (isWaylandPlatform() && !hiding_with_snapshot_) {
        hiding_with_snapshot_ = true;
        syncClipboardMonitorState();
        const uint64_t snap_gen = ++wayland_snapshot_gen_;
        waiting_card_geom_gen_ = snap_gen;
        QTimer::singleShot(65, this, [this, snap_gen]() {
            finishPendingHide(snap_gen);
        });
        requestKWinWindowGeometrySnapshots(
            visibleSnapshotWidgets(),
            QStringLiteral("org.fcitx.ShadowBarFloat"),
            QStringLiteral("/Floating"),
            QStringLiteral("org.fcitx.ShadowBarFloat"),
            QStringLiteral("ReportWindowGeometry"),
            [this, snap_gen](bool dispatched) {
                if (!dispatched) {
                    finishPendingHide(snap_gen);
                }
            });
        return;
    }

    saveUiStateNow();
    hide();
    syncClipboardMonitorState();
}

bool FloatingWindow::isPopupActiveOnScreen() const {
    return isVisible() && !isMinimized() && !hiding_with_snapshot_ &&
           !quitting_with_snapshot_ && stack_ &&
           stack_->currentWidget() == card_page_;
}

void FloatingWindow::cancelPopupTranslation() {
    ++active_revision_;
    waiting_ty_server_ = false;
    active_segment_idx_ = -1;
    segment_queue_.clear();
    setPopupTyBusy(false);
    if (translator_) {
        translator_->cancelAll();
    }
    bool cleared_loading = false;
    for (auto &seg : segments_) {
        if (seg.loading) {
            seg.loading = false;
            cleared_loading = true;
        }
    }
    if (cleared_loading) {
        renderSegmentedResult();
    }
}

void FloatingWindow::setPopupTyBusy(bool busy) {
    if (ty_translation_busy_ == busy) {
        return;
    }
    ty_translation_busy_ = busy;
    if (!ty_mgr_) {
        return;
    }
    if (busy) {
        ty_mgr_->beginTranslationActivity();
    } else {
        ty_mgr_->endTranslationActivity();
    }
}

void FloatingWindow::maybePrewarmLocalTyServer() {
    if (!ty_mgr_) {
        return;
    }
    const auto &cfg = config_mgr_.config();
    if (!cfg.hymt_auto_start) {
        return;
    }
    const Preset *active = nullptr;
    if (preset_combo_ && preset_combo_->count() > 0) {
        const std::string cur_id =
            preset_combo_->currentData().toString().toStdString();
        active = cfg.findPreset(cur_id);
    }
    if (!active) {
        active = cfg.findPreset(cfg.active_preset);
    }
    if (!active) {
        return;
    }
    ensureTyServerWarm(cfg, ty_mgr_.get(),
                       inferTyVariantForPreset(*active, cfg.hymt_port));
}

void FloatingWindow::syncClipboardMonitorState() {
    if (!clip_monitor_) {
        return;
    }
    clip_monitor_->setWatchEnabled(watch_clipboard_ && isPopupActiveOnScreen());
}

void FloatingWindow::readClipboardAndTranslate(bool force) {
    if (clip_monitor_) {
        clip_monitor_->requestProbe(force);
    }
}

void FloatingWindow::applyClipboardText(const QString &text, bool force) {
    if (!force && (!isVisible() || !watch_clipboard_ ||
                   stack_->currentWidget() != card_page_)) {
        return;
    }
    QString clip = text.trimmed();
    if (clip.isEmpty()) {
        if (force) {
            status_label_->setText("剪贴板为空，请直接输入文本");
        }
        return;
    }
    if (clip.size() > 200000) {
        clip.truncate(200000);
    }

    const bool has_existing = !source_edit_->toPlainText().trimmed().isEmpty();
    if (!force && !last_copied_text_.isEmpty() && clip == last_copied_text_) {
        return;
    }
    if (!force && has_existing && clip == last_auto_loaded_clip_) {
        if (last_translation_failed_) {
            dispatchTranslation(true, true);
        }
        return;
    }

    last_auto_loaded_clip_ = clip;
    source_edit_->blockSignals(true);
    source_edit_->setPlainText(clip);
    source_edit_->blockSignals(false);
    updateSourceCountLabel(clip);
    scheduleTranslation(true);
}

void FloatingWindow::onClipboardAvoided(ClipboardAvoidReason reason, bool force) {
    if (!force && (!isVisible() || !watch_clipboard_ ||
                   stack_->currentWidget() != card_page_)) {
        return;
    }
    if (reason == ClipboardAvoidReason::Empty && !force) {
        return;
    }
    const QString hint = clipboardAvoidReasonText(reason);
    if (!hint.isEmpty() && status_label_) {
        status_label_->setText(hint);
    }
}

std::pair<std::string, std::string>
FloatingWindow::currentEffectiveDirection() const {
    const std::string text = source_edit_->toPlainText().trimmed().toStdString();
    const std::string src = source_combo_->currentData().toString().toStdString();
    const std::string tgt = target_combo_->currentData().toString().toStdString();
    return resolveTranslationDirection(
        text, src, tgt, config_mgr_.config().auto_detect_direction);
}

void FloatingWindow::updateDirectionBadge() {
    const auto [src, tgt] = currentEffectiveDirection();
    const QString preset_lbl = preset_combo_->currentText();
    dir_badge_->setText(QString("%1 → %2 · %3")
                            .arg(QString::fromStdString(formatLanguageLabel(src)),
                                 QString::fromStdString(formatLanguageLabel(tgt)),
                                 preset_lbl));
}

void FloatingWindow::scheduleTranslation(bool immediate) {
    updateDirectionBadge();
    const QString raw = source_edit_->toPlainText().trimmed();
    if (raw.isEmpty()) {
        ++active_revision_;
        waiting_ty_server_ = false;
        translator_->cancelAll();
        active_segment_idx_ = -1;
        segment_queue_.clear();
        setPopupTyBusy(false);
        segments_.clear();
        last_split_context_key_.clear();
        current_translation_.clear();
        last_translation_failed_ = false;
        result_edit_->clear();
        segment_badge_->hide();
        next_seg_btn_->hide();
        retranslate_btn_->setEnabled(false);
        setRetryHighlight(false);
        reverse_btn_->setEnabled(false);
        copy_btn_->setEnabled(false);
        copy_close_btn_->setEnabled(false);
        status_label_->setText(pinned_ ? "已置顶 · 输入或读取剪贴板"
                                       : "按 Esc 收起 · Ctrl+Enter 复制并收起");
        return;
    }

    retranslate_btn_->setEnabled(true);
    bool trigger_now = immediate;
    if (!trigger_now && raw.size() >= 1) {
        const QChar last = raw.back();
        if (QString("。！？!?.；;\n").contains(last)) {
            trigger_now = true;
        }
    }
    dispatchTranslation(trigger_now, false);
}

void FloatingWindow::retranslateNow() {
    const QString raw = source_edit_->toPlainText().trimmed();
    if (raw.isEmpty()) {
        status_label_->setText("请先输入或粘贴需要翻译的文本");
        if (!result_only_) {
            source_edit_->setFocus();
        }
        return;
    }
    dispatchTranslation(true, true);
}

void FloatingWindow::setRetryHighlight(bool highlight) {
    if (!retranslate_btn_) {
        return;
    }
    if (retranslate_btn_->property("retryNeeded").toBool() == highlight) {
        return;
    }
    retranslate_btn_->setProperty("retryNeeded", highlight);
    retranslate_btn_->style()->unpolish(retranslate_btn_);
    retranslate_btn_->style()->polish(retranslate_btn_);
    retranslate_btn_->update();
}

int FloatingWindow::firstUntranslatedSegmentIndex() const {
    for (int i = 0; i < static_cast<int>(segments_.size()); ++i) {
        if (!segments_[i].translated && !segments_[i].loading &&
            !segments_[i].failed) {
            return i;
        }
    }
    return -1;
}

void FloatingWindow::dispatchTranslation(bool immediate, bool bypass_cache) {
    const std::string text = source_edit_->toPlainText().trimmed().toStdString();
    if (text.empty()) {
        return;
    }

    updateDirectionBadge();
    retranslate_btn_->setEnabled(true);
    setRetryHighlight(false);
    last_translation_failed_ = false;

    const auto &cfg = config_mgr_.config();
    const std::string preset_id =
        preset_combo_->currentData().toString().toStdString();
    auto [eff_src, eff_tgt] = currentEffectiveDirection();
    last_effective_source_ = eff_src;
    last_effective_target_ = eff_tgt;

    const size_t max_chars =
        static_cast<size_t>(std::clamp(cfg.floating_segment_chars, 150, 4000));
    std::vector<std::string> chunks =
        splitTextIntoScrollChunks(text, max_chars);
    if (chunks.empty()) {
        return;
    }

    const std::string context_key =
        preset_id + "|" + eff_src + "->" + eff_tgt + "|" + std::to_string(max_chars);
    const bool same_context =
        (!bypass_cache && context_key == last_split_context_key_);
    last_split_context_key_ = context_key;

    std::vector<TranslationSegment> next_segments;
    next_segments.reserve(chunks.size());
    int highest_loaded = 0;
    for (size_t i = 0; i < chunks.size(); ++i) {
        TranslationSegment seg;
        seg.source = QString::fromStdString(chunks[i]);
        if (same_context && i < segments_.size() &&
            segments_[i].source == seg.source &&
            (segments_[i].translated || segments_[i].failed)) {
            seg.translation = segments_[i].translation;
            seg.translated = segments_[i].translated;
            seg.failed = segments_[i].failed;
            seg.from_offline = segments_[i].from_offline;
            seg.error = segments_[i].error;
            highest_loaded = static_cast<int>(i);
        }
        next_segments.push_back(std::move(seg));
    }

    ++active_revision_;
    waiting_ty_server_ = false;
    translator_->cancelAll();
    active_segment_idx_ = -1;
    segment_queue_.clear();
    setPopupTyBusy(false);
    segments_ = std::move(next_segments);
    queue_bypass_cache_ = bypass_cache;

    if (bypass_cache) {
        bool queued_failed_only = false;
        for (int i = 0; i <= highest_loaded && i < static_cast<int>(segments_.size());
             ++i) {
            if (segments_[i].failed) {
                segments_[i].failed = false;
                segments_[i].error.clear();
                segment_queue_.push_back(static_cast<size_t>(i));
                queued_failed_only = true;
            }
        }
        if (!queued_failed_only) {
            for (int i = 0;
                 i <= highest_loaded && i < static_cast<int>(segments_.size()); ++i) {
                segments_[i].translated = false;
                segments_[i].failed = false;
                segments_[i].error.clear();
                segment_queue_.push_back(static_cast<size_t>(i));
            }
        }
    } else {
        for (int i = 0; i <= highest_loaded && i < static_cast<int>(segments_.size());
             ++i) {
            if (!segments_[i].translated) {
                segment_queue_.push_back(static_cast<size_t>(i));
            }
        }
    }

    renderSegmentedResult();
    if (!segment_queue_.empty()) {
        pumpNextSegmentTranslation(immediate);
    }
}

void FloatingWindow::pumpNextSegmentTranslation(bool immediate) {
    if (active_segment_idx_ >= 0) {
        return;
    }
    if (segment_queue_.empty()) {
        setPopupTyBusy(false);
        return;
    }

    const size_t seg_idx = segment_queue_.front();
    segment_queue_.pop_front();
    if (seg_idx >= segments_.size()) {
        setPopupTyBusy(false);
        return;
    }

    const auto &cfg = config_mgr_.config();
    const std::string preset_id =
        preset_combo_->currentData().toString().toStdString();
    Preset preset;
    if (const auto *p = cfg.findPreset(preset_id)) {
        preset = *p;
    } else if (!cfg.presets.empty()) {
        preset = cfg.presets.front();
    }

    auto [eff_src, eff_tgt] = currentEffectiveDirection();
    last_effective_source_ = eff_src;
    last_effective_target_ = eff_tgt;
    preset.target_lang = eff_tgt;

    const QString ty_variant = inferTyVariantForPreset(preset, cfg.hymt_port);
    if (!ty_variant.isEmpty() && cfg.hymt_auto_start && ty_mgr_) {
        const QString bin = resolvedTyServerBinary(cfg);
        const QString mdl = resolvedTyModelPath(cfg, ty_variant);
        if (!ty_mgr_->isReadyFor(ty_variant, mdl, cfg.hymt_port,
                                 QString::fromStdString(cfg.hymt_backend))) {
            if (!ty_mgr_->startServer(bin, ty_variant, mdl, cfg.hymt_port,
                                      cfg.hymt_threads,
                                      QString::fromStdString(cfg.hymt_backend))) {
                setPopupTyBusy(false);
                segments_[seg_idx].loading = false;
                segments_[seg_idx].failed = true;
                segments_[seg_idx].error = ty_mgr_->statusMessage();
                active_segment_idx_ = static_cast<int>(seg_idx);
                renderSegmentedResult();
                setRetryHighlight(true);
                if (status_label_) {
                    status_label_->setText(
                        QString("⚠ %1").arg(ty_mgr_->statusMessage()));
                }
                return;
            }
            if (ty_mgr_->state() == TyModelManager::ServerState::Starting) {
                setPopupTyBusy(false);
                ty_mgr_->markActivity();
                waiting_ty_server_ = true;
                segment_queue_.push_front(seg_idx);
                if (status_label_) {
                    status_label_->setText(
                        QString("⏳ 正在加载本地混元 HY-MT1.5 (%1)…")
                            .arg(ty_variant));
                }
                return;
            }
        }
    }

    setPopupTyBusy(!ty_variant.isEmpty());
    if (!ty_variant.isEmpty() && ty_mgr_) {
        ty_mgr_->markActivity();
    }

    segments_[seg_idx].loading = true;
    segments_[seg_idx].failed = false;
    segments_[seg_idx].error.clear();
    if (queue_bypass_cache_) {
        segments_[seg_idx].translation.clear();
        segments_[seg_idx].translated = false;
    }
    active_segment_idx_ = static_cast<int>(seg_idx);
    renderSegmentedResult();

    const uint64_t rev = ++active_revision_;
    if (segments_.size() > 1) {
        status_label_->setText(
            QString("⏳ 正在翻译第 %1/%2 段 (%3→%4)…")
                .arg(seg_idx + 1)
                .arg(segments_.size())
                .arg(QString::fromStdString(formatLanguageLabel(eff_src)),
                     QString::fromStdString(formatLanguageLabel(eff_tgt))));
    } else {
        status_label_->setText(
            QString(queue_bypass_cache_ ? "⏳ 正在重新翻译 (%1→%2)…"
                                        : "⏳ 正在翻译 (%1→%2)…")
                .arg(QString::fromStdString(formatLanguageLabel(eff_src)),
                     QString::fromStdString(formatLanguageLabel(eff_tgt))));
    }

    const int effective_timeout_ms =
        (preset.provider == ProviderKind::OpenAICompatible ||
         preset.provider == ProviderKind::Ollama ||
         preset.provider == ProviderKind::DeepL)
            ? std::max(cfg.request_timeout_ms,
                       isHyMtModelName(preset.model) ? 30000 : 20000)
            : cfg.request_timeout_ms;

    TranslationJob job{
        .revision = rev,
        .source_text = segments_[seg_idx].source.toStdString(),
        .source_lang = eff_src,
        .preset = std::move(preset),
        .immediate = immediate || (seg_idx > 0),
        // Floating segments are coherent paragraphs; translate each segment in
        // one streaming request rather than serial per-sentence calls.
        .lock_on_punctuation = false,
        .offline_fallback = cfg.offline_dict_fallback,
        .bypass_cache = queue_bypass_cache_,
        .debounce_ms = cfg.debounce_ms,
        .timeout_ms = effective_timeout_ms,
    };

    QPointer<FloatingWindow> self(this);
    translator_->submit(
        std::move(job),
        [self, seg_idx](uint64_t res_rev, const TranslationResult &res) {
            if (!self) {
                return;
            }
            QMetaObject::invokeMethod(
                self.data(),
                [self, res_rev, seg_idx, res]() {
                    if (!self || res_rev != self->active_revision_ ||
                        seg_idx >= self->segments_.size()) {
                        return;
                    }
                    auto &seg = self->segments_[seg_idx];
                    if (!res.is_final) {
                        seg.translation = QString::fromStdString(res.translated_text);
                        self->renderSegmentedResult();
                        return;
                    }

                    seg.loading = false;
                    self->active_segment_idx_ = -1;
                    if (res.ok) {
                        seg.translated = true;
                        seg.failed = false;
                        seg.from_offline = res.from_offline_dict;
                        seg.error.clear();
                        seg.translation = QString::fromStdString(res.translated_text);
                    } else {
                        seg.translated = false;
                        seg.failed = true;
                        seg.from_offline = false;
                        seg.error = QString::fromStdString(res.error_message);
                    }
                    self->renderSegmentedResult();
                    if (!self->segment_queue_.empty()) {
                        self->pumpNextSegmentTranslation(true);
                    } else {
                        self->setPopupTyBusy(false);
                    }
                },
                Qt::QueuedConnection);
        });
}

void FloatingWindow::maybeLoadNextSegmentOnScroll() {
    if (segments_.size() <= 1 || active_segment_idx_ >= 0 ||
        !segment_queue_.empty()) {
        return;
    }
    const int next_idx = firstUntranslatedSegmentIndex();
    if (next_idx < 0) {
        return;
    }
    segment_queue_.push_back(static_cast<size_t>(next_idx));
    queue_bypass_cache_ = false;
    pumpNextSegmentTranslation(true);
}

void FloatingWindow::handleSourceScrollChanged() {
    if (result_only_ || segments_.size() <= 1 || active_segment_idx_ >= 0 ||
        !segment_queue_.empty()) {
        return;
    }
    auto *sb = source_edit_->verticalScrollBar();
    if (!sb || sb->maximum() <= 0) {
        return;
    }
    const double ratio =
        static_cast<double>(sb->value()) / static_cast<double>(sb->maximum());
    const int target_idx = std::min<int>(
        static_cast<int>(segments_.size()) - 1,
        static_cast<int>(ratio * static_cast<double>(segments_.size())));
    const int next_idx = firstUntranslatedSegmentIndex();
    if (next_idx >= 0 && next_idx <= target_idx) {
        maybeLoadNextSegmentOnScroll();
    }
}

void FloatingWindow::renderSegmentedResult() {
    if (segments_.empty()) {
        current_translation_.clear();
        result_edit_->clear();
        segment_badge_->hide();
        next_seg_btn_->hide();
        reverse_btn_->setEnabled(false);
        copy_btn_->setEnabled(false);
        copy_close_btn_->setEnabled(false);
        return;
    }

    auto *sb = result_edit_->verticalScrollBar();
    const int prev_scroll = sb ? sb->value() : 0;

    if (segments_.size() == 1) {
        segment_badge_->hide();
        next_seg_btn_->hide();
        const auto &seg = segments_.front();
        if (seg.loading && seg.translation.isEmpty()) {
            // Keep placeholder visible while first chunk awaits initial token.
            reverse_btn_->setEnabled(false);
            copy_btn_->setEnabled(false);
            copy_close_btn_->setEnabled(false);
            return;
        }
        if (seg.failed) {
            last_translation_failed_ = true;
            setRetryHighlight(true);
            current_translation_.clear();
            result_edit_->setPlainText(
                QString("翻译失败: %1").arg(seg.error));
            reverse_btn_->setEnabled(false);
            copy_btn_->setEnabled(false);
            copy_close_btn_->setEnabled(false);
            status_label_->setText("请求失败 · 点击「再次翻译」或按 Ctrl+R 重试");
            return;
        }
        last_translation_failed_ = false;
        setRetryHighlight(false);
        current_translation_ = seg.translation.toStdString();
        result_edit_->setPlainText(seg.translation);
        const bool has_out = !seg.translation.trimmed().isEmpty();
        reverse_btn_->setEnabled(has_out && !seg.loading);
        copy_btn_->setEnabled(has_out && !seg.loading);
        copy_close_btn_->setEnabled(has_out && !seg.loading);
        if (!seg.loading) {
            status_label_->setText(
                seg.from_offline
                    ? "已使用内置离线词典 · Ctrl+Enter 复制并收起"
                    : "翻译完成 · ⇅ 可反向重译核对");
        }
        if (sb) {
            sb->setValue(std::min(prev_scroll, sb->maximum()));
        }
        return;
    }

    int highest_active = 0;
    int translated_count = 0;
    int untranslated_count = 0;
    bool any_failed = false;
    bool any_offline = false;
    for (int i = 0; i < static_cast<int>(segments_.size()); ++i) {
        const auto &s = segments_[i];
        if (s.translated) {
            ++translated_count;
            highest_active = i;
            if (s.from_offline) {
                any_offline = true;
            }
        } else if (s.loading || s.failed) {
            highest_active = i;
            if (s.failed) {
                any_failed = true;
            }
        } else {
            ++untranslated_count;
        }
    }

    QStringList clean_parts;
    QStringList view_parts;
    for (int i = 0; i <= highest_active; ++i) {
        const auto &s = segments_[i];
        if (s.translated) {
            if (!s.translation.trimmed().isEmpty()) {
                clean_parts.append(s.translation);
                view_parts.append(s.translation);
            }
        } else if (s.loading) {
            view_parts.append(
                s.translation.trimmed().isEmpty()
                    ? QString("⏳ 正在翻译第 %1/%2 段…")
                          .arg(i + 1)
                          .arg(segments_.size())
                    : (s.translation + " …"));
        } else if (s.failed) {
            view_parts.append(QString("❌ 第 %1/%2 段翻译失败: %3")
                                  .arg(i + 1)
                                  .arg(segments_.size())
                                  .arg(s.error));
        }
    }

    const int next_idx = firstUntranslatedSegmentIndex();
    if (next_idx >= 0) {
        view_parts.append(
            QString("┄┄ 还有 %1 段待译（第 %2/%3 段 · 约 %4 字）· 向下滚动继续翻译 ┄┄")
                .arg(untranslated_count)
                .arg(next_idx + 1)
                .arg(segments_.size())
                .arg(segments_[next_idx].source.size()));
    }

    const QString clean_joined = clean_parts.join("\n\n");
    current_translation_ = clean_joined.toStdString();
    result_edit_->setPlainText(view_parts.join("\n\n"));
    if (sb) {
        sb->setValue(std::min(prev_scroll, sb->maximum()));
    }

    segment_badge_->setText(
        QString("第 %1/%2 段").arg(translated_count).arg(segments_.size()));
    segment_badge_->show();
    next_seg_btn_->setVisible(next_idx >= 0 && active_segment_idx_ < 0);

    last_translation_failed_ = any_failed;
    setRetryHighlight(any_failed);

    const bool has_out = !clean_joined.trimmed().isEmpty();
    reverse_btn_->setEnabled(has_out && active_segment_idx_ < 0);
    copy_btn_->setEnabled(has_out);
    copy_close_btn_->setEnabled(has_out);

    if (active_segment_idx_ < 0) {
        if (any_failed) {
            status_label_->setText(
                "部分分段请求失败 · 点击「再次翻译」或按 Ctrl+R 重试失败段落");
        } else if (next_idx >= 0) {
            status_label_->setText(
                QString("已译 %1/%2 段 · 向下滚动或点「译下段」继续")
                    .arg(translated_count)
                    .arg(segments_.size()));
        } else {
            status_label_->setText(
                any_offline
                    ? QString("全部 %1 段完成（含离线词典）· Ctrl+Enter 复制")
                          .arg(segments_.size())
                    : QString("全部 %1 段翻译完成 · Ctrl+Enter 复制并收起")
                          .arg(segments_.size()));
        }
    }
}

void FloatingWindow::swapDirection() {
    auto [eff_src, eff_tgt] = currentEffectiveDirection();
    if (eff_src == "auto" || eff_src.empty()) {
        eff_src = (eff_tgt == "zh") ? "en" : "zh";
    }
    updating_combos_ = true;
    selectComboByData(source_combo_, QString::fromStdString(eff_tgt));
    selectComboByData(target_combo_, QString::fromStdString(eff_src));
    updating_combos_ = false;
    config_mgr_.mutableConfig().floating_source_lang = normalizeLanguageCode(eff_tgt);
    config_mgr_.save();
    updateDirectionBadge();
    scheduleTranslation(true);
}

void FloatingWindow::reverseTranslateResult() {
    if (current_translation_.empty()) {
        return;
    }
    std::string back_target = normalizeLanguageCode(last_effective_source_);
    if (back_target.empty() || back_target == "auto" ||
        back_target == last_effective_target_) {
        const std::string explicit_src =
            source_combo_->currentData().toString().toStdString();
        if (!explicit_src.empty() && explicit_src != "auto" &&
            explicit_src != last_effective_target_) {
            back_target = normalizeLanguageCode(explicit_src);
        } else if (!config_mgr_.config().target_lang.empty() &&
                   normalizeLanguageCode(config_mgr_.config().target_lang) !=
                       last_effective_target_) {
            back_target = normalizeLanguageCode(config_mgr_.config().target_lang);
        } else {
            back_target = (last_effective_target_ == "zh") ? "en" : "zh";
        }
    }

    const QString reversed_input = QString::fromStdString(current_translation_);
    updating_combos_ = true;
    selectComboByData(source_combo_, "auto");
    selectComboByData(target_combo_, QString::fromStdString(back_target));
    updating_combos_ = false;
    updateDirectionBadge();

    last_auto_loaded_clip_ = reversed_input;
    source_edit_->blockSignals(true);
    source_edit_->setPlainText(reversed_input);
    source_edit_->blockSignals(false);
    updateSourceCountLabel(reversed_input);
    dispatchTranslation(true);
}

void FloatingWindow::updateSourceCountLabel(const QString &text) {
    if (!char_count_label_) {
        return;
    }
    const auto stats = countTranslationUnits(text.toStdString());
    if (stats.total_units == 0) {
        char_count_label_->setText("0 字");
    } else if (stats.cjk_chars > 0 && stats.word_count > 0) {
        char_count_label_->setText(
            QString("%1 字 · %2 词").arg(stats.cjk_chars).arg(stats.word_count));
    } else if (stats.word_count > 0) {
        char_count_label_->setText(QString("%1 词").arg(stats.word_count));
    } else {
        char_count_label_->setText(QString("%1 字").arg(stats.cjk_chars));
    }
}

void FloatingWindow::copyTranslation(bool hide_after) {
    if (current_translation_.empty()) {
        return;
    }
    const QString text = QString::fromStdString(current_translation_);
    last_copied_text_ = text;
    if (clip_monitor_) {
        clip_monitor_->markSelfCopiedText(text);
    }
    if (QClipboard *cb = QGuiApplication::clipboard()) {
        cb->setText(text, QClipboard::Clipboard);
    }
    if (hide_after) {
        hideCard();
        return;
    }
    copy_btn_->setText("✓ 已复制");
    status_label_->setText("已复制到剪贴板");
    copy_flash_timer_->start(1300);
}

void FloatingWindow::clearDraft() {
    source_edit_->clear();
    source_edit_->setFocus();
}

void FloatingWindow::applyPinnedState(bool pinned, bool persist) {
    const bool changed = (pinned_ != pinned);
    pinned_ = pinned;
    pin_btn_->blockSignals(true);
    pin_btn_->setChecked(pinned_);
    pin_btn_->blockSignals(false);

    if (changed || (isVisible() && pinned_)) {
        PinWindowOptions pin_opts;
        pin_opts.keep_above = pinned_;
        syncNativeWindowPinState(this, pin_opts);
    }

    if (persist) {
        config_mgr_.mutableConfig().floating_pinned = pinned_;
        config_mgr_.save();
        status_label_->setText(pinned_ ? "已置顶保持在最前" : "已取消置顶");
    }
}

void FloatingWindow::applyResultOnlyState(bool result_only,
                                          bool persist,
                                          bool adjust_size) {
    const bool changing_mode = (result_only_ != result_only);
    if (changing_mode && !result_only_ && stack_->currentWidget() == card_page_) {
        captureSplitterSizes();
    }
    if (adjust_size && changing_mode && stack_->currentWidget() == card_page_) {
        captureLiveWidgetGeometry();
    }

    result_only_ = result_only;
    if (result_only_btn_) {
        result_only_btn_->blockSignals(true);
        result_only_btn_->setChecked(result_only_);
        result_only_btn_->blockSignals(false);
    }
    if (source_edit_) {
        source_edit_->setVisible(!result_only_);
    }
    if (char_count_label_) {
        char_count_label_->setVisible(!result_only_);
    }
    if (clear_btn_) {
        clear_btn_->setVisible(!result_only_);
    }

    if (stack_->currentWidget() == card_page_) {
        applyCurrentPageBounds(adjust_size && changing_mode);
    }

    if (persist) {
        config_mgr_.mutableConfig().floating_result_only = result_only_;
        config_mgr_.save();
        scheduleSaveUiState();
        status_label_->setText(
            result_only_ ? "已开启仅译文模式（配合剪贴板联动更紧凑 · Alt+M 还原）"
                         : "已切回原文 + 译文对照模式");
    }
}

void FloatingWindow::adjustFontScale(int new_scale, bool persist) {
    const int clamped = std::clamp(new_scale, 80, 160);
    if (clamped == font_scale_) {
        return;
    }
    font_scale_ = clamped;
    applyTheme();
    if (persist) {
        config_mgr_.mutableConfig().floating_font_scale = font_scale_;
        config_mgr_.save();
        status_label_->setText(
            QString("文本缩放: %1% (Ctrl+滚轮 / Ctrl+± / Ctrl+0 重置)")
                .arg(font_scale_));
    }
}

void FloatingWindow::saveConfigFromSettings(const ShadowConfig &cfg) {
    config_mgr_.mutableConfig() = cfg;
    config_mgr_.save();
    translator_->clearCache();
    applyConfigToUi(config_mgr_.config());
    notifyFcitxReload();
    if (!source_edit_->toPlainText().trimmed().isEmpty()) {
        dispatchTranslation(true, true);
    }
}

void FloatingWindow::notifyFcitxReload() {
    QDBusInterface iface("org.fcitx.Fcitx5", "/controller",
                         "org.fcitx.Fcitx.Controller1",
                         QDBusConnection::sessionBus());
    if (iface.isValid()) {
        iface.asyncCall("ReloadAddonConfig", QStringLiteral("shadowbar"));
    }
}

void FloatingWindow::updateFloatingToggleShortcuts(const std::string &hotkey_str) {
    for (QShortcut *sc : toggle_shortcuts_) {
        delete sc;
    }
    toggle_shortcuts_.clear();

    const QList<QKeySequence> seqs = fcitxToQtKeySequences(hotkey_str);
    const bool recording = isFocusInHotkeyEditor(QApplication::focusWidget());
    for (const QKeySequence &seq : seqs) {
        auto *sc = new QShortcut(seq, this);
        sc->setContext(Qt::WindowShortcut);
        sc->setEnabled(!recording);
        connect(sc, &QShortcut::activated, this, [this]() {
            if (!isVisible() ||
                isFocusInHotkeyEditor(QApplication::focusWidget())) {
                return;
            }
            if (last_shown_timer_.isValid() &&
                last_shown_timer_.elapsed() < 150) {
                return;
            }
            last_internal_toggle_timer_.restart();
            Toggle();
        });
        toggle_shortcuts_.push_back(sc);
    }

    if (close_btn_) {
        const QString primary = fcitxToQtKeySequenceString(hotkey_str);
        close_btn_->setToolTip(
            primary.isEmpty()
                ? QStringLiteral("收起悬浮窗 (Esc)")
                : QStringLiteral("收起悬浮窗 (%1 / Esc)").arg(primary));
    }
}

QString FloatingWindow::uiStateFilePath() const {
    const auto dir = config_mgr_.path().parent_path();
    if (!dir.empty()) {
        return QString::fromStdString((dir / "floating-state.ini").string());
    }
    return QString::fromStdString(
        (ConfigManager::defaultConfigPath().parent_path() /
         "floating-state.ini")
            .string());
}

void FloatingWindow::loadUiState() {
    QSettings state(uiStateFilePath(), QSettings::IniFormat);

    if (state.contains("window/card_size")) {
        const QSize sz = state.value("window/card_size").toSize();
        if (sz.isValid()) {
            saved_card_size_ = clampWindowSizeToScreen(sz, kMinCardSize);
        }
    }
    if (state.contains("window/result_only_size")) {
        const QSize sz = state.value("window/result_only_size").toSize();
        if (sz.isValid()) {
            saved_result_only_size_ =
                clampWindowSizeToScreen(sz, kMinResultOnlySize);
        }
    }
    if (state.contains("window/settings_size")) {
        const QSize sz = state.value("window/settings_size").toSize();
        if (sz.isValid()) {
            saved_settings_size_ =
                clampWindowSizeToScreen(sz, kMinSettingsSize);
        }
    }

    const QVariantList raw_split = state.value("window/splitter_sizes").toList();
    if (raw_split.size() == 2) {
        const int top_h = raw_split[0].toInt();
        const int bot_h = raw_split[1].toInt();
        if (top_h > 0 && bot_h > 0) {
            saved_splitter_sizes_ = {top_h, bot_h};
        }
    }

    has_saved_pos_ = state.value("window/has_pos", false).toBool();
    if (has_saved_pos_) {
        const QVariant v = state.value("window/pos");
        if (v.canConvert<QPoint>()) {
            saved_pos_ = v.toPoint();
        } else {
            has_saved_pos_ = false;
        }
    }

    has_saved_orb_pos_ = state.value("orb/has_pos", false).toBool();
    if (has_saved_orb_pos_) {
        const QVariant v = state.value("orb/pos");
        if (v.canConvert<QPoint>()) {
            saved_orb_pos_ = v.toPoint();
        } else {
            has_saved_orb_pos_ = false;
        }
    }
}

void FloatingWindow::saveUiStateNow() const {
    const QString path = uiStateFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSettings state(path, QSettings::IniFormat);
    state.setValue("window/card_size", saved_card_size_);
    state.setValue("window/result_only_size", saved_result_only_size_);
    state.setValue("window/settings_size", saved_settings_size_);

    QVariantList split_list;
    for (int h : saved_splitter_sizes_) {
        split_list.append(h);
    }
    state.setValue("window/splitter_sizes", split_list);

    state.setValue("window/has_pos", has_saved_pos_);
    if (has_saved_pos_) {
        state.setValue("window/pos", saved_pos_);
    }
    state.setValue("orb/has_pos", has_saved_orb_pos_);
    if (has_saved_orb_pos_) {
        state.setValue("orb/pos", saved_orb_pos_);
    }
    state.sync();
}

void FloatingWindow::scheduleSaveUiState() {
    if (save_state_timer_) {
        save_state_timer_->start(250);
    }
}

void FloatingWindow::captureLiveWidgetGeometry() {
    if (stack_) {
        if (stack_->currentWidget() == settings_view_) {
            saved_settings_size_ = size();
        } else if (stack_->currentWidget() == card_page_) {
            if (result_only_) {
                saved_result_only_size_ = size();
            } else {
                saved_card_size_ = size();
            }
        }
    }
    if (!isWaylandPlatform()) {
        if (isVisible()) {
            saved_pos_ = pos();
            has_saved_pos_ = true;
        }
        if (orb_ && orb_->isVisible()) {
            saved_orb_pos_ = orb_->pos();
            has_saved_orb_pos_ = true;
        }
    }
}

void FloatingWindow::captureSplitterSizes() {
    if (!splitter_ || result_only_ || !stack_ ||
        stack_->currentWidget() != card_page_) {
        return;
    }
    const QList<int> sizes = splitter_->sizes();
    if (sizes.size() == 2 && sizes[0] > 0 && sizes[1] > 0) {
        saved_splitter_sizes_ = sizes;
    }
}

void FloatingWindow::applyCurrentPageBounds(bool force_resize) {
    if (!stack_) {
        return;
    }
    restoring_geometry_ = true;
    if (stack_->currentWidget() == settings_view_) {
        setMinimumSize(kMinSettingsSize);
        saved_settings_size_ =
            clampWindowSizeToScreen(saved_settings_size_, kMinSettingsSize);
        if (force_resize) {
            resize(saved_settings_size_);
        }
    } else {
        const QSize &min_sz =
            result_only_ ? kMinResultOnlySize : kMinCardSize;
        setMinimumSize(min_sz);
        if (result_only_) {
            saved_result_only_size_ =
                clampWindowSizeToScreen(saved_result_only_size_, min_sz);
            if (force_resize) {
                resize(saved_result_only_size_);
            }
        } else {
            saved_card_size_ =
                clampWindowSizeToScreen(saved_card_size_, min_sz);
            if (force_resize) {
                resize(saved_card_size_);
            }
            applySavedSplitterSizes();
        }
    }
    restoring_geometry_ = false;
}

void FloatingWindow::applySavedSplitterSizes() {
    if (!splitter_ || result_only_ || saved_splitter_sizes_.size() != 2) {
        return;
    }
    if (saved_splitter_sizes_[0] <= 0 || saved_splitter_sizes_[1] <= 0) {
        return;
    }
    const QList<int> target = saved_splitter_sizes_;
    splitter_->setSizes(target);
    QTimer::singleShot(0, this, [this, target]() {
        if (splitter_ && !result_only_ && stack_ &&
            stack_->currentWidget() == card_page_) {
            splitter_->setSizes(target);
        }
    });
}

void FloatingWindow::resetWindowGeometry(bool show_card_after) {
    hiding_with_snapshot_ = false;
    ++wayland_snapshot_gen_;
    if (wayland_snapshot_timer_) {
        wayland_snapshot_timer_->stop();
    }

    const bool was_visible = isVisible();
    if (!was_visible && show_card_after) {
        config_mgr_.reloadIfModified();
        applyConfigToUi(config_mgr_.config());
    }

    saved_card_size_ = clampWindowSizeToScreen(kDefaultCardSize, kMinCardSize);
    saved_result_only_size_ =
        clampWindowSizeToScreen(kDefaultResultOnlySize, kMinResultOnlySize);
    saved_settings_size_ =
        clampWindowSizeToScreen(kDefaultSettingsSize, kMinSettingsSize);
    saved_splitter_sizes_ = {kDefaultSplitterTop, kDefaultSplitterBottom};

    if (stack_ && stack_->currentWidget() != card_page_) {
        stack_->setCurrentWidget(card_page_);
    }
    applyCurrentPageBounds(true);

    const QSize target_sz =
        result_only_ ? saved_result_only_size_ : saved_card_size_;
    const QPoint centered = centeredWindowPosOnActiveScreen(this, target_sz);
    saved_pos_ = centered;
    has_saved_pos_ = true;

    if (has_saved_orb_pos_) {
        const QSize orb_sz = orb_ ? orb_->size() : QSize(50, 50);
        saved_orb_pos_ = clampWindowPosToScreens(saved_orb_pos_, orb_sz);
        if (orb_ && orb_->isVisible()) {
            orb_->setRememberedPosition(true, saved_orb_pos_);
        }
    }
    saveUiStateNow();

    if (!isWaylandPlatform()) {
        restoring_geometry_ = true;
        move(centered);
        restoring_geometry_ = false;
    }

    if (!was_visible) {
        if (show_card_after) {
            last_shown_timer_.restart();
            show();
            raise();
            activateWindow();
            if (result_only_) {
                result_edit_->setFocus(Qt::ShortcutFocusReason);
            } else {
                source_edit_->setFocus(Qt::ShortcutFocusReason);
            }
            if (config_mgr_.config().floating_auto_read_clipboard) {
                readClipboardAndTranslate(false);
            }
        }
    } else {
        if (isWaylandPlatform()) {
            PinWindowOptions opts;
            opts.keep_above = pinned_;
            opts.apply_position = true;
            opts.target_pos = centered;
            opts.apply_size = true;
            opts.target_size = target_sz;
            scheduleNativeWindowPinSync(this, opts);
        }
        raise();
        activateWindow();
    }

    if (status_label_) {
        status_label_->setText("✓ 已重置窗口位置与默认大小");
    }
}

QList<QWidget *> FloatingWindow::visibleSnapshotWidgets() const {
    QList<QWidget *> out;
    if (isVisible()) {
        out.append(const_cast<FloatingWindow *>(this));
    }
    if (orb_ && orb_->isVisible()) {
        out.append(orb_.get());
    }
    return out;
}

void FloatingWindow::scheduleWaylandGeometrySnapshot(int delay_ms) {
    if (!wayland_snapshot_timer_ || !isWaylandPlatform() ||
        hiding_with_snapshot_ || quitting_with_snapshot_) {
        return;
    }
    if (!visibleSnapshotWidgets().isEmpty()) {
        wayland_snapshot_timer_->start(delay_ms);
    }
}

void FloatingWindow::finishPendingHide(uint64_t snapshot_gen) {
    if (!hiding_with_snapshot_ || waiting_card_geom_gen_ != snapshot_gen) {
        return;
    }
    hiding_with_snapshot_ = false;
    cancelPopupTranslation();
    captureLiveWidgetGeometry();
    captureSplitterSizes();
    saveUiStateNow();
    hide();
    syncClipboardMonitorState();
}

void FloatingWindow::finishPendingQuit(uint64_t snapshot_gen) {
    if (!quitting_with_snapshot_ || waiting_quit_gen_ != snapshot_gen) {
        return;
    }
    quitting_with_snapshot_ = false;
    pending_quit_captions_.clear();
    cancelPopupTranslation();
    if (clip_monitor_) {
        clip_monitor_->setWatchEnabled(false);
    }
    saveUiStateNow();
    if (orb_) {
        orb_->hide();
    }
    hide();
    qApp->quit();
}

Qt::Edges FloatingWindow::detectResizeEdges(const QPoint &pos) const {
    constexpr int kResizeMargin = 9;
    Qt::Edges edges;
    if (pos.x() >= 0 && pos.x() <= kResizeMargin) {
        edges |= Qt::LeftEdge;
    } else if (pos.x() >= width() - kResizeMargin && pos.x() <= width()) {
        edges |= Qt::RightEdge;
    }
    if (pos.y() >= 0 && pos.y() <= kResizeMargin) {
        edges |= Qt::TopEdge;
    } else if (pos.y() >= height() - kResizeMargin && pos.y() <= height()) {
        edges |= Qt::BottomEdge;
    }
    return edges;
}

void FloatingWindow::updateHoverResizeCursor(const QPoint &global_pos) {
    if (manual_resize_edges_ != Qt::Edges{}) {
        return;
    }
    const Qt::Edges edges = detectResizeEdges(mapFromGlobal(global_pos));
    if (edges == Qt::Edges{}) {
        unsetCursor();
    } else {
        setCursor(cursorForEdges(edges));
    }
}

bool FloatingWindow::tryStartResizeAtGlobalPos(const QPoint &global_pos) {
    const Qt::Edges edges = detectResizeEdges(mapFromGlobal(global_pos));
    if (edges == Qt::Edges{}) {
        return false;
    }
    if (QWindow *win = windowHandle(); win && win->startSystemResize(edges)) {
        return true;
    }
    manual_resize_edges_ = edges;
    resize_start_global_ = global_pos;
    resize_start_geo_ = geometry();
    setCursor(cursorForEdges(edges));
    return true;
}

bool FloatingWindow::event(QEvent *ev) {
    switch (ev->type()) {
    case QEvent::HoverMove: {
        auto *he = static_cast<QHoverEvent *>(ev);
        updateHoverResizeCursor(mapToGlobal(he->position().toPoint()));
        break;
    }
    case QEvent::HoverLeave:
    case QEvent::Leave:
        if (manual_resize_edges_ == Qt::Edges{}) {
            unsetCursor();
        }
        break;
    case QEvent::MouseButtonPress: {
        auto *me = static_cast<QMouseEvent *>(ev);
        if (me->button() == Qt::LeftButton &&
            tryStartResizeAtGlobalPos(me->globalPosition().toPoint())) {
            ev->accept();
            return true;
        }
        break;
    }
    case QEvent::WindowDeactivate:
        if (isWaylandPlatform() && isVisible()) {
            scheduleWaylandGeometrySnapshot(100);
        }
        break;
    default:
        break;
    }
    return QWidget::event(ev);
}

bool FloatingWindow::eventFilter(QObject *watched, QEvent *event) {
    if (!watched || !event) {
        return QWidget::eventFilter(watched, event);
    }
    const QEvent::Type ev_type = event->type();

    if (ev_type == QEvent::MouseButtonPress) {
        if (watched == card_root_ || watched == card_page_ ||
            watched == header_bar_ ||
            (header_bar_ && watched->parent() == header_bar_)) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton &&
                tryStartResizeAtGlobalPos(me->globalPosition().toPoint())) {
                return true;
            }
        }
        if (header_bar_ &&
            (watched == header_bar_ || watched->parent() == header_bar_)) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton &&
                (watched == header_bar_ ||
                 qobject_cast<QFrame *>(watched) != nullptr)) {
                if (QWindow *win = windowHandle()) {
                    win->startSystemMove();
                }
                return true;
            }
        }
    } else if (ev_type == QEvent::KeyPress) {
        if ((source_edit_ && watched == source_edit_) ||
            (result_edit_ && watched == result_edit_)) {
            auto *ke = static_cast<QKeyEvent *>(event);
            const Qt::KeyboardModifiers mods = ke->modifiers();
            if (watched == source_edit_ && (mods & Qt::ControlModifier) &&
                (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter)) {
                copyTranslation(true);
                return true;
            }
            if (mods & Qt::ControlModifier) {
                if (ke->key() == Qt::Key_Plus || ke->key() == Qt::Key_Equal) {
                    adjustFontScale(font_scale_ + 10, true);
                    return true;
                }
                if (ke->key() == Qt::Key_Minus ||
                    ke->key() == Qt::Key_Underscore) {
                    adjustFontScale(font_scale_ - 10, true);
                    return true;
                }
                if (ke->key() == Qt::Key_0) {
                    adjustFontScale(100, true);
                    return true;
                }
            }
            if (result_edit_ && watched == result_edit_ &&
                (ke->key() == Qt::Key_PageDown || ke->key() == Qt::Key_Down)) {
                auto *sb = result_edit_->verticalScrollBar();
                if (sb &&
                    (sb->maximum() == 0 || sb->value() >= sb->maximum() - 12)) {
                    maybeLoadNextSegmentOnScroll();
                }
            }
        }
    } else if (ev_type == QEvent::Wheel) {
        const bool on_res_vp =
            (result_edit_ && watched == result_edit_->viewport());
        const bool on_src_vp =
            (source_edit_ && watched == source_edit_->viewport());
        if (on_res_vp || on_src_vp) {
            auto *we = static_cast<QWheelEvent *>(event);
            if (we->modifiers() & Qt::ControlModifier) {
                const int delta = we->angleDelta().y();
                if (delta != 0) {
                    adjustFontScale(font_scale_ + (delta > 0 ? 10 : -10), true);
                }
                return true;
            }
            if (on_res_vp &&
                (we->angleDelta().y() < 0 || we->pixelDelta().y() < 0)) {
                auto *sb = result_edit_->verticalScrollBar();
                if (sb &&
                    (sb->maximum() == 0 || sb->value() >= sb->maximum() - 36)) {
                    if (!wheel_load_cooldown_.isValid() ||
                        wheel_load_cooldown_.elapsed() >= 180) {
                        wheel_load_cooldown_.restart();
                        maybeLoadNextSegmentOnScroll();
                    }
                }
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void FloatingWindow::changeEvent(QEvent *event) {
    if (event->type() == QEvent::ApplicationPaletteChange ||
        event->type() == QEvent::PaletteChange ||
        event->type() == QEvent::ThemeChange) {
        applyTheme();
    } else if (event->type() == QEvent::WindowStateChange) {
        syncClipboardMonitorState();
    }
    QWidget::changeEvent(event);
}

void FloatingWindow::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    syncClipboardMonitorState();
    if (!event->spontaneous()) {
        PinWindowOptions opts;
        opts.keep_above = pinned_;
        if (isWaylandPlatform() && has_saved_pos_) {
            opts.apply_position = true;
            opts.target_pos = clampWindowPosToScreens(saved_pos_, size());
        }
        if (opts.keep_above || opts.apply_position) {
            scheduleNativeWindowPinSync(this, opts);
        }
    }
}

void FloatingWindow::hideEvent(QHideEvent *event) {
    QWidget::hideEvent(event);
    cancelPopupTranslation();
    syncClipboardMonitorState();
}

void FloatingWindow::closeEvent(QCloseEvent *event) {
    cancelPopupTranslation();
    if (clip_monitor_) {
        clip_monitor_->setWatchEnabled(false);
    }
    captureLiveWidgetGeometry();
    captureSplitterSizes();
    saveUiStateNow();
    QWidget::closeEvent(event);
}

void FloatingWindow::moveEvent(QMoveEvent *event) {
    QWidget::moveEvent(event);
    if (!restoring_geometry_ && !isWaylandPlatform() && isVisible()) {
        saved_pos_ = pos();
        has_saved_pos_ = true;
        scheduleSaveUiState();
    }
}

void FloatingWindow::keyPressEvent(QKeyEvent *event) {
    if (event->modifiers() & Qt::ControlModifier) {
        if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal) {
            adjustFontScale(font_scale_ + 10, true);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Minus || event->key() == Qt::Key_Underscore) {
            adjustFontScale(font_scale_ - 10, true);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_0) {
            adjustFontScale(100, true);
            event->accept();
            return;
        }
    }
    if (event->key() == Qt::Key_Escape) {
        if (stack_->currentWidget() == settings_view_) {
            returnToCardPage();
            event->accept();
            return;
        }
        if (!pinned_) {
            hideCard();
            event->accept();
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

void FloatingWindow::mouseMoveEvent(QMouseEvent *event) {
    if (manual_resize_edges_ != Qt::Edges{}) {
        const QPoint delta =
            event->globalPosition().toPoint() - resize_start_global_;
        QRect r = resize_start_geo_;
        const int min_w = minimumWidth();
        const int min_h = minimumHeight();
        if (manual_resize_edges_ & Qt::LeftEdge) {
            r.setLeft(std::min(r.right() - min_w + 1, r.left() + delta.x()));
        }
        if (manual_resize_edges_ & Qt::RightEdge) {
            r.setRight(std::max(r.left() + min_w - 1, r.right() + delta.x()));
        }
        if (manual_resize_edges_ & Qt::TopEdge) {
            r.setTop(std::min(r.bottom() - min_h + 1, r.top() + delta.y()));
        }
        if (manual_resize_edges_ & Qt::BottomEdge) {
            r.setBottom(std::max(r.top() + min_h - 1, r.bottom() + delta.y()));
        }
        setGeometry(r);
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void FloatingWindow::mouseReleaseEvent(QMouseEvent *event) {
    if (manual_resize_edges_ != Qt::Edges{} && event->button() == Qt::LeftButton) {
        manual_resize_edges_ = {};
        unsetCursor();
        captureLiveWidgetGeometry();
        captureSplitterSizes();
        scheduleSaveUiState();
        if (isWaylandPlatform()) {
            scheduleWaylandGeometrySnapshot(80);
        }
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void FloatingWindow::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if (restoring_geometry_ || !stack_) {
        return;
    }
    if (stack_->currentWidget() == settings_view_) {
        saved_settings_size_ = event->size();
    } else if (stack_->currentWidget() == card_page_) {
        if (result_only_) {
            saved_result_only_size_ = event->size();
        } else {
            saved_card_size_ = event->size();
        }
    }
    if (isVisible()) {
        scheduleSaveUiState();
    }
}

} // namespace shadowbar::floating_ui
