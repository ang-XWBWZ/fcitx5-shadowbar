#include "settings_view.hpp"
#include "hotkey_util.hpp"
#include "shadowbar/config.hpp"
#include "theme_util.hpp"
#include "ty_model_helpers.hpp"
#include "ty_model_manager.hpp"
#include "ui_util.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>

namespace shadowbar::floating_ui {

namespace {

QWidget *createHotkeyRow(QKeySequenceEdit *&out_edit, QWidget *parent) {
    auto *wrap = new QWidget(parent);
    auto *row = new QHBoxLayout(wrap);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);

    out_edit = new QKeySequenceEdit(wrap);
    auto *clear_btn = new QToolButton(wrap);
    clear_btn->setText("✕");
    clear_btn->setToolTip("清除此快捷键");
    clear_btn->setAutoRaise(true);
    QObject::connect(clear_btn, &QToolButton::clicked, out_edit,
                     &QKeySequenceEdit::clear);

    row->addWidget(out_edit, 1);
    row->addWidget(clear_btn);
    return wrap;
}

QString normalizeDictPathKey(const QString &path) {
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    const QFileInfo fi(trimmed);
    const QString canon = fi.canonicalFilePath();
    return canon.isEmpty() ? QDir::cleanPath(fi.absoluteFilePath()) : canon;
}

QCheckBox *cellCheckBox(const QTableWidget *tbl, int row, int col) {
    if (!tbl) {
        return nullptr;
    }
    QWidget *w = tbl->cellWidget(row, col);
    return w ? w->findChild<QCheckBox *>() : nullptr;
}

QComboBox *cellComboBox(const QTableWidget *tbl, int row, int col) {
    if (!tbl) {
        return nullptr;
    }
    return qobject_cast<QComboBox *>(tbl->cellWidget(row, col));
}

std::vector<ExternalDictSpec> readDictRowsFromUi(
    const QTableWidget *tbl, const std::vector<ExternalDictSpec> &base) {
    if (!tbl || tbl->rowCount() == 0) {
        return base;
    }
    std::vector<ExternalDictSpec> out = base;
    out.resize(static_cast<size_t>(tbl->rowCount()));
    for (int r = 0; r < tbl->rowCount(); ++r) {
        ExternalDictSpec &spec = out[static_cast<size_t>(r)];
        if (QCheckBox *chk = cellCheckBox(tbl, r, 0)) {
            spec.enabled = chk->isChecked();
        }
        if (QComboBox *src_cb = cellComboBox(tbl, r, 2)) {
            const std::string code = src_cb->currentData().toString().toStdString();
            spec.source_lang = code.empty() ? std::string("auto") : code;
        }
        if (QComboBox *tgt_cb = cellComboBox(tbl, r, 3)) {
            const std::string code = tgt_cb->currentData().toString().toStdString();
            spec.target_lang = code.empty() ? std::string("auto") : code;
        }
    }
    return out;
}

QString formatDictReportBadge(const DictSourceReport &rep) {
    QString head = rep.format.empty()
                       ? QStringLiteral("词典")
                       : QString::fromStdString(rep.format).toUpper();
    if (!rep.loaded) {
        const QString err = QString::fromStdString(rep.message);
        return err.isEmpty() ? QString("⚠ %1 · 加载失败").arg(head)
                             : QString("⚠ %1 · %2").arg(head, err);
    }
    const QString dir = QString("%1→%2").arg(
        QString::fromStdString(rep.source_lang.empty() ? "auto" : rep.source_lang),
        QString::fromStdString(rep.target_lang.empty() ? "auto" : rep.target_lang));
    if (rep.format == "sqlite") {
        return QString("✓ %1 (%2) · SQLite 按需检索").arg(head, dir);
    }
    return QString("✓ %1 (%2) · %3 条").arg(head, dir).arg(rep.entry_count);
}

} // namespace

SettingsView::SettingsView(QWidget *parent)
    : QWidget(parent),
      test_engine_(std::make_unique<AsyncTranslationEngine>(curlHttpTransport, 32)),
      zh_fixer_(new ZhEnvFixer(this)) {
    buildUi();
    applyTheme();
    connect(zh_fixer_, &ZhEnvFixer::diagnoseFinished, this,
            &SettingsView::applyZhEnvReport);
    connect(zh_fixer_, &ZhEnvFixer::logLine, this, [this](const QString &line) {
        if (zh_fix_log_edit_) {
            zh_fix_log_edit_->appendPlainText(line);
        }
    });
    connect(zh_fixer_, &ZhEnvFixer::fixFinished, this,
            [this](bool, const QString &summary) {
                if (zh_diagnose_btn_) {
                    zh_diagnose_btn_->setEnabled(true);
                }
                if (zh_fix_btn_) {
                    zh_fix_btn_->setEnabled(true);
                }
                if (footer_status_) {
                    footer_status_->setText(summary);
                }
            });
}

SettingsView::~SettingsView() {
    if (test_engine_) {
        test_engine_->shutdown();
    }
}

void SettingsView::buildUi() {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 10, 14, 12);
    root->setSpacing(10);

    // Top bar
    auto *header = new QHBoxLayout();
    header->setSpacing(8);
    auto *back_btn = new QPushButton("← 返回悬浮窗", this);
    back_btn->setObjectName("SecondaryButton");
    connect(back_btn, &QPushButton::clicked, this, &SettingsView::backRequested);

    auto *title = new QLabel("ShadowBar 设置与 API 管理", this);
    QFont tf = title->font();
    tf.setBold(true);
    tf.setPointSize(11);
    title->setFont(tf);

    header->addWidget(back_btn);
    header->addWidget(title);
    header->addStretch(1);
    root->addLayout(header);

    tabs_ = new QTabWidget(this);
    tabs_->addTab(buildPresetsTab(), "API 与译文预设");
    tabs_->addTab(buildGeneralTab(), "输入法、快捷键与悬浮窗");
    tabs_->addTab(buildTyModelTab(), "TY 模型 (1.25bit/2bit)");
    tabs_->addTab(buildZhEnvTab(), "中文环境修复");
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int idx) {
        if (idx == 3 && zh_fixer_ && !zh_fixer_->isBusy()) {
            zh_fixer_->runDiagnose();
        }
    });
    root->addWidget(tabs_, 1);

    // Footer buttons
    auto *footer = new QHBoxLayout();
    footer->setSpacing(8);
    footer_status_ = new QLabel(
        QString("配置文件: %1")
            .arg(QString::fromStdString(ConfigManager::defaultConfigPath().string())),
        this);
    footer_status_->setObjectName("SubtleStatus");

    auto *reset_btn = new QPushButton("恢复默认", this);
    reset_btn->setObjectName("SecondaryButton");
    connect(reset_btn, &QPushButton::clicked, this,
            &SettingsView::onResetDefaults);

    auto *save_btn = new QPushButton("✓ 保存并生效", this);
    save_btn->setObjectName("PrimaryButton");
    connect(save_btn, &QPushButton::clicked, this, &SettingsView::onSaveClicked);

    footer->addWidget(footer_status_, 1);
    footer->addWidget(reset_btn);
    footer->addWidget(save_btn);
    root->addLayout(footer);
}

QWidget *SettingsView::buildPresetsTab() {
    auto *page = new QWidget(this);
    auto *main_row = new QHBoxLayout(page);
    main_row->setContentsMargins(8, 10, 8, 8);
    main_row->setSpacing(12);

    // Left: preset list + CRUD buttons
    auto *left_col = new QVBoxLayout();
    left_col->setSpacing(6);
    preset_list_ = new QListWidget(page);
    preset_list_->setFixedWidth(156);
    connect(preset_list_, &QListWidget::currentRowChanged, this,
            &SettingsView::onPresetRowChanged);
    left_col->addWidget(preset_list_, 1);

    auto *crud_row = new QHBoxLayout();
    crud_row->setSpacing(4);
    auto *add_btn = new QPushButton("+ 新增", page);
    add_btn->setObjectName("SecondaryButton");
    connect(add_btn, &QPushButton::clicked, this, &SettingsView::onAddPreset);

    del_preset_btn_ = new QPushButton("删除", page);
    del_preset_btn_->setObjectName("SecondaryButton");
    connect(del_preset_btn_, &QPushButton::clicked, this,
            &SettingsView::onDeletePreset);

    default_preset_btn_ = new QPushButton("★ 默认", page);
    default_preset_btn_->setObjectName("SecondaryButton");
    connect(default_preset_btn_, &QPushButton::clicked, this,
            &SettingsView::onSetDefaultPreset);

    crud_row->addWidget(add_btn);
    crud_row->addWidget(del_preset_btn_);
    crud_row->addWidget(default_preset_btn_);
    left_col->addLayout(crud_row);
    main_row->addLayout(left_col);

    // Right: form inside scroll area
    auto *scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *form_host = new QWidget(scroll);
    auto *right_col = new QVBoxLayout(form_host);
    right_col->setContentsMargins(0, 0, 4, 0);
    right_col->setSpacing(8);

    // Quick API template chips
    auto *tpl_row = new QHBoxLayout();
    tpl_row->setSpacing(4);
    auto *tpl_lbl = new QLabel("快速模板:", form_host);
    tpl_lbl->setObjectName("SubtleStatus");
    tpl_row->addWidget(tpl_lbl);

    auto add_tpl_btn = [&](const QString &name, ProviderKind kind,
                           const QString &ep, const QString &mdl,
                           const QString &key) {
        auto *b = new QPushButton(name, form_host);
        b->setObjectName("ChipButton");
        connect(b, &QPushButton::clicked, this, [this, kind, ep, mdl, key]() {
            applyQuickTemplate(kind, ep, mdl, key);
        });
        tpl_row->addWidget(b);
    };
    add_tpl_btn("Google", ProviderKind::GoogleFree, "", "", "");
    add_tpl_btn("OpenAI", ProviderKind::OpenAICompatible,
                "https://api.openai.com/v1/chat/completions", "gpt-4o-mini",
                "env:OPENAI_API_KEY");
    add_tpl_btn("DeepSeek", ProviderKind::OpenAICompatible,
                "https://api.deepseek.com/chat/completions", "deepseek-chat",
                "env:DEEPSEEK_API_KEY");
    add_tpl_btn("DeepL", ProviderKind::DeepL,
                "https://api-free.deepl.com/v2/translate", "",
                "env:DEEPL_API_KEY");
    add_tpl_btn("Ollama", ProviderKind::Ollama,
                "http://127.0.0.1:11434/api/chat", "qwen2.5:7b", "");
    add_tpl_btn("离线词典", ProviderKind::Offline, "", "", "");
    tpl_row->addStretch(1);
    right_col->addLayout(tpl_row);

    auto *ty_tpl_row = new QHBoxLayout();
    ty_tpl_row->setSpacing(4);
    auto *ty_tpl_lbl = new QLabel("本地 TY:", form_host);
    ty_tpl_lbl->setObjectName("SubtleStatus");
    ty_tpl_row->addWidget(ty_tpl_lbl);

    auto add_ty_tpl_btn = [&](const QString &name, const QString &variant) {
        auto *b = new QPushButton(name, form_host);
        b->setObjectName("ChipButton");
        connect(b, &QPushButton::clicked, this, [this, variant]() {
            const TyModelSpec spec = tyModelSpec(variant);
            const int port = ty_port_spin_ ? ty_port_spin_->value()
                                            : working_config_.hymt_port;
            applyQuickTemplate(ProviderKind::OpenAICompatible,
                               tyChatCompletionsEndpoint(port),
                               spec.model_name, "");
        });
        ty_tpl_row->addWidget(b);
    };
    add_ty_tpl_btn("TY 1.25bit", QStringLiteral("1.25bit"));
    add_ty_tpl_btn("TY 2bit", QStringLiteral("2bit"));

    auto *goto_ty_btn = new QPushButton("管理本地模型与服务 →", form_host);
    goto_ty_btn->setObjectName("ChipButton");
    connect(goto_ty_btn, &QPushButton::clicked, this, [this]() {
        setCurrentTab(2);
    });
    ty_tpl_row->addWidget(goto_ty_btn);
    ty_tpl_row->addStretch(1);
    right_col->addLayout(ty_tpl_row);

    auto *form = new QFormLayout();
    form->setSpacing(8);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    preset_id_edit_ = new QLineEdit(form_host);
    preset_id_edit_->setPlaceholderText("例如 quick / natural / deepseek");

    preset_label_edit_ = new QLineEdit(form_host);
    preset_label_edit_->setPlaceholderText("候选卡片中显示的短名称，例如 自然");
    connect(preset_label_edit_, &QLineEdit::textEdited, this, [this]() {
        if (!updating_preset_ui_) {
            storeCurrentPresetFields();
            if (auto *item = preset_list_->currentItem()) {
                if (editing_preset_row_ >= 0 &&
                    editing_preset_row_ <
                        static_cast<int>(working_config_.presets.size())) {
                    const auto &p = working_config_.presets[editing_preset_row_];
                    const bool is_def = (p.id == working_config_.active_preset);
                    item->setText(QString("%1%2 · %3")
                                      .arg(is_def ? "★ " : "",
                                           QString::fromStdString(p.label),
                                           QString::fromStdString(p.id)));
                }
            }
        }
    });

    preset_provider_combo_ = new QComboBox(form_host);
    preset_provider_combo_->addItem("Google Free (免密钥)", "google");
    preset_provider_combo_->addItem("OpenAI 兼容 API", "openai");
    preset_provider_combo_->addItem("DeepL API", "deepl");
    preset_provider_combo_->addItem("Ollama 本地 API", "ollama");
    preset_provider_combo_->addItem("离线词典 (多语言 / 扩展词库)", "offline");
    connect(preset_provider_combo_, &QComboBox::currentIndexChanged, this,
            [this](int) {
                if (!updating_preset_ui_) {
                    updateProviderFieldStates();
                }
            });

    preset_lang_combo_ = new QComboBox(form_host);
    populateTargetLangCombo(preset_lang_combo_, false, true);

    preset_endpoint_edit_ = new QLineEdit(form_host);
    preset_endpoint_edit_->setPlaceholderText(
        "https://api.openai.com/v1（支持自动补全 /chat/completions 或 Ollama /api/chat）");

    preset_model_edit_ = new QLineEdit(form_host);
    preset_model_edit_->setPlaceholderText("gpt-4o-mini / deepseek-chat");

    auto *key_wrap = new QWidget(form_host);
    auto *key_row = new QHBoxLayout(key_wrap);
    key_row->setContentsMargins(0, 0, 0, 0);
    key_row->setSpacing(4);
    preset_apikey_edit_ = new QLineEdit(key_wrap);
    preset_apikey_edit_->setEchoMode(QLineEdit::Password);
    preset_apikey_edit_->setPlaceholderText("支持填写 env:OPENAI_API_KEY");
    apikey_reveal_btn_ = new QToolButton(key_wrap);
    apikey_reveal_btn_->setText("👁");
    apikey_reveal_btn_->setCheckable(true);
    apikey_reveal_btn_->setToolTip("显示/隐藏 API Key");
    connect(apikey_reveal_btn_, &QToolButton::toggled, this,
            [this](bool checked) {
                preset_apikey_edit_->setEchoMode(
                    checked ? QLineEdit::Normal : QLineEdit::Password);
            });
    key_row->addWidget(preset_apikey_edit_, 1);
    key_row->addWidget(apikey_reveal_btn_);

    preset_prompt_edit_ = new QPlainTextEdit(form_host);
    preset_prompt_edit_->setFixedHeight(68);
    preset_prompt_edit_->setPlaceholderText("自定义翻译提示词（System Prompt）");

    form->addRow("预设 ID:", preset_id_edit_);
    form->addRow("显示标签:", preset_label_edit_);
    form->addRow("引擎类型:", preset_provider_combo_);
    form->addRow("默认目标:", preset_lang_combo_);
    form->addRow("API 地址:", preset_endpoint_edit_);
    form->addRow("模型:", preset_model_edit_);
    form->addRow("API Key:", key_wrap);
    form->addRow("Prompt:", preset_prompt_edit_);
    right_col->addLayout(form);

    // Live connectivity test row
    auto *test_row = new QHBoxLayout();
    test_row->setSpacing(8);
    test_preset_btn_ = new QPushButton("🧪 测试连通性", form_host);
    test_preset_btn_->setObjectName("SecondaryButton");
    connect(test_preset_btn_, &QPushButton::clicked, this,
            &SettingsView::onTestPreset);
    test_result_label_ = new QLabel("测试样本: “你好，世界”", form_host);
    test_result_label_->setObjectName("SubtleStatus");
    test_result_label_->setWordWrap(true);
    test_row->addWidget(test_preset_btn_);
    test_row->addWidget(test_result_label_, 1);
    right_col->addLayout(test_row);
    right_col->addStretch(1);

    scroll->setWidget(form_host);
    main_row->addWidget(scroll, 1);
    return page;
}

