#pragma once

#include "shadowbar/types.hpp"

#include <QComboBox>
#include <QString>

namespace shadowbar::floating_ui {

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
