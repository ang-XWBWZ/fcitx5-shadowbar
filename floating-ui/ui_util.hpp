#pragma once

#include "shadowbar/types.hpp"
#include "theme_util.hpp"

#include <QAbstractItemView>
#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QMenu>
#include <QObject>
#include <QPalette>
#include <QPointer>
#include <QString>
#include <QWidget>

namespace shadowbar::floating_ui {

namespace detail {

class OpaquePopupEventFilter final : public QObject {
public:
    OpaquePopupEventFilter(QComboBox *combo, QAbstractItemView *view)
        : QObject(view), combo_(combo), view_(view) {}

    void setOpaquePalette(const QPalette &pal, const QString &container_qss) {
        palette_ = pal;
        container_qss_ = container_qss;
        applyNow();
    }

    void applyNow() const {
        if (!view_) {
            return;
        }
        auto enforce_widget = [this](QWidget *w) {
            if (!w) {
                return;
            }
            w->setAttribute(Qt::WA_TranslucentBackground, false);
            w->setAttribute(Qt::WA_NoSystemBackground, false);
            w->setAutoFillBackground(true);
            w->setPalette(palette_);
        };
        enforce_widget(view_);
        enforce_widget(view_->viewport());
        if (QWidget *popup_win = view_->window()) {
            const QWidget *host_win = combo_ ? combo_->window() : nullptr;
            if (popup_win != host_win &&
                (popup_win->windowFlags().testFlag(Qt::Popup) ||
                 host_win != nullptr)) {
                enforce_widget(popup_win);
                popup_win->setAttribute(Qt::WA_StyledBackground, true);
                if (!container_qss_.isEmpty()) {
                    popup_win->setStyleSheet(container_qss_);
                }
            }
        }
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event &&
            (event->type() == QEvent::Show ||
             event->type() == QEvent::Polish ||
             event->type() == QEvent::StyleChange)) {
            applyNow();
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QPointer<QComboBox> combo_;
    QPointer<QAbstractItemView> view_;
    QPalette palette_;
    QString container_qss_;
};

} // namespace detail

inline void setupOpaqueComboPopup(QComboBox *combo) {
    if (!combo) {
        return;
    }
    QAbstractItemView *view = combo->view();
    if (!view) {
        return;
    }
    view->setFrameShape(QFrame::NoFrame);
    view->setAutoFillBackground(true);
    if (view->viewport()) {
        view->viewport()->setAutoFillBackground(true);
    }

    auto *existing = view->findChild<QObject *>(
        QStringLiteral("__shadowbar_opaque_popup_filter"),
        Qt::FindDirectChildrenOnly);
    if (!existing) {
        auto *filter = new detail::OpaquePopupEventFilter(combo, view);
        filter->setObjectName(QStringLiteral("__shadowbar_opaque_popup_filter"));
        view->installEventFilter(filter);
        if (QWidget *popup_win = view->window()) {
            if (popup_win != combo->window()) {
                popup_win->installEventFilter(filter);
            }
        }
    }
}

inline void applyOpaqueComboTheme(QComboBox *combo, const ThemeTokens &t) {
    if (!combo) {
        return;
    }
    setupOpaqueComboPopup(combo);
    QAbstractItemView *view = combo->view();
    if (!view) {
        return;
    }

    QPalette pal = view->palette();
    pal.setColor(QPalette::Window, t.popup_bg);
    pal.setColor(QPalette::Base, t.popup_bg);
    pal.setColor(QPalette::AlternateBase, t.popup_bg);
    pal.setColor(QPalette::Button, t.popup_bg);
    pal.setColor(QPalette::WindowText, t.text_primary);
    pal.setColor(QPalette::Text, t.text_primary);
    pal.setColor(QPalette::ButtonText, t.text_primary);
    pal.setColor(QPalette::Highlight, t.accent);
    pal.setColor(QPalette::HighlightedText, t.accent_text);

    const QString view_qss = QString(
        "QAbstractItemView {"
        "  background-color: %1;"
        "  color: %2;"
        "  border: 1px solid %3;"
        "  border-radius: 8px;"
        "  padding: 4px;"
        "  outline: none;"
        "  selection-background-color: %4;"
        "  selection-color: %5;"
        "}"
        "QAbstractItemView::item {"
        "  background-color: %1;"
        "  color: %2;"
        "  padding: 5px 10px;"
        "  min-height: 22px;"
        "  border-radius: 5px;"
        "}"
        "QAbstractItemView::item:hover {"
        "  background-color: %6;"
        "  color: %2;"
        "}"
        "QAbstractItemView::item:selected {"
        "  background-color: %4;"
        "  color: %5;"
        "}")
                                 .arg(t.popup_bg.name(),
                                      t.text_primary.name(),
                                      t.popup_border.name(),
                                      t.accent.name(),
                                      t.accent_text.name(),
                                      t.popup_hover.name());

    const QString container_qss =
        QString("QFrame { background-color: %1; border: 1px solid %2; border-radius: 8px; }")
            .arg(t.popup_bg.name(), t.popup_border.name());

    view->setStyleSheet(view_qss);
    if (QObject *obj = view->findChild<QObject *>(
            QStringLiteral("__shadowbar_opaque_popup_filter"),
            Qt::FindDirectChildrenOnly)) {
        static_cast<detail::OpaquePopupEventFilter *>(obj)->setOpaquePalette(
            pal, container_qss);
    }
}

inline void applyOpaqueMenuTheme(QMenu *menu, const ThemeTokens &t) {
    if (!menu) {
        return;
    }
    menu->setAttribute(Qt::WA_TranslucentBackground, false);
    menu->setAttribute(Qt::WA_NoSystemBackground, false);
    menu->setAutoFillBackground(true);

    QPalette pal = menu->palette();
    pal.setColor(QPalette::Window, t.popup_bg);
    pal.setColor(QPalette::Base, t.popup_bg);
    pal.setColor(QPalette::WindowText, t.text_primary);
    pal.setColor(QPalette::Text, t.text_primary);
    pal.setColor(QPalette::Highlight, t.accent);
    pal.setColor(QPalette::HighlightedText, t.accent_text);
    menu->setPalette(pal);

    menu->setStyleSheet(QString(
        "QMenu {"
        "  background-color: %1;"
        "  color: %2;"
        "  border: 1px solid %3;"
        "  border-radius: 8px;"
        "  padding: 5px;"
        "}"
        "QMenu::item {"
        "  background-color: transparent;"
        "  color: %2;"
        "  padding: 6px 14px;"
        "  border-radius: 5px;"
        "}"
        "QMenu::item:selected {"
        "  background-color: %4;"
        "  color: %5;"
        "}"
        "QMenu::separator {"
        "  height: 1px;"
        "  background-color: %3;"
        "  margin: 4px 8px;"
        "}")
                            .arg(t.popup_bg.name(),
                                 t.text_primary.name(),
                                 t.popup_border.name(),
                                 t.accent.name(),
                                 t.accent_text.name()));
}

inline void selectComboByData(QComboBox *combo, const QString &value) {
    if (!combo) {
        return;
    }
    for (int i = 0; i < combo->count(); ++i) {
        if (combo->itemData(i).toString() == value) {
            combo->setCurrentIndex(i);
            return;
        }
    }
    if (combo->count() > 0) {
        combo->setCurrentIndex(0);
    }
}

inline void populateSourceLangCombo(QComboBox *combo,
                                    bool concise = false,
                                    const QString &auto_label = QStringLiteral("自动")) {
    if (!combo) {
        return;
    }
    combo->clear();
    combo->addItem(concise ? auto_label : QStringLiteral("自动识别 (auto)"),
                   QStringLiteral("auto"));
    for (const auto &lang : shadowbar::supportedLanguages()) {
        const QString label =
            concise
                ? QString::fromUtf8(lang.short_label)
                : QString("%1 (%2)").arg(QString::fromUtf8(lang.zh_name),
                                         QString::fromUtf8(lang.code));
        combo->addItem(label, QString::fromUtf8(lang.code));
    }
}

inline void populateTargetLangCombo(QComboBox *combo,
                                    bool concise = false,
                                    bool include_follow_global = false) {
    if (!combo) {
        return;
    }
    combo->clear();
    if (include_follow_global) {
        combo->addItem(concise ? QStringLiteral("全局")
                               : QStringLiteral("跟随全局默认 (推荐)"),
                       QStringLiteral(""));
    }
    for (const auto &lang : shadowbar::supportedLanguages()) {
        const QString label =
            concise
                ? QString::fromUtf8(lang.zh_name)
                : QString("%1 (%2)").arg(QString::fromUtf8(lang.zh_name),
                                         QString::fromUtf8(lang.code));
        combo->addItem(label, QString::fromUtf8(lang.code));
    }
}

} // namespace shadowbar::floating_ui