QWidget *SettingsView::buildGeneralTab() {
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *host = new QWidget(scroll);
    auto *col = new QVBoxLayout(host);
    col->setContentsMargins(8, 10, 8, 8);
    col->setSpacing(10);

    // 1. Shortcuts
    auto *hk_group = new QGroupBox("快捷键绑定", host);
    auto *hk_form = new QFormLayout(hk_group);
    hk_form->setSpacing(6);
    hk_form->addRow("呼出 / 隐藏悬浮窗:",
                    createHotkeyRow(floating_hotkey_edit_, hk_group));
    hk_form->addRow("伴随翻译常驻开关:",
                    createHotkeyRow(toggle_hotkey_edit_, hk_group));
    hk_form->addRow("全局切换目标语言:",
                    createHotkeyRow(cycle_lang_hotkey_edit_, hk_group));
    hk_form->addRow("全局切换译文预设:",
                    createHotkeyRow(cycle_preset_hotkey_edit_, hk_group));
    hk_form->addRow("单句快译 (可选):",
                    createHotkeyRow(oneshot_hotkey_edit_, hk_group));
    col->addWidget(hk_group);

    // 2. Floating window & orb
    auto *fl_group = new QGroupBox("翻译悬浮窗与桌面悬浮球", host);
    auto *fl_lay = new QVBoxLayout(fl_group);
    fl_lay->setSpacing(5);
    floating_enabled_check_ =
        new QCheckBox("启用独立翻译悬浮窗组件及全局快捷键", fl_group);
    floating_autoread_check_ =
        new QCheckBox("主动打开悬浮窗时默认读取剪贴板（后台绝不自动弹窗）", fl_group);
    floating_watch_check_ = new QCheckBox(
        "悬浮窗打开时后台自动监听剪贴板变更并翻译（自动避让文件与链接）",
        fl_group);
    floating_orb_check_ =
        new QCheckBox("在桌面显示可拖动的迷你悬浮球 (✦)", fl_group);
    floating_pinned_check_ =
        new QCheckBox("默认保持悬浮窗置顶钉住", fl_group);
    floating_result_only_check_ = new QCheckBox(
        "仅译文模式（隐藏原文框，配合剪贴板联动更省空间）", fl_group);
    fl_lay->addWidget(floating_enabled_check_);
    fl_lay->addWidget(floating_autoread_check_);
    fl_lay->addWidget(floating_watch_check_);
    fl_lay->addWidget(floating_orb_check_);
    fl_lay->addWidget(floating_pinned_check_);
    fl_lay->addWidget(floating_result_only_check_);

    auto *fl_opts_form = new QFormLayout();
    fl_opts_form->setContentsMargins(0, 4, 0, 0);
    fl_opts_form->setSpacing(6);
    floating_font_scale_spin_ = new QSpinBox(fl_group);
    floating_font_scale_spin_->setRange(80, 160);
    floating_font_scale_spin_->setSingleStep(10);
    floating_font_scale_spin_->setSuffix(" %");
    fl_opts_form->addRow("悬浮窗文本缩放 (Ctrl+± / 滚轮):",
                         floating_font_scale_spin_);

    floating_segment_chars_spin_ = new QSpinBox(fl_group);
    floating_segment_chars_spin_->setRange(150, 4000);
    floating_segment_chars_spin_->setSingleStep(50);
    floating_segment_chars_spin_->setSuffix(" 词/字");
    floating_segment_chars_spin_->setToolTip(
        "英文按单词数、中文按字符数计算分批上限，优先在句末/词边界切分，向下滚动时逐段翻译");
    fl_opts_form->addRow("长文本分批上限:", floating_segment_chars_spin_);
    fl_lay->addLayout(fl_opts_form);
    col->addWidget(fl_group);

    // 3. Prefix trigger & IME UI/behavior
    auto *ime_group = new QGroupBox("输入法触发、候选栏与翻译行为", host);
    auto *ime_form = new QFormLayout(ime_group);
    ime_form->setSpacing(6);

    auto *prefix_row_w = new QWidget(ime_group);
    auto *prefix_row = new QHBoxLayout(prefix_row_w);
    prefix_row->setContentsMargins(0, 0, 0, 0);
    prefix_row->setSpacing(8);
    enable_prefix_check_ = new QCheckBox("启用前缀快译", prefix_row_w);
    prefix_edit_ = new QLineEdit(prefix_row_w);
    prefix_edit_->setMaximumWidth(64);
    prefix_cjk_only_check_ = new QCheckBox("仅中文输入法下监听", prefix_row_w);
    prefix_row->addWidget(enable_prefix_check_);
    prefix_row->addWidget(prefix_edit_);
    prefix_row->addWidget(prefix_cjk_only_check_);
    prefix_row->addStretch(1);
    ime_form->addRow("短前缀触发:", prefix_row_w);

    prefix_timeout_spin_ = new QSpinBox(ime_group);
    prefix_timeout_spin_->setRange(80, 2000);
    prefix_timeout_spin_->setSuffix(" ms");
    ime_form->addRow("前缀等待窗口:", prefix_timeout_spin_);

    floating_source_lang_combo_ = new QComboBox(ime_group);
    populateSourceLangCombo(floating_source_lang_combo_, false);
    ime_form->addRow("悬浮窗默认源语言:", floating_source_lang_combo_);

    default_target_lang_combo_ = new QComboBox(ime_group);
    populateTargetLangCombo(default_target_lang_combo_, false, false);
    ime_form->addRow("全局默认目标语言:", default_target_lang_combo_);

    auto *cycle_wrap = new QWidget(ime_group);
    auto *cycle_vbox = new QVBoxLayout(cycle_wrap);
    cycle_vbox->setContentsMargins(0, 2, 0, 2);
    cycle_vbox->setSpacing(4);
    auto *cycle_grid = new QGridLayout();
    cycle_grid->setContentsMargins(0, 0, 0, 0);
    cycle_grid->setHorizontalSpacing(10);
    cycle_grid->setVerticalSpacing(2);
    cycle_lang_checks_.clear();
    const auto &langs = supportedLanguages();
    for (size_t i = 0; i < langs.size(); ++i) {
        const auto &lang = langs[i];
        const std::string code(lang.code);
        auto *cb = new QCheckBox(
            QString("%1 (%2)").arg(QString::fromUtf8(lang.zh_name),
                                   QString::fromUtf8(lang.code)),
            cycle_wrap);
        connect(cb, &QCheckBox::toggled, this, [this, code](bool on) {
            auto &list = working_config_.cycle_languages;
            auto it = std::find(list.begin(), list.end(), code);
            if (on) {
                if (it == list.end()) {
                    list.push_back(code);
                }
            } else if (it != list.end()) {
                if (list.size() <= 1) {
                    for (const auto &pair : cycle_lang_checks_) {
                        if (pair.first == code && pair.second) {
                            pair.second->blockSignals(true);
                            pair.second->setChecked(true);
                            pair.second->blockSignals(false);
                        }
                    }
                    return;
                }
                list.erase(it);
            }
            updateCycleLangSummary();
        });
        cycle_grid->addWidget(cb, static_cast<int>(i / 4),
                              static_cast<int>(i % 4));
        cycle_lang_checks_.emplace_back(code, cb);
    }
    cycle_vbox->addLayout(cycle_grid);
    cycle_lang_summary_label_ = new QLabel(cycle_wrap);
    cycle_lang_summary_label_->setProperty("mutedText", true);
    cycle_lang_summary_label_->setWordWrap(true);
    cycle_vbox->addWidget(cycle_lang_summary_label_);
    ime_form->addRow("快捷键轮换语言:", cycle_wrap);

    debounce_spin_ = new QSpinBox(ime_group);
    debounce_spin_->setRange(0, 2000);
    debounce_spin_->setSuffix(" ms");
    ime_form->addRow("输入防抖延迟:", debounce_spin_);

    timeout_spin_ = new QSpinBox(ime_group);
    timeout_spin_->setRange(500, 30000);
    timeout_spin_->setSuffix(" ms");
    ime_form->addRow("请求超时:", timeout_spin_);

    cache_size_spin_ = new QSpinBox(ime_group);
    cache_size_spin_->setRange(16, 20000);
    cache_size_spin_->setSuffix(" 条");
    ime_form->addRow("内存缓存条数:", cache_size_spin_);

    auto_detect_check_ = new QCheckBox(
        "自动识别源语言并在目标语言 / 中文间智能切换方向", ime_group);
    lock_punct_check_ =
        new QCheckBox("句末标点自动锁定前半句译文", ime_group);
    esc_pinyin_check_ =
        new QCheckBox("按 Esc 时优先交由拼音清除未选词", ime_group);
    offline_fallback_check_ =
        new QCheckBox("无网或请求失败时降级使用离线词典", host);
    show_source_line_check_ =
        new QCheckBox("输入法候选框上方显示原文与插入光标", ime_group);
    collapse_idle_check_ =
        new QCheckBox("Sticky 模式空闲无输入时自动折叠面板", ime_group);
    expand_actions_check_ =
        new QCheckBox("始终展开输入法底部操作菜单（关闭则精简展示，按 ↓ 展开）",
                      ime_group);

    auto *checks_box = new QWidget(ime_group);
    auto *checks_lay = new QVBoxLayout(checks_box);
    checks_lay->setContentsMargins(0, 2, 0, 0);
    checks_lay->setSpacing(4);
    checks_lay->addWidget(auto_detect_check_);
    checks_lay->addWidget(lock_punct_check_);
    checks_lay->addWidget(esc_pinyin_check_);
    checks_lay->addWidget(show_source_line_check_);
    checks_lay->addWidget(collapse_idle_check_);
    checks_lay->addWidget(expand_actions_check_);
    ime_form->addRow(checks_box);

    col->addWidget(ime_group);

    // 4. Multi-language & external offline dictionary manager
    auto *dict_group = new QGroupBox("多语言离线词典与外部词库管理", host);
    auto *dict_lay = new QVBoxLayout(dict_group);
    dict_lay->setSpacing(6);

    auto *dict_hint = new QLabel(
        "支持 .tsv / .csv (ECDICT) / .json / .jsonl / .u8 (CC-CEDICT) / "
        ".ifo+.idx+.dict(.dz) (StarDict) / .db (SQLite)；首行可写 # pair: ja->zh "
        "或直接在下表指定源/目标语言。",
        dict_group);
    dict_hint->setProperty("mutedText", true);
    dict_hint->setWordWrap(true);
    dict_lay->addWidget(dict_hint);
    dict_lay->addWidget(offline_fallback_check_);

    auto *dict_bar = new QHBoxLayout();
    dict_bar->setSpacing(6);
    auto *import_dict_btn = new QPushButton("＋ 导入外部词典…", dict_group);
    import_dict_btn->setObjectName("PrimaryButton");
    dict_copy_managed_check_ =
        new QCheckBox("导入时复制到托管目录 (~/.local/share/shadowbar/dicts)", dict_group);
    dict_copy_managed_check_->setChecked(true);
    auto *reload_dict_btn = new QPushButton("↻ 扫描重载", dict_group);
    reload_dict_btn->setObjectName("SecondaryButton");
    auto *open_dict_dir_btn = new QPushButton("📂 词典目录", dict_group);
    open_dict_dir_btn->setObjectName("SecondaryButton");

    dict_bar->addWidget(import_dict_btn);
    dict_bar->addWidget(dict_copy_managed_check_, 1);
    dict_bar->addWidget(reload_dict_btn);
    dict_bar->addWidget(open_dict_dir_btn);
    dict_lay->addLayout(dict_bar);

    dict_table_ = new QTableWidget(0, 6, dict_group);
    dict_table_->setHorizontalHeaderLabels(
        {"启用", "词典文件", "源语言", "目标语言", "格式 / 状态", "操作"});
    dict_table_->verticalHeader()->setVisible(false);
    dict_table_->setSelectionMode(QAbstractItemView::NoSelection);
    dict_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    dict_table_->setMinimumHeight(145);
    dict_table_->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::ResizeToContents);
    dict_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    dict_table_->horizontalHeader()->setSectionResizeMode(
        2, QHeaderView::ResizeToContents);
    dict_table_->horizontalHeader()->setSectionResizeMode(
        3, QHeaderView::ResizeToContents);
    dict_table_->horizontalHeader()->setSectionResizeMode(
        4, QHeaderView::ResizeToContents);
    dict_table_->horizontalHeader()->setSectionResizeMode(
        5, QHeaderView::ResizeToContents);
    dict_lay->addWidget(dict_table_);

    offline_dict_status_label_ = new QLabel(dict_group);
    offline_dict_status_label_->setProperty("mutedText", true);
    offline_dict_status_label_->setWordWrap(true);
    dict_lay->addWidget(offline_dict_status_label_);

    connect(import_dict_btn, &QPushButton::clicked, this,
            &SettingsView::onImportDictionary);
    connect(reload_dict_btn, &QPushButton::clicked, this, [this]() {
        syncDictRowsFromTable();
        updateOfflineDictStatus();
    });
    connect(open_dict_dir_btn, &QPushButton::clicked, this,
            &SettingsView::openManagedDictDir);

    col->addWidget(dict_group);
    col->addStretch(1);

    scroll->setWidget(host);
    return scroll;
}

void SettingsView::setConfig(const ShadowConfig &config) {
    working_config_ = config;
    if (working_config_.presets.empty()) {
        working_config_.presets = ShadowConfig::makeDefault().presets;
    }

    int active_row = 0;
    for (int i = 0; i < static_cast<int>(working_config_.presets.size()); ++i) {
        if (working_config_.presets[i].id == working_config_.active_preset) {
            active_row = i;
            break;
        }
    }
    editing_preset_row_ = -1;
    refreshPresetList(active_row);
    loadGeneralFields();
    loadTyFields();
}

void SettingsView::setCurrentTab(int index) {
    if (tabs_ && index >= 0 && index < tabs_->count()) {
        tabs_->setCurrentIndex(index);
    }
}

void SettingsView::applyTheme() {
    const auto t = resolveThemeTokens();
    setStyleSheet(QString(
        "QGroupBox {"
        "  border: 1px solid %1;"
        "  border-radius: 8px;"
        "  margin-top: 10px;"
        "  padding-top: 10px;"
        "  color: %2;"
        "  font-weight: 600;"
        "}"
        "QGroupBox::title {"
        "  subcontrol-origin: margin;"
        "  left: 10px;"
        "  padding: 0 4px;"
        "}"
        "QListWidget, QLineEdit, QPlainTextEdit, QComboBox, QSpinBox, QKeySequenceEdit {"
        "  background: %3;"
        "  color: %2;"
        "  border: 1px solid %1;"
        "  border-radius: 6px;"
        "  padding: 4px 7px;"
        "}"
        "QListWidget::item { padding: 6px 8px; border-radius: 5px; }"
        "QListWidget::item:selected { background: %4; color: %2; font-weight: 600; }"
        "QTabWidget::pane { border: 1px solid %1; border-radius: 8px; }"
        "QTabBar::tab {"
        "  padding: 6px 14px;"
        "  margin-right: 4px;"
        "  border-top-left-radius: 6px;"
        "  border-top-right-radius: 6px;"
        "  color: %5;"
        "  background: %6;"
        "}"
        "QTabBar::tab:selected {"
        "  color: %2;"
        "  background: %7;"
        "  font-weight: 600;"
        "}"
        "QLabel#SubtleStatus { color: %5; font-size: 12px; }"
        "QPushButton#ChipButton {"
        "  background: %6;"
        "  color: %2;"
        "  border: 1px solid %1;"
        "  border-radius: 10px;"
        "  padding: 2px 8px;"
        "  font-size: 11px;"
        "}"
        "QPushButton#ChipButton:hover { background: %8; }")
                      .arg(cssRgba(t.border), t.text_primary.name(),
                           cssRgba(t.surface_input), cssRgba(t.accent_soft),
                           t.text_secondary.name(), cssRgba(t.pill_bg),
                           cssRgba(t.surface_elevated), cssRgba(t.hover_bg)));
}

void SettingsView::refreshPresetList(int select_row) {
    updating_preset_ui_ = true;
    preset_list_->clear();
    for (const auto &p : working_config_.presets) {
        const bool is_def = (p.id == working_config_.active_preset);
        preset_list_->addItem(QString("%1%2 · %3")
                                  .arg(is_def ? "★ " : "",
                                       QString::fromStdString(p.label),
                                       QString::fromStdString(p.id)));
    }
    const int clamped =
        std::clamp(select_row, 0, preset_list_->count() - 1);
    updating_preset_ui_ = false;
    if (preset_list_->count() > 0) {
        preset_list_->setCurrentRow(clamped);
        loadPresetFields(clamped);
    }
    del_preset_btn_->setEnabled(working_config_.presets.size() > 1);
}

void SettingsView::onPresetRowChanged(int row) {
    if (updating_preset_ui_ || row < 0 ||
        row >= static_cast<int>(working_config_.presets.size())) {
        return;
    }
    if (editing_preset_row_ >= 0 && editing_preset_row_ != row) {
        storeCurrentPresetFields();
    }
    loadPresetFields(row);
}

void SettingsView::loadPresetFields(int row) {
    if (row < 0 || row >= static_cast<int>(working_config_.presets.size())) {
        return;
    }
    updating_preset_ui_ = true;
    editing_preset_row_ = row;
    const Preset &p = working_config_.presets[row];
    preset_id_edit_->setText(QString::fromStdString(p.id));
    preset_label_edit_->setText(QString::fromStdString(p.label));
    selectComboByData(preset_provider_combo_,
                      QString::fromStdString(ConfigManager::providerToString(p.provider)));
    const std::string norm_tgt =
        p.target_lang.empty() ? std::string("")
                              : normalizeLanguageCode(p.target_lang);
    selectComboByData(preset_lang_combo_, QString::fromStdString(norm_tgt));
    preset_endpoint_edit_->setText(QString::fromStdString(p.endpoint));
    preset_model_edit_->setText(QString::fromStdString(p.model));
    preset_apikey_edit_->setText(QString::fromStdString(p.api_key));
    preset_prompt_edit_->setPlainText(QString::fromStdString(p.system_prompt));
    test_result_label_->setText("测试样本: 按目标语言自动匹配测试句");
    updating_preset_ui_ = false;
    updateProviderFieldStates();
}

void SettingsView::storeCurrentPresetFields() {
    if (editing_preset_row_ < 0 ||
        editing_preset_row_ >= static_cast<int>(working_config_.presets.size())) {
        return;
    }
    Preset &p = working_config_.presets[editing_preset_row_];
    const std::string old_id = p.id;
    std::string clean_id =
        ConfigManager::sanitizePresetId(preset_id_edit_->text().toStdString());
    if (clean_id.empty()) {
        clean_id = old_id.empty() ? "custom" : old_id;
    }
    // Keep ID unique across presets
    for (int i = 0; i < static_cast<int>(working_config_.presets.size()); ++i) {
        if (i != editing_preset_row_ &&
            working_config_.presets[i].id == clean_id) {
            clean_id += "_" + std::to_string(editing_preset_row_ + 1);
            break;
        }
    }
    p.id = clean_id;
    if (working_config_.active_preset == old_id) {
        working_config_.active_preset = p.id;
    }
    const std::string lbl = preset_label_edit_->text().trimmed().toStdString();
    p.label = lbl.empty() ? p.id : lbl;
    p.provider = ConfigManager::providerFromString(
        preset_provider_combo_->currentData().toString().toStdString());
    p.target_lang = preset_lang_combo_->currentData().toString().toStdString();
    p.endpoint = preset_endpoint_edit_->text().trimmed().toStdString();
    p.model = preset_model_edit_->text().trimmed().toStdString();
    p.api_key = preset_apikey_edit_->text().trimmed().toStdString();
    p.system_prompt = preset_prompt_edit_->toPlainText().trimmed().toStdString();
}

void SettingsView::updateProviderFieldStates() {
    const ProviderKind kind = ConfigManager::providerFromString(
        preset_provider_combo_->currentData().toString().toStdString());
    const bool needs_endpoint =
        (kind == ProviderKind::OpenAICompatible || kind == ProviderKind::DeepL ||
         kind == ProviderKind::Ollama);
    const bool needs_model_prompt =
        (kind == ProviderKind::OpenAICompatible || kind == ProviderKind::Ollama);
    const bool needs_key =
        (kind == ProviderKind::OpenAICompatible || kind == ProviderKind::DeepL);

    preset_endpoint_edit_->setEnabled(needs_endpoint);
    preset_model_edit_->setEnabled(needs_model_prompt);
    preset_prompt_edit_->setEnabled(needs_model_prompt);
    preset_apikey_edit_->setEnabled(needs_key);
    apikey_reveal_btn_->setEnabled(needs_key);
}

void SettingsView::applyQuickTemplate(ProviderKind kind,
                                      const QString &endpoint,
                                      const QString &model,
                                      const QString &api_key_hint) {
    selectComboByData(
        preset_provider_combo_,
        QString::fromStdString(ConfigManager::providerToString(kind)));
    preset_endpoint_edit_->setText(endpoint);
    preset_model_edit_->setText(model);
    if (kind == ProviderKind::OpenAICompatible &&
        isHyMtModelName(model.toStdString())) {
        preset_apikey_edit_->clear();
        preset_prompt_edit_->clear();
    } else if (!api_key_hint.isEmpty() &&
               preset_apikey_edit_->text().trimmed().isEmpty()) {
        preset_apikey_edit_->setText(api_key_hint);
    }
    updateProviderFieldStates();
}

void SettingsView::onAddPreset() {
    storeCurrentPresetFields();
    Preset p;
    int suffix = static_cast<int>(working_config_.presets.size()) + 1;
    p.id = "custom_" + std::to_string(suffix);
    p.label = "自定义" + std::to_string(suffix);
    p.provider = ProviderKind::OpenAICompatible;
    p.endpoint = "https://api.openai.com/v1/chat/completions";
    p.model = "gpt-4o-mini";
    p.api_key = "env:OPENAI_API_KEY";
    p.target_lang = "";
    working_config_.presets.push_back(std::move(p));
    refreshPresetList(static_cast<int>(working_config_.presets.size()) - 1);
}

void SettingsView::onDeletePreset() {
    if (working_config_.presets.size() <= 1 || editing_preset_row_ < 0) {
        return;
    }
    const int remove_idx = editing_preset_row_;
    const std::string removed_id = working_config_.presets[remove_idx].id;
    working_config_.presets.erase(working_config_.presets.begin() + remove_idx);
    if (working_config_.active_preset == removed_id) {
        working_config_.active_preset = working_config_.presets.front().id;
    }
    editing_preset_row_ = -1;
    refreshPresetList(std::max(0, remove_idx - 1));
}

void SettingsView::onSetDefaultPreset() {
    storeCurrentPresetFields();
    if (editing_preset_row_ >= 0 &&
        editing_preset_row_ < static_cast<int>(working_config_.presets.size())) {
        working_config_.active_preset =
            working_config_.presets[editing_preset_row_].id;
        refreshPresetList(editing_preset_row_);
    }
}

void SettingsView::onTestPreset() {
    storeCurrentPresetFields();
    if (editing_preset_row_ < 0 ||
        editing_preset_row_ >= static_cast<int>(working_config_.presets.size())) {
        return;
    }
    Preset preset = working_config_.presets[editing_preset_row_];
    if (preset.target_lang.empty()) {
        preset.target_lang =
            default_target_lang_combo_
                ? default_target_lang_combo_->currentData().toString().toStdString()
                : working_config_.target_lang;
        if (preset.target_lang.empty()) {
            preset.target_lang = "en";
        }
    }
    const bool test_en_to_zh = (normalizeLanguageCode(preset.target_lang) == "zh");
    test_preset_btn_->setEnabled(false);
    test_result_label_->setText("⏳ 正在请求测试…");

    const auto start_tp = std::chrono::steady_clock::now();
    QPointer<SettingsView> self(this);
    const int test_timeout_ms =
        (preset.provider == ProviderKind::OpenAICompatible ||
         preset.provider == ProviderKind::Ollama ||
         preset.provider == ProviderKind::DeepL)
            ? std::max(timeout_spin_->value(), 15000)
            : std::max(timeout_spin_->value(), 8000);

    TranslationJob job{
        .revision = 1,
        .source_text = test_en_to_zh ? "Hello, world" : "你好，世界",
        .source_lang = test_en_to_zh ? "en" : "zh",
        .preset = std::move(preset),
        .immediate = true,
        .lock_on_punctuation = false,
        .offline_fallback = false,
        .bypass_cache = true,
        .debounce_ms = 0,
        .timeout_ms = test_timeout_ms,
    };
    test_engine_->clearCache();
    test_engine_->reloadOfflineDictionaries(collectedDictSpecs());
    test_engine_->submit(
        std::move(job),
        [self, start_tp](uint64_t, const TranslationResult &res) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_tp)
                                .count();
            if (!self) {
                return;
            }
            QMetaObject::invokeMethod(
                self.data(),
                [self, res, ms]() {
                    if (!self) {
                        return;
                    }
                    if (!res.is_final) {
                        self->test_result_label_->setText(
                            QString("⏳ 流式响应中 (%1 ms): %2")
                                .arg(ms)
                                .arg(QString::fromStdString(res.translated_text)));
                        return;
                    }
                    self->test_preset_btn_->setEnabled(true);
                    if (res.ok) {
                        self->test_result_label_->setText(
                            QString("✓ 成功 (%1 ms): %2")
                                .arg(ms)
                                .arg(QString::fromStdString(res.translated_text)));
                    } else {
                        self->test_result_label_->setText(
                            QString("✕ 失败 (%1 ms): %2")
                                .arg(ms)
                                .arg(QString::fromStdString(res.error_message)));
                    }
                },
                Qt::QueuedConnection);
        });
}

void SettingsView::loadGeneralFields() {
    floating_hotkey_edit_->setKeySequence(QKeySequence::fromString(
        fcitxToQtKeySequenceString(working_config_.floating_hotkey),
        QKeySequence::PortableText));
    toggle_hotkey_edit_->setKeySequence(QKeySequence::fromString(
        fcitxToQtKeySequenceString(working_config_.toggle_hotkey),
        QKeySequence::PortableText));
    cycle_lang_hotkey_edit_->setKeySequence(QKeySequence::fromString(
        fcitxToQtKeySequenceString(working_config_.cycle_lang_hotkey),
        QKeySequence::PortableText));
    cycle_preset_hotkey_edit_->setKeySequence(QKeySequence::fromString(
        fcitxToQtKeySequenceString(working_config_.cycle_preset_hotkey),
        QKeySequence::PortableText));
    oneshot_hotkey_edit_->setKeySequence(QKeySequence::fromString(
        fcitxToQtKeySequenceString(working_config_.oneshot_hotkey),
        QKeySequence::PortableText));

    floating_enabled_check_->setChecked(working_config_.floating_enabled);
    floating_autoread_check_->setChecked(
        working_config_.floating_auto_read_clipboard);
    floating_watch_check_->setChecked(working_config_.floating_watch_clipboard);
    floating_orb_check_->setChecked(working_config_.floating_show_orb);
    floating_pinned_check_->setChecked(working_config_.floating_pinned);
    floating_result_only_check_->setChecked(working_config_.floating_result_only);
    floating_font_scale_spin_->setValue(
        std::clamp(working_config_.floating_font_scale, 80, 160));
    floating_segment_chars_spin_->setValue(
        std::clamp(working_config_.floating_segment_chars, 150, 4000));

    enable_prefix_check_->setChecked(working_config_.enable_prefix);
    prefix_edit_->setText(QString::fromStdString(working_config_.prefix));
    prefix_cjk_only_check_->setChecked(working_config_.prefix_only_in_cjk_im);
    prefix_timeout_spin_->setValue(working_config_.prefix_timeout_ms);

    selectComboByData(
        floating_source_lang_combo_,
        QString::fromStdString(working_config_.floating_source_lang.empty()
                                   ? std::string("auto")
                                   : normalizeLanguageCode(
                                         working_config_.floating_source_lang)));
    selectComboByData(
        default_target_lang_combo_,
        QString::fromStdString(working_config_.target_lang.empty()
                                   ? std::string("en")
                                   : normalizeLanguageCode(
                                         working_config_.target_lang)));

    if (working_config_.cycle_languages.empty()) {
        working_config_.cycle_languages = {"en", "zh", "ja"};
    }
    for (const auto &pair : cycle_lang_checks_) {
        if (!pair.second) {
            continue;
        }
        const bool checked =
            std::find(working_config_.cycle_languages.begin(),
                      working_config_.cycle_languages.end(),
                      pair.first) != working_config_.cycle_languages.end();
        pair.second->blockSignals(true);
        pair.second->setChecked(checked);
        pair.second->blockSignals(false);
    }
    updateCycleLangSummary();

    debounce_spin_->setValue(working_config_.debounce_ms);
    timeout_spin_->setValue(working_config_.request_timeout_ms);
    cache_size_spin_->setValue(static_cast<int>(working_config_.cache_size));

    auto_detect_check_->setChecked(working_config_.auto_detect_direction);
    lock_punct_check_->setChecked(working_config_.lock_on_punctuation);
    esc_pinyin_check_->setChecked(working_config_.esc_clears_pinyin_first);
    offline_fallback_check_->setChecked(working_config_.offline_dict_fallback);
    if (!working_config_.offline_dicts.empty()) {
        dict_rows_ = working_config_.offline_dicts;
    } else {
        dict_rows_ = working_config_.effectiveOfflineDicts();
    }
    rebuildDictTable();
    show_source_line_check_->setChecked(working_config_.show_source_line);
    collapse_idle_check_->setChecked(
        working_config_.collapse_candidates_when_idle);
    expand_actions_check_->setChecked(working_config_.expand_actions);
    updateOfflineDictStatus();
}

void SettingsView::updateCycleLangSummary() {
    if (!cycle_lang_summary_label_) {
        return;
    }
    QStringList names;
    for (const std::string &code : working_config_.cycle_languages) {
        if (const LanguageInfo *info = findLanguageInfo(code)) {
            names << QString("%1(%2)").arg(QString::fromUtf8(info->short_label),
                                           QString::fromUtf8(info->code));
        } else if (!code.empty()) {
            names << QString::fromStdString(code);
        }
    }
    cycle_lang_summary_label_->setText(
        names.isEmpty()
            ? QStringLiteral("当前轮换顺序: EN(en)")
            : QString("当前轮换顺序: %1").arg(names.join(" → ")));
}

void SettingsView::syncDictRowsFromTable() {
    dict_rows_ = readDictRowsFromUi(dict_table_, dict_rows_);
}

std::vector<ExternalDictSpec> SettingsView::collectedDictSpecs() const {
    return readDictRowsFromUi(dict_table_, dict_rows_);
}

void SettingsView::rebuildDictTable() {
    if (!dict_table_) {
        return;
    }
    dict_table_->setRowCount(0);
    dict_table_->setRowCount(static_cast<int>(dict_rows_.size()));
    for (int r = 0; r < static_cast<int>(dict_rows_.size()); ++r) {
        const ExternalDictSpec &spec = dict_rows_[static_cast<size_t>(r)];
        dict_table_->setRowHeight(r, 30);

        auto *chk_wrap = new QWidget(dict_table_);
        auto *chk_lay = new QHBoxLayout(chk_wrap);
        chk_lay->setContentsMargins(0, 0, 0, 0);
        chk_lay->setAlignment(Qt::AlignCenter);
        auto *chk = new QCheckBox(chk_wrap);
        chk->setChecked(spec.enabled);
        connect(chk, &QCheckBox::toggled, this, [this](bool) {
            syncDictRowsFromTable();
            updateOfflineDictStatus();
        });
        chk_lay->addWidget(chk);
        dict_table_->setCellWidget(r, 0, chk_wrap);

        const QString qpath = QString::fromStdString(spec.path);
        const QString fname = QFileInfo(qpath).fileName();
        const QString qlabel = QString::fromStdString(spec.label);
        const QString disp =
            (!qlabel.isEmpty() && qlabel != QFileInfo(qpath).completeBaseName())
                ? QString("%1 (%2)").arg(qlabel, fname)
                : fname;
        auto *name_item = new QTableWidgetItem(disp.isEmpty() ? qpath : disp);
        name_item->setToolTip(qpath);
        dict_table_->setItem(r, 1, name_item);

        auto *src_cb = new QComboBox(dict_table_);
        populateSourceLangCombo(src_cb, true, QStringLiteral("自动"));
        selectComboByData(
            src_cb,
            QString::fromStdString(spec.source_lang.empty()
                                       ? std::string("auto")
                                       : normalizeLanguageCode(spec.source_lang)));
        connect(src_cb, &QComboBox::currentIndexChanged, this, [this](int) {
            syncDictRowsFromTable();
            updateOfflineDictStatus();
        });
        dict_table_->setCellWidget(r, 2, src_cb);

        auto *tgt_cb = new QComboBox(dict_table_);
        populateSourceLangCombo(tgt_cb, true, QStringLiteral("自动"));
        selectComboByData(
            tgt_cb,
            QString::fromStdString(spec.target_lang.empty()
                                       ? std::string("auto")
                                       : normalizeLanguageCode(spec.target_lang)));
        connect(tgt_cb, &QComboBox::currentIndexChanged, this, [this](int) {
            syncDictRowsFromTable();
            updateOfflineDictStatus();
        });
        dict_table_->setCellWidget(r, 3, tgt_cb);

        auto *status_item = new QTableWidgetItem("待检测");
        dict_table_->setItem(r, 4, status_item);

        auto *rm_btn = new QPushButton("移除", dict_table_);
        rm_btn->setObjectName("ChipButton");
        connect(rm_btn, &QPushButton::clicked, this,
                [this, r]() { onRemoveDictRow(r); });
        dict_table_->setCellWidget(r, 5, rm_btn);
    }
}

void SettingsView::onImportDictionary() {
    QString start_dir = QString::fromStdString(OfflineDictionary::defaultManagedDictDir());
    if (!QDir(start_dir).exists()) {
        start_dir = QDir::homePath();
    }
    const QStringList picked = QFileDialog::getOpenFileNames(
        this, "导入多语言 / 离线词典文件", start_dir,
        "所有支持词典 (*.tsv *.txt *.csv *.json *.jsonl *.u8 *.ifo *.db *.sqlite *.sqlite3 *.dz *.gz);;"
        "TSV / CSV 词表 (*.tsv *.txt *.csv);;StarDict 词典 (*.ifo);;"
        "CC-CEDICT / UTF-8 (*.u8 *.txt);;JSON / JSONL 词典 (*.json *.jsonl);;"
        "SQLite 词库 (*.db *.sqlite *.sqlite3);;所有文件 (*)");
    if (picked.isEmpty()) {
        return;
    }

    syncDictRowsFromTable();
    const QString managed_dir_key = normalizeDictPathKey(
        QString::fromStdString(OfflineDictionary::defaultManagedDictDir()));
    QString warn_msg;

    for (QString src_path : picked) {
        QFileInfo fi(src_path);
        const QString ext = fi.suffix().toLower();
        // If user picked a StarDict payload companion (.idx/.dict/.dz), prefer sibling .ifo.
        if (ext == "idx" || ext == "dict" || ext == "dz") {
            const QString stem = fi.completeBaseName();
            const QString base_dir = fi.absolutePath();
            for (const QString &cand_name : {fi.baseName() + ".ifo", stem + ".ifo"}) {
                const QString ifo_path = QDir(base_dir).filePath(cand_name);
                if (QFileInfo::exists(ifo_path)) {
                    src_path = ifo_path;
                    fi = QFileInfo(src_path);
                    break;
                }
            }
        }

        std::string active_path = src_path.toStdString();
        if (dict_copy_managed_check_ && dict_copy_managed_check_->isChecked() &&
            normalizeDictPathKey(fi.absolutePath()) != managed_dir_key) {
            std::string copied_path;
            std::string copy_err;
            if (OfflineDictionary::copyToManagedDir(active_path, copied_path,
                                                    &copy_err)) {
                active_path = std::move(copied_path);
            } else {
                warn_msg = QString("⚠ 复制到托管目录失败 (%1)，已引用原路径")
                               .arg(QString::fromStdString(copy_err));
            }
        }

        const DictSourceReport rep =
            OfflineDictionary::inspectFile(active_path, "auto", "auto");
        const QString new_key =
            normalizeDictPathKey(QString::fromStdString(active_path));

        bool updated_existing = false;
        for (auto &row : dict_rows_) {
            if (normalizeDictPathKey(QString::fromStdString(row.path)) == new_key) {
                row.path = active_path;
                row.enabled = true;
                if (!rep.source_lang.empty()) {
                    row.source_lang = rep.source_lang;
                }
                if (!rep.target_lang.empty()) {
                    row.target_lang = rep.target_lang;
                }
                updated_existing = true;
                break;
            }
        }

        if (!updated_existing) {
            ExternalDictSpec spec;
            std::string base_id = ConfigManager::sanitizePresetId(
                QFileInfo(QString::fromStdString(active_path))
                    .completeBaseName()
                    .toStdString());
            if (base_id.empty()) {
                base_id = "dict";
            }
            std::string unique_id = base_id;
            int suffix = 2;
            while (std::any_of(dict_rows_.begin(), dict_rows_.end(),
                               [&](const ExternalDictSpec &e) {
                                   return e.id == unique_id;
                               })) {
                unique_id = base_id + "_" + std::to_string(suffix++);
            }
            spec.id = std::move(unique_id);
            spec.path = active_path;
            spec.source_lang =
                rep.source_lang.empty() ? std::string("auto") : rep.source_lang;
            spec.target_lang =
                rep.target_lang.empty() ? std::string("auto") : rep.target_lang;
            spec.enabled = true;
            spec.label = QFileInfo(QString::fromStdString(active_path))
                             .completeBaseName()
                             .toStdString();
            dict_rows_.push_back(std::move(spec));
        }
    }

    rebuildDictTable();
    updateOfflineDictStatus();
    if (!warn_msg.isEmpty() && offline_dict_status_label_) {
        offline_dict_status_label_->setText(
            warn_msg + "\n" + offline_dict_status_label_->text());
    }
    if (footer_status_) {
        footer_status_->setText(
            "✓ 词典列表已更新（点击「✓ 保存并生效」写入 config.toml）");
    }
}

void SettingsView::onRemoveDictRow(int row) {
    syncDictRowsFromTable();
    if (row < 0 || row >= static_cast<int>(dict_rows_.size())) {
        return;
    }
    dict_rows_.erase(dict_rows_.begin() + row);
    rebuildDictTable();
    updateOfflineDictStatus();
}

void SettingsView::openManagedDictDir() {
    const QString dir_path =
        QString::fromStdString(OfflineDictionary::defaultManagedDictDir());
    QDir().mkpath(dir_path);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir_path));
}

void SettingsView::updateOfflineDictStatus() {
    if (!offline_dict_status_label_ || !test_engine_) {
        return;
    }
    const std::vector<ExternalDictSpec> specs = collectedDictSpecs();
    std::vector<ExternalDictSpec> active_specs;
    for (const auto &s : specs) {
        if (s.enabled && !s.path.empty()) {
            active_specs.push_back(s);
        }
    }
    test_engine_->reloadOfflineDictionaries(active_specs);
    const OfflineDictStats st = test_engine_->offlineDictStats();

    QList<QString> matched_keys;
    if (dict_table_) {
        for (int r = 0; r < dict_table_->rowCount(); ++r) {
            QTableWidgetItem *status_item = dict_table_->item(r, 4);
            if (!status_item) {
                continue;
            }
            const bool enabled =
                (r < static_cast<int>(specs.size()) && specs[static_cast<size_t>(r)].enabled);
            if (!enabled) {
                status_item->setText("已停用");
                status_item->setToolTip("取消勾选时不会加载该词典");
                continue;
            }
            const QString row_key = normalizeDictPathKey(
                QString::fromStdString(specs[static_cast<size_t>(r)].path));
            const DictSourceReport *found = nullptr;
            for (const auto &rep : st.source_reports) {
                if (normalizeDictPathKey(QString::fromStdString(rep.path)) == row_key) {
                    found = &rep;
                    break;
                }
            }
            if (found) {
                matched_keys.append(row_key);
                status_item->setText(formatDictReportBadge(*found));
                status_item->setToolTip(QString::fromStdString(found->message));
            } else {
                const DictSourceReport probe = OfflineDictionary::inspectFile(
                    specs[static_cast<size_t>(r)].path,
                    specs[static_cast<size_t>(r)].source_lang,
                    specs[static_cast<size_t>(r)].target_lang);
                status_item->setText(formatDictReportBadge(probe));
                status_item->setToolTip(QString::fromStdString(probe.message));
            }
        }
    }

    QStringList pairs;
    for (const auto &p : st.supported_pairs) {
        pairs << QString::fromStdString(p);
    }
    QStringList extra_notes;
    for (const auto &rep : st.source_reports) {
        const QString key = normalizeDictPathKey(QString::fromStdString(rep.path));
        if (!matched_keys.contains(key)) {
            extra_notes << QString("%1: %2")
                               .arg(QFileInfo(QString::fromStdString(rep.path))
                                        .fileName(),
                                    formatDictReportBadge(rep));
        }
    }

    QString msg = QString("内置词条：中→英 %1 · 英→中 %2 · 多语言扩展词条：%3")
                      .arg(st.zh_to_en_entries)
                      .arg(st.en_to_zh_entries)
                      .arg(st.multi_lang_entries);
    if (!pairs.isEmpty()) {
        msg += QString("\n支持语言对: %1").arg(pairs.join(", "));
    }
    if (!extra_notes.isEmpty()) {
        msg += QString("\n自动扫描托管词典: %1").arg(extra_notes.join(" | "));
    } else if (specs.empty()) {
        msg += QString("\n提示: 点击「＋ 导入外部词典…」或放入 %1")
                   .arg(QString::fromStdString(
                       OfflineDictionary::defaultManagedDictDir()));
    }
    offline_dict_status_label_->setText(msg);
}

ShadowConfig SettingsView::collectConfig() const {
    ShadowConfig cfg = working_config_;
    cfg.floating_hotkey =
        qtKeySequenceToFcitx(floating_hotkey_edit_->keySequence());
    cfg.toggle_hotkey = qtKeySequenceToFcitx(toggle_hotkey_edit_->keySequence());
    cfg.cycle_lang_hotkey =
        qtKeySequenceToFcitx(cycle_lang_hotkey_edit_->keySequence());
    cfg.cycle_preset_hotkey =
        qtKeySequenceToFcitx(cycle_preset_hotkey_edit_->keySequence());
    cfg.oneshot_hotkey =
        qtKeySequenceToFcitx(oneshot_hotkey_edit_->keySequence());

    cfg.floating_enabled = floating_enabled_check_->isChecked();
    cfg.floating_auto_read_clipboard = floating_autoread_check_->isChecked();
    cfg.floating_watch_clipboard = floating_watch_check_->isChecked();
    cfg.floating_show_orb = floating_orb_check_->isChecked();
    cfg.floating_pinned = floating_pinned_check_->isChecked();
    cfg.floating_result_only = floating_result_only_check_->isChecked();
    cfg.floating_font_scale = floating_font_scale_spin_->value();
    cfg.floating_segment_chars = floating_segment_chars_spin_->value();

    cfg.enable_prefix = enable_prefix_check_->isChecked();
    const std::string pfx = prefix_edit_->text().trimmed().toStdString();
    cfg.prefix = pfx.empty() ? ";;" : pfx;
    cfg.prefix_only_in_cjk_im = prefix_cjk_only_check_->isChecked();
    cfg.prefix_timeout_ms = prefix_timeout_spin_->value();

    const std::string fsrc =
        floating_source_lang_combo_->currentData().toString().toStdString();
    cfg.floating_source_lang = fsrc.empty() ? std::string("auto") : fsrc;
    cfg.target_lang =
        default_target_lang_combo_->currentData().toString().toStdString();
    if (cfg.target_lang.empty()) {
        cfg.target_lang = "en";
    }
    std::vector<std::string> cycle;
    for (const std::string &code : working_config_.cycle_languages) {
        if (!code.empty() &&
            std::find(cycle.begin(), cycle.end(), code) == cycle.end()) {
            cycle.push_back(code);
        }
    }
    if (cycle.empty()) {
        cycle = {"en", "zh", "ja"};
    }
    cfg.cycle_languages = std::move(cycle);

    cfg.debounce_ms = debounce_spin_->value();
    cfg.request_timeout_ms = timeout_spin_->value();
    cfg.cache_size = static_cast<size_t>(cache_size_spin_->value());

    cfg.auto_detect_direction = auto_detect_check_->isChecked();
    cfg.lock_on_punctuation = lock_punct_check_->isChecked();
    cfg.esc_clears_pinyin_first = esc_pinyin_check_->isChecked();
    cfg.offline_dict_fallback = offline_fallback_check_->isChecked();
    cfg.offline_dicts = collectedDictSpecs();
    cfg.offline_dict_path.clear();
    cfg.show_source_line = show_source_line_check_->isChecked();
    cfg.collapse_candidates_when_idle = collapse_idle_check_->isChecked();
    cfg.expand_actions = expand_actions_check_->isChecked();
    collectTyFields(cfg);
    return cfg;
}

void SettingsView::onSaveClicked() {
    storeCurrentPresetFields();
    ShadowConfig updated = collectConfig();
    working_config_ = updated;
    refreshPresetList(std::max(0, editing_preset_row_));
    footer_status_->setText("✓ 已写入 config.toml 并通知 Fcitx5 热重载");
    emit configSaved(updated);
}

void SettingsView::onResetDefaults() {
    setConfig(ShadowConfig::makeDefault());
    footer_status_->setText("已恢复默认值，点击「✓ 保存并生效」后写入配置");
}

void SettingsView::setTyModelManager(TyModelManager *manager) {
    if (ty_mgr_ == manager) {
        return;
    }
    if (ty_mgr_) {
        ty_mgr_->disconnect(this);
    }
    ty_mgr_ = manager;
    if (!ty_mgr_) {
        refreshTyServiceStatus();
        return;
    }

    connect(ty_mgr_, &TyModelManager::stateChanged, this,
            [this](TyModelManager::ServerState state, const QString &msg) {
                refreshTyServiceStatus();
                if (!ty_trial_waiting_server_) {
                    return;
                }
                if (state == TyModelManager::ServerState::Running ||
                    state == TyModelManager::ServerState::ExternalRunning) {
                    const QString variant = ty_trial_waiting_variant_;
                    ty_trial_waiting_server_ = false;
                    ty_trial_waiting_variant_.clear();
                    runTyTrialTranslation(variant, currentTyEffectiveConfig());
                } else if (state == TyModelManager::ServerState::Error ||
                           state == TyModelManager::ServerState::Stopped) {
                    ty_trial_waiting_server_ = false;
                    ty_trial_waiting_variant_.clear();
                    if (ty_trial_btn_) {
                        ty_trial_btn_->setEnabled(true);
                    }
                    if (ty_trial_status_) {
                        ty_trial_status_->setText(
                            QString("⚠ 服务未能启动: %1").arg(msg));
                    }
                }
            });

    connect(ty_mgr_, &TyModelManager::logAppended, this,
            [this](const QString &line) {
                if (ty_log_view_) {
                    ty_log_view_->appendPlainText(line);
                }
            });

    connect(ty_mgr_, &TyModelManager::downloadProgress, this,
            [this](const QString &, int percent, const QString &detail) {
                if (ty_dl_progress_) {
                    ty_dl_progress_->setVisible(true);
                    ty_dl_progress_->setValue(percent);
                }
                if (ty_service_badge_) {
                    ty_service_badge_->setText(detail);
                }
            });

    connect(ty_mgr_, &TyModelManager::downloadFinished, this,
            [this](const QString &variant, bool ok, const QString &msg) {
                if (ty_download_btn_) {
                    ty_download_btn_->setEnabled(true);
                }
                if (ty_cancel_dl_btn_) {
                    ty_cancel_dl_btn_->setEnabled(false);
                }
                if (ty_dl_progress_ && !ok) {
                    ty_dl_progress_->setVisible(false);
                }
                refreshTyFileStatus();
                if (footer_status_) {
                    footer_status_->setText(
                        QString("%1 %2: %3")
                            .arg(ok ? "✓" : "⚠", variant, msg));
                }
            });

    if (ty_log_view_) {
        ty_log_view_->setPlainText(ty_mgr_->recentLogs().join(QLatin1Char('\n')));
    }
    refreshTyServiceStatus();
    ty_mgr_->probeHealthOnce(
        ty_port_spin_ ? ty_port_spin_->value() : working_config_.hymt_port);
}

QWidget *SettingsView::buildTyModelTab() {
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *host = new QWidget(scroll);
    auto *col = new QVBoxLayout(host);
    col->setContentsMargins(6, 6, 6, 6);
    col->setSpacing(10);

    // 1. Variant selection & GGUF model files
    auto *model_group =
        new QGroupBox("混元 HY-MT1.5-1.8B 本地量化模型 (1.25bit STQ1_0 / 2bit Q2_0C)", host);
    auto *m_lay = new QVBoxLayout(model_group);
    m_lay->setSpacing(6);

    auto *variant_group = new QButtonGroup(model_group);
    ty_variant_125_ = new QRadioButton(
        "1.25bit · STQ1_0 (约 441 MB，体积最小 / 内存更省 / 实测 ~60 tok/s)",
        model_group);
    ty_variant_2_ = new QRadioButton(
        "2bit · Q2_0C (约 573 MB，译文细节更稳 / 实测 ~65 tok/s · 推荐)",
        model_group);
    ty_variant_custom_ = new QRadioButton("自定义本地 GGUF 模型路径", model_group);
    variant_group->addButton(ty_variant_125_);
    variant_group->addButton(ty_variant_2_);
    variant_group->addButton(ty_variant_custom_);
    m_lay->addWidget(ty_variant_2_);
    m_lay->addWidget(ty_variant_125_);
    m_lay->addWidget(ty_variant_custom_);

    ty_files_status_ = new QLabel(model_group);
    ty_files_status_->setObjectName("SubtleStatus");
    ty_files_status_->setWordWrap(true);
    m_lay->addWidget(ty_files_status_);

    auto *m_form = new QFormLayout();
    m_form->setSpacing(6);
    m_form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    auto make_browse_row = [&](QLineEdit *&edit_out,
                               const QString &placeholder,
                               const QString &dialog_title,
                               const QString &filter) {
        auto *row = new QHBoxLayout();
        row->setSpacing(4);
        edit_out = new QLineEdit(model_group);
        edit_out->setPlaceholderText(placeholder);
        auto *btn = new QPushButton("浏览...", model_group);
        btn->setObjectName("SecondaryButton");
        connect(edit_out, &QLineEdit::textChanged, this,
                &SettingsView::refreshTyFileStatus);
        connect(btn, &QPushButton::clicked, this, [this, edit_out, dialog_title, filter]() {
            const QString start_dir =
                edit_out->text().trimmed().isEmpty()
                    ? defaultShadowbarShareDir()
                    : QFileInfo(edit_out->text().trimmed()).absolutePath();
            const QString picked =
                QFileDialog::getOpenFileName(this, dialog_title, start_dir, filter);
            if (!picked.isEmpty()) {
                edit_out->setText(picked);
            }
        });
        row->addWidget(edit_out, 1);
        row->addWidget(btn);
        return row;
    };

    m_form->addRow(
        "1.25bit GGUF:",
        make_browse_row(ty_model_125_edit_,
                        defaultTyModelPath(QStringLiteral("1.25bit")),
                        "选择 HY-MT1.5 1.25bit GGUF 模型",
                        "GGUF Models (*.gguf);;All Files (*)"));
    m_form->addRow(
        "2bit GGUF:",
        make_browse_row(ty_model_2_edit_,
                        defaultTyModelPath(QStringLiteral("2bit")),
                        "选择 HY-MT1.5 2bit GGUF 模型",
                        "GGUF Models (*.gguf);;All Files (*)"));
    m_form->addRow(
        "自定义 GGUF:",
        make_browse_row(ty_model_custom_edit_,
                        "/path/to/custom-hy-mt.gguf",
                        "选择自定义 HY-MT GGUF 模型",
                        "GGUF Models (*.gguf);;All Files (*)"));

    auto *dl_row = new QHBoxLayout();
    dl_row->setSpacing(6);
    ty_mirror_combo_ = new QComboBox(model_group);
    ty_mirror_combo_->addItem("HuggingFace 官方", "huggingface");
    ty_mirror_combo_->addItem("HF-Mirror 国内镜像", "hf-mirror");

    ty_download_btn_ = new QPushButton("⬇ 下载当前选中版本", model_group);
    ty_download_btn_->setObjectName("SecondaryButton");
    connect(ty_download_btn_, &QPushButton::clicked, this,
            &SettingsView::onTyDownloadClicked);

    ty_cancel_dl_btn_ = new QPushButton("取消下载", model_group);
    ty_cancel_dl_btn_->setObjectName("SecondaryButton");
    ty_cancel_dl_btn_->setEnabled(false);
    connect(ty_cancel_dl_btn_, &QPushButton::clicked, this, [this]() {
        if (ty_mgr_) {
            ty_mgr_->cancelDownload();
        }
    });

    dl_row->addWidget(ty_mirror_combo_);
    dl_row->addWidget(ty_download_btn_);
    dl_row->addWidget(ty_cancel_dl_btn_);
    dl_row->addStretch(1);
    m_form->addRow("模型下载源:", dl_row);

    ty_dl_progress_ = new QProgressBar(model_group);
    ty_dl_progress_->setRange(0, 100);
    ty_dl_progress_->setVisible(false);
    m_lay->addLayout(m_form);
    m_lay->addWidget(ty_dl_progress_);
    col->addWidget(model_group);

    // 2. llama-server process control & preset wiring
    auto *svc_group = new QGroupBox("本地 llama-server 推理服务与快捷接入", host);
    auto *s_lay = new QVBoxLayout(svc_group);
    s_lay->setSpacing(6);

    auto *s_form = new QFormLayout();
    s_form->setSpacing(6);
    s_form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    s_form->addRow(
        "llama-server:",
        make_browse_row(
            ty_server_path_edit_,
            defaultShadowbarShareDir() + QStringLiteral("/bin/llama-server"),
            "选择支持 STQ1_0 / Q2_0C 的 llama-server",
            "All Files (*)"));

    auto *rt_row = new QHBoxLayout();
    rt_row->setSpacing(8);
    ty_port_spin_ = new QSpinBox(svc_group);
    ty_port_spin_->setRange(1024, 65535);
    ty_port_spin_->setToolTip("本地 llama-server OpenAI 兼容端口 (默认 18081)");

    ty_threads_spin_ = new QSpinBox(svc_group);
    ty_threads_spin_->setRange(0, 256);
    ty_threads_spin_->setSpecialValueText("自动");
    ty_threads_spin_->setSuffix(" 线程");

    ty_auto_start_check_ = new QCheckBox("选用 TY 预设时自动启停对应版本", svc_group);
    rt_row->addWidget(new QLabel("端口:", svc_group));
    rt_row->addWidget(ty_port_spin_);
    rt_row->addWidget(new QLabel("线程:", svc_group));
    rt_row->addWidget(ty_threads_spin_);
    rt_row->addWidget(ty_auto_start_check_, 1);
    s_form->addRow("运行参数:", rt_row);

    ty_backend_combo_ = new QComboBox(svc_group);
    ty_backend_combo_->addItem("首选 Vulkan GPU，失败自动回退 CPU（推荐）", "auto");
    ty_backend_combo_->addItem("仅尝试 Vulkan GPU (-ngl 99)", "vulkan");
    ty_backend_combo_->addItem("仅 CPU AVX2 (-ngl 0)", "cpu");
    s_form->addRow("推理后端:", ty_backend_combo_);
    s_lay->addLayout(s_form);

    auto *ctrl_row = new QHBoxLayout();
    ctrl_row->setSpacing(6);
    ty_start_btn_ = new QPushButton("▶ 启动 / 切换当前版本", svc_group);
    ty_start_btn_->setObjectName("PrimaryButton");
    connect(ty_start_btn_, &QPushButton::clicked, this, [this]() {
        onTyStartStopClicked(true);
    });

    ty_stop_btn_ = new QPushButton("■ 停止服务", svc_group);
    ty_stop_btn_->setObjectName("SecondaryButton");
    connect(ty_stop_btn_, &QPushButton::clicked, this, [this]() {
        onTyStartStopClicked(false);
    });

    auto *probe_btn = new QPushButton("↻ 检测端口", svc_group);
    probe_btn->setObjectName("SecondaryButton");
    connect(probe_btn, &QPushButton::clicked, this, [this]() {
        if (ty_mgr_) {
            ty_mgr_->probeHealthOnce(ty_port_spin_->value());
        }
    });

    auto *apply_preset_btn = new QPushButton("★ 创建预设并设为默认", svc_group);
    apply_preset_btn->setObjectName("SecondaryButton");
    connect(apply_preset_btn, &QPushButton::clicked, this, [this]() {
        onTyApplyPresetClicked(true);
    });

    ctrl_row->addWidget(ty_start_btn_);
    ctrl_row->addWidget(ty_stop_btn_);
    ctrl_row->addWidget(probe_btn);
    ctrl_row->addWidget(apply_preset_btn);
    ctrl_row->addStretch(1);
    s_lay->addLayout(ctrl_row);

    ty_service_badge_ = new QLabel("状态: 未运行", svc_group);
    ty_service_badge_->setObjectName("SubtleStatus");
    ty_service_badge_->setWordWrap(true);
    s_lay->addWidget(ty_service_badge_);

    ty_log_view_ = new QPlainTextEdit(svc_group);
    ty_log_view_->setReadOnly(true);
    ty_log_view_->setMaximumBlockCount(120);
    ty_log_view_->setFixedHeight(78);
    ty_log_view_->setPlaceholderText("llama-server 启动与推理日志将显示在此处...");
    s_lay->addWidget(ty_log_view_);
    col->addWidget(svc_group);

    // 3. Built-in instant trial sandbox
    auto *trial_group = new QGroupBox("即时试译对比（自动按当前选择启动/切换 1.25bit 或 2bit）", host);
    auto *t_lay = new QVBoxLayout(trial_group);
    t_lay->setSpacing(6);

    auto *t_top = new QHBoxLayout();
    t_top->setSpacing(6);
    ty_trial_sample_combo_ = new QComboBox(trial_group);
    ty_trial_sample_combo_->addItem(
        "日常问路: 地铁站在哪里",
        "你好，请问附近的地铁站在哪里？");
    ty_trial_sample_combo_->addItem(
        "中文习惯表达: 口是心非",
        "他嘴上说没关系，其实心里一直惦记着这件事。");
    ty_trial_sample_combo_->addItem(
        "技术文档: 量化推理加速",
        "The quantized GGUF inference server now supports AVX2 SIMD acceleration and streams UTF-8 deltas cleanly.");
    ty_trial_sample_combo_->addItem(
        "长句出行: 预订高铁",
        "请帮我预订明天上午九点去上海的高铁票。如果一等座已满，二等座也可以。");

    ty_trial_dir_combo_ = new QComboBox(trial_group);
    ty_trial_dir_combo_->addItem("中 → 英 (en)", "zh|en");
    ty_trial_dir_combo_->addItem("英 → 中 (zh)", "en|zh");
    ty_trial_dir_combo_->addItem("中 → 日 (ja)", "zh|ja");

    ty_trial_btn_ = new QPushButton("⚡ 立即试译", trial_group);
    ty_trial_btn_->setObjectName("PrimaryButton");
    connect(ty_trial_btn_, &QPushButton::clicked, this,
            &SettingsView::onTyTrialTranslate);

    t_top->addWidget(ty_trial_sample_combo_, 1);
    t_top->addWidget(ty_trial_dir_combo_);
    t_top->addWidget(ty_trial_btn_);
    t_lay->addLayout(t_top);

    ty_trial_input_ = new QLineEdit(trial_group);
    ty_trial_input_->setText(
        ty_trial_sample_combo_->currentData().toString());
    ty_trial_input_->setPlaceholderText("输入待测试的句子...");
    connect(ty_trial_sample_combo_, &QComboBox::currentIndexChanged, this,
            [this](int idx) {
                if (idx >= 0 && ty_trial_input_) {
                    const QString txt =
                        ty_trial_sample_combo_->itemData(idx).toString();
                    ty_trial_input_->setText(txt);
                    if (ty_trial_dir_combo_) {
                        const bool has_cjk = txt.contains(
                            QRegularExpression(QStringLiteral("[\\x{4E00}-\\x{9FFF}]")));
                        selectComboByData(ty_trial_dir_combo_,
                                          has_cjk ? "zh|en" : "en|zh");
                    }
                }
            });
    t_lay->addWidget(ty_trial_input_);

    ty_trial_output_ = new QPlainTextEdit(trial_group);
    ty_trial_output_->setReadOnly(true);
    ty_trial_output_->setFixedHeight(68);
    ty_trial_output_->setPlaceholderText("流式译文输出与耗时将显示在这里...");
    t_lay->addWidget(ty_trial_output_);

    ty_trial_status_ = new QLabel("提示: 可直接切换上方 1.25bit / 2bit 单选框后点“立即试译”对比效果与耗时", trial_group);
    ty_trial_status_->setObjectName("SubtleStatus");
    ty_trial_status_->setWordWrap(true);
    t_lay->addWidget(ty_trial_status_);

    col->addWidget(trial_group);
    col->addStretch(1);

    for (QRadioButton *rb : {ty_variant_125_, ty_variant_2_, ty_variant_custom_}) {
        connect(rb, &QRadioButton::toggled, this, [this](bool checked) {
            if (checked) {
                refreshTyFileStatus();
                if (ty_download_btn_) {
                    ty_download_btn_->setEnabled(
                        selectedTyVariant() != QLatin1String("custom") &&
                        !(ty_mgr_ && ty_mgr_->isDownloading()));
                }
            }
        });
    }

    scroll->setWidget(host);
    return scroll;
}

QString SettingsView::selectedTyVariant() const {
    if (ty_variant_125_ && ty_variant_125_->isChecked()) {
        return QStringLiteral("1.25bit");
    }
    if (ty_variant_custom_ && ty_variant_custom_->isChecked()) {
        return QStringLiteral("custom");
    }
    return QStringLiteral("2bit");
}

void SettingsView::loadTyFields() {
    if (!ty_variant_2_) {
        return;
    }
    const QString v =
        normalizeTyVariant(QString::fromStdString(working_config_.hymt_variant));
    if (v == QLatin1String("1.25bit")) {
        ty_variant_125_->setChecked(true);
    } else if (v == QLatin1String("custom")) {
        ty_variant_custom_->setChecked(true);
    } else {
        ty_variant_2_->setChecked(true);
    }

    selectComboByData(
        ty_mirror_combo_,
        working_config_.hymt_mirror == "hf-mirror" ? "hf-mirror" : "huggingface");
    selectComboByData(
        ty_backend_combo_,
        normalizeTyBackend(QString::fromStdString(working_config_.hymt_backend)));
    ty_port_spin_->setValue(std::clamp(working_config_.hymt_port, 1024, 65535));
    ty_threads_spin_->setValue(std::clamp(working_config_.hymt_threads, 0, 256));
    ty_auto_start_check_->setChecked(working_config_.hymt_auto_start);

    ty_server_path_edit_->setText(
        QString::fromStdString(working_config_.hymt_server_path));
    ty_model_125_edit_->setText(
        QString::fromStdString(working_config_.hymt_model_125bit_path));
    ty_model_2_edit_->setText(
        QString::fromStdString(working_config_.hymt_model_2bit_path));
    ty_model_custom_edit_->setText(
        QString::fromStdString(working_config_.hymt_custom_model_path));

    refreshTyFileStatus();
    refreshTyServiceStatus();
}

void SettingsView::collectTyFields(ShadowConfig &cfg) const {
    if (!ty_variant_2_) {
        return;
    }
    cfg.hymt_variant = selectedTyVariant().toStdString();
    cfg.hymt_backend =
        ty_backend_combo_->currentData().toString().toStdString();
    cfg.hymt_port = ty_port_spin_->value();
    cfg.hymt_threads = ty_threads_spin_->value();
    cfg.hymt_mirror =
        ty_mirror_combo_->currentData().toString().toStdString();
    cfg.hymt_auto_start = ty_auto_start_check_->isChecked();
    cfg.hymt_server_path = ty_server_path_edit_->text().trimmed().toStdString();
    cfg.hymt_model_125bit_path =
        ty_model_125_edit_->text().trimmed().toStdString();
    cfg.hymt_model_2bit_path = ty_model_2_edit_->text().trimmed().toStdString();
    cfg.hymt_custom_model_path =
        ty_model_custom_edit_->text().trimmed().toStdString();
}

ShadowConfig SettingsView::currentTyEffectiveConfig() const {
    ShadowConfig cfg = working_config_;
    collectTyFields(cfg);
    return cfg;
}

void SettingsView::refreshTyFileStatus() {
    if (!ty_files_status_) {
        return;
    }
    const ShadowConfig cfg = currentTyEffectiveConfig();
    auto fmt_model = [&](const char *label, const QString &variant) -> QString {
        const QString p = resolvedTyModelPath(cfg, variant);
        const QFileInfo fi(p);
        if (fi.exists() && fi.size() > 1024 * 1024) {
            return QString("✓ %1: %2 MB")
                .arg(label)
                .arg(static_cast<double>(fi.size()) / (1024.0 * 1024.0), 0, 'f', 1);
        }
        return QString("○ %1: 未下载").arg(label);
    };

    const QString bin = resolvedTyServerBinary(cfg);
    const QFileInfo bfi(bin);
    const QString bin_status =
        (bfi.exists() && bfi.isExecutable())
            ? QString("✓ llama-server: %1").arg(bin)
            : QString("⚠ 未检测到 llama-server (%1)").arg(bin);

    ty_files_status_->setText(
        QString("%1  |  %2\n%3")
            .arg(fmt_model("1.25bit", QStringLiteral("1.25bit")),
                 fmt_model("2bit", QStringLiteral("2bit")), bin_status));

    if (ty_download_btn_) {
        ty_download_btn_->setEnabled(
            selectedTyVariant() != QLatin1String("custom") &&
            !(ty_mgr_ && ty_mgr_->isDownloading()));
    }
}

void SettingsView::refreshTyServiceStatus() {
    if (!ty_service_badge_) {
        return;
    }
    if (!ty_mgr_) {
        ty_service_badge_->setText("状态: 管理器未初始化");
        return;
    }
    ty_service_badge_->setText(QString("状态: %1").arg(ty_mgr_->statusMessage()));
    const bool busy = (ty_mgr_->state() == TyModelManager::ServerState::Running ||
                       ty_mgr_->state() == TyModelManager::ServerState::Starting);
    if (ty_stop_btn_) {
        ty_stop_btn_->setEnabled(busy);
    }
}

void SettingsView::onTyDownloadClicked() {
    if (!ty_mgr_) {
        return;
    }
    const QString variant = selectedTyVariant();
    if (variant == QLatin1String("custom")) {
        return;
    }
    const ShadowConfig cfg = currentTyEffectiveConfig();
    const QString dest = resolvedTyModelPath(cfg, variant);
    if (ty_mgr_->startDownload(variant,
                               ty_mirror_combo_->currentData().toString(),
                               dest)) {
        ty_download_btn_->setEnabled(false);
        ty_cancel_dl_btn_->setEnabled(true);
        ty_dl_progress_->setValue(0);
        ty_dl_progress_->setVisible(true);
    }
}

void SettingsView::onTyStartStopClicked(bool start) {
    if (!ty_mgr_) {
        return;
    }
    if (!start) {
        ty_trial_waiting_server_ = false;
        ty_mgr_->stopServer();
        refreshTyServiceStatus();
        return;
    }
    const ShadowConfig cfg = currentTyEffectiveConfig();
    const QString variant = selectedTyVariant();
    const QString bin = resolvedTyServerBinary(cfg);
    const QString model_path = resolvedTyModelPath(cfg, variant);
    ty_mgr_->startServer(bin, variant, model_path, cfg.hymt_port, cfg.hymt_threads,
                         QString::fromStdString(cfg.hymt_backend));
    refreshTyServiceStatus();
}

void SettingsView::onTyApplyPresetClicked(bool set_default) {
    storeCurrentPresetFields();
    ShadowConfig updated = collectConfig();
    const QString variant = selectedTyVariant();
    const QString target_lang;
    upsertTyPreset(updated, variant, set_default, target_lang);
    working_config_ = updated;

    int target_row = 0;
    const QString preset_id = tyModelSpec(variant).preset_id;
    for (int i = 0; i < static_cast<int>(working_config_.presets.size()); ++i) {
        if (working_config_.presets[i].id == preset_id.toStdString()) {
            target_row = i;
            break;
        }
    }
    editing_preset_row_ = -1;
    refreshPresetList(target_row);

    if (footer_status_) {
        footer_status_->setText(
            QString("✓ 已创建并启用预设「%1」，点击底部「保存并生效」或直接返回即可使用")
                .arg(tyModelSpec(variant).preset_label));
    }
    emit configSaved(updated);
}

void SettingsView::onTyTrialTranslate() {
    if (!test_engine_ || !ty_trial_input_ || !ty_trial_output_) {
        return;
    }
    const QString text = ty_trial_input_->text().trimmed();
    if (text.isEmpty()) {
        ty_trial_status_->setText("⚠ 请先输入待试译文本");
        return;
    }

    const ShadowConfig cfg = currentTyEffectiveConfig();
    const QString variant = selectedTyVariant();
    const QString model_path = resolvedTyModelPath(cfg, variant);
    const QString bin_path = resolvedTyServerBinary(cfg);

    if (ty_trial_output_) {
        ty_trial_output_->clear();
    }

    if (ty_mgr_ &&
        !ty_mgr_->isReadyFor(variant, model_path, cfg.hymt_port,
                             QString::fromStdString(cfg.hymt_backend))) {
        if (!ty_mgr_->startServer(bin_path, variant, model_path, cfg.hymt_port,
                                  cfg.hymt_threads,
                                  QString::fromStdString(cfg.hymt_backend))) {
            if (ty_trial_status_) {
                ty_trial_status_->setText(
                    QString("⚠ %1").arg(ty_mgr_->statusMessage()));
            }
            return;
        }
        ty_trial_waiting_server_ = true;
        ty_trial_waiting_variant_ = variant;
        if (ty_trial_btn_) {
            ty_trial_btn_->setEnabled(false);
        }
        if (ty_trial_status_) {
            ty_trial_status_->setText(
                QString("⏳ 正在启动并加载 %1 模型到内存（约 0.7s）...")
                    .arg(variant));
        }
        return;
    }

    runTyTrialTranslation(variant, cfg);
}

void SettingsView::runTyTrialTranslation(const QString &variant,
                                         const ShadowConfig &cfg) {
    if (!test_engine_ || !ty_trial_input_ || !ty_trial_output_) {
        return;
    }
    const QStringList parts =
        ty_trial_dir_combo_->currentData().toString().split(QLatin1Char('|'));
    const std::string src_lang =
        (parts.size() >= 1) ? parts[0].toStdString() : "auto";
    const std::string tgt_lang =
        (parts.size() >= 2) ? parts[1].toStdString() : "en";

    Preset preset =
        makeTyPreset(variant, cfg.hymt_port, QString::fromStdString(tgt_lang));
    const QString text = ty_trial_input_->text().trimmed();

    ty_trial_btn_->setEnabled(false);
    ty_trial_output_->clear();
    ty_trial_status_->setText(
        QString("⏳ 正在调用本地 %1 翻译...").arg(variant));

    QElapsedTimer elapsed;
    elapsed.start();
    QPointer<SettingsView> self(this);

    test_engine_->submit(
        TranslationJob{
            .revision = 0,
            .source_text = text.toStdString(),
            .source_lang = src_lang,
            .preset = std::move(preset),
            .immediate = true,
            .lock_on_punctuation = false,
            .offline_fallback = false,
            .bypass_cache = true,
            .debounce_ms = 0,
            .timeout_ms = 30000,
        },
        [self, variant, elapsed](uint64_t, const TranslationResult &res) {
            if (!self) {
                return;
            }
            QMetaObject::invokeMethod(
                self.data(),
                [self, variant, elapsed, res]() {
                    if (!self) {
                        return;
                    }
                    if (!res.translated_text.empty()) {
                        self->ty_trial_output_->setPlainText(
                            QString::fromStdString(res.translated_text));
                    }
                    if (!res.is_final) {
                        self->ty_trial_status_->setText(
                            QString("⏳ %1 流式生成中 (%2 ms)...")
                                .arg(variant)
                                .arg(elapsed.elapsed()));
                        return;
                    }
                    self->ty_trial_btn_->setEnabled(true);
                    if (res.ok) {
                        self->ty_trial_status_->setText(
                            QString("✓ %1 完成 · 耗时 %2 ms · 输出 %3 字符")
                                .arg(variant)
                                .arg(elapsed.elapsed())
                                .arg(QString::fromStdString(res.translated_text)
                                         .length()));
                    } else {
                        self->ty_trial_status_->setText(
                            QString("✗ %1 失败 (%2 ms): %3")
                                .arg(variant)
                                .arg(elapsed.elapsed())
                                .arg(QString::fromStdString(res.error_message)));
                    }
                },
                Qt::QueuedConnection);
        });
}

QWidget *SettingsView::buildZhEnvTab() {
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *host = new QWidget(scroll);
    auto *col = new QVBoxLayout(host);
    col->setContentsMargins(8, 10, 8, 8);
    col->setSpacing(10);

    auto *intro = new QLabel(
        "针对新装 Linux 主机或刚部署的 Wayland / X11 桌面，一键检查并修复缺失的 "
        "Fcitx5 中文输入法包、zh_CN.UTF-8 Locale、简体中文等宽字体匹配、拼音+ShadowBar "
        "输入法初始化及跨 Wayland / XWayland / X11 环境变量传递。",
        host);
    intro->setProperty("mutedText", true);
    intro->setWordWrap(true);
    col->addWidget(intro);

    // 1. Live 4-item diagnostics
    auto *diag_group = new QGroupBox("环境诊断状态", host);
    auto *diag_lay = new QVBoxLayout(diag_group);
    diag_lay->setSpacing(8);

    zh_env_overview_label_ =
        new QLabel("点击下方「🔍 重新检测」或切换到本页自动扫描当前系统状态。", diag_group);
    zh_env_overview_label_->setWordWrap(true);
    diag_lay->addWidget(zh_env_overview_label_);

    zh_diag_status_labels_.clear();
    zh_diag_detail_labels_.clear();
    static const QStringList kTitles = {
        QStringLiteral("1. 输入法软件与中文 Locale"),
        QStringLiteral("2. 中文与等宽字体匹配"),
        QStringLiteral("3. Fcitx5 中文输入法列表"),
        QStringLiteral("4. Wayland + X11 环境变量与会话集成"),
    };
    for (const QString &t : kTitles) {
        auto *card = new QFrame(diag_group);
        card->setFrameShape(QFrame::StyledPanel);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 6, 8, 6);
        cl->setSpacing(2);

        auto *top = new QHBoxLayout();
        auto *title_lbl = new QLabel(t, card);
        QFont tf = title_lbl->font();
        tf.setBold(true);
        title_lbl->setFont(tf);
        auto *status_lbl = new QLabel("⏳ 待检测", card);
        status_lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        top->addWidget(title_lbl, 1);
        top->addWidget(status_lbl);

        auto *detail_lbl = new QLabel("等待检测…", card);
        detail_lbl->setProperty("mutedText", true);
        detail_lbl->setWordWrap(true);

        cl->addLayout(top);
        cl->addWidget(detail_lbl);
        diag_lay->addWidget(card);

        zh_diag_status_labels_.append(status_lbl);
        zh_diag_detail_labels_.append(detail_lbl);
    }
    col->addWidget(diag_group);

    // 2. Fix options & actions
    auto *act_group = new QGroupBox("一键修复选项与执行", host);
    auto *act_lay = new QVBoxLayout(act_group);
    act_lay->setSpacing(8);

    zh_install_pkg_check_ = new QCheckBox(
        "检测到缺失输入法/字体包或未生成 zh_CN.UTF-8 Locale 时弹窗授权安装 (pkexec)",
        act_group);
    zh_install_pkg_check_->setChecked(true);

    const QString cur_lang = qEnvironmentVariable("LANG").trimmed();
    const bool suggest_zh_locale =
        cur_lang.isEmpty() || cur_lang == "C" || cur_lang == "POSIX" ||
        cur_lang.startsWith("zh_CN", Qt::CaseInsensitive);
    zh_set_locale_check_ = new QCheckBox(
        "同时将默认会话语言写入简体中文 (LANG=zh_CN.UTF-8 / LC_CTYPE)",
        act_group);
    zh_set_locale_check_->setChecked(suggest_zh_locale);

    act_lay->addWidget(zh_install_pkg_check_);
    act_lay->addWidget(zh_set_locale_check_);

    auto *btn_row = new QHBoxLayout();
    btn_row->setSpacing(8);
    zh_diagnose_btn_ = new QPushButton("🔍 重新检测", act_group);
    zh_diagnose_btn_->setObjectName("SecondaryButton");
    zh_fix_btn_ = new QPushButton("🛠 一键修复中文环境与输入法", act_group);
    zh_fix_btn_->setObjectName("PrimaryButton");
    auto *copy_log_btn = new QPushButton("📋 复制诊断日志", act_group);
    copy_log_btn->setObjectName("SecondaryButton");

    btn_row->addWidget(zh_diagnose_btn_);
    btn_row->addWidget(zh_fix_btn_, 1);
    btn_row->addWidget(copy_log_btn);
    act_lay->addLayout(btn_row);
    col->addWidget(act_group);

    // 3. Execution log
    auto *log_group = new QGroupBox("修复详情与执行日志", host);
    auto *log_lay = new QVBoxLayout(log_group);
    zh_fix_log_edit_ = new QPlainTextEdit(log_group);
    zh_fix_log_edit_->setReadOnly(true);
    zh_fix_log_edit_->setMinimumHeight(155);
    zh_fix_log_edit_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    zh_fix_log_edit_->setPlaceholderText(
        "点击「🔍 重新检测」查看 4 项诊断详情，或点击「🛠 一键修复中文环境与输入法」开始修复…");
    log_lay->addWidget(zh_fix_log_edit_);
    col->addWidget(log_group, 1);

    connect(zh_diagnose_btn_, &QPushButton::clicked, this, [this]() {
        if (zh_fixer_ && !zh_fixer_->isBusy()) {
            if (zh_fix_log_edit_) {
                zh_fix_log_edit_->appendPlainText("🔍 正在检测中文环境、字体与 Fcitx5 状态…");
            }
            zh_fixer_->runDiagnose();
        }
    });
    connect(zh_fix_btn_, &QPushButton::clicked, this, [this]() {
        if (!zh_fixer_ || zh_fixer_->isBusy()) {
            return;
        }
        zh_diagnose_btn_->setEnabled(false);
        zh_fix_btn_->setEnabled(false);
        if (zh_fix_log_edit_) {
            zh_fix_log_edit_->clear();
        }
        zh_fixer_->runFix(zh_install_pkg_check_->isChecked(),
                          zh_set_locale_check_->isChecked());
    });
    connect(copy_log_btn, &QPushButton::clicked, this, [this]() {
        if (QClipboard *cb = QGuiApplication::clipboard();
            cb && zh_fix_log_edit_) {
            cb->setText(zh_fix_log_edit_->toPlainText());
            if (footer_status_) {
                footer_status_->setText("✓ 已复制中文环境诊断与修复日志到剪贴板");
            }
        }
    });

    scroll->setWidget(host);
    return scroll;
}

void SettingsView::applyZhEnvReport(const ZhEnvDiagReport &report) {
    if (zh_env_overview_label_) {
        const QString overall =
            report.allOk() ? QStringLiteral("✓ 全部 4 项均就绪（支持 Wayland 与 X11 双端中文输入）")
                           : QStringLiteral("⚠ 检测到可修复项，建议点击「🛠 一键修复中文环境与输入法」");
        zh_env_overview_label_->setText(
            QString("系统: %1 · 会话: %2 (%3) · %4")
                .arg(report.distro_name,
                     report.session_type.isEmpty() ? "unknown" : report.session_type,
                     report.desktop_name.isEmpty() ? "Desktop" : report.desktop_name,
                     overall));
    }

    for (int i = 0; i < report.items.size() && i < zh_diag_status_labels_.size() &&
                    i < zh_diag_detail_labels_.size();
         ++i) {
        const ZhEnvDiagItem &it = report.items[i];
        QLabel *st_lbl = zh_diag_status_labels_[i];
        QLabel *det_lbl = zh_diag_detail_labels_[i];
        if (st_lbl) {
            st_lbl->setText(it.ok ? QStringLiteral("✓ 已就绪")
                                  : (it.warning ? QStringLiteral("⚠ 待修复")
                                                : QStringLiteral("✕ 异常")));
            st_lbl->setStyleSheet(
                it.ok ? QStringLiteral("color: #2ecc71; font-weight: bold;")
                      : QStringLiteral("color: #e67e22; font-weight: bold;"));
        }
        if (det_lbl) {
            det_lbl->setText(it.detail.isEmpty()
                                 ? it.summary
                                 : QString("%1\n%2").arg(it.summary, it.detail));
        }
    }
}

} // namespace shadowbar::floating_ui
