#include "hotkey_util.hpp"

#include <QKeyCombination>
#include <QStringList>

namespace shadowbar::floating_ui {
namespace {

QString normalizeSingleFcitxBinding(const QString &binding) {
    QStringList parts = binding.split('+', Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return {};
    }
    for (int i = 0; i < parts.size(); ++i) {
        QString &part = parts[i];
        const bool is_last = (i + 1 == parts.size());
        if (!is_last) {
            if (part.compare("Control", Qt::CaseInsensitive) == 0 ||
                part.compare("Ctrl", Qt::CaseInsensitive) == 0) {
                part = "Ctrl";
            } else if (part.compare("Super", Qt::CaseInsensitive) == 0) {
                part = "Meta";
            } else if (part.compare("Alt", Qt::CaseInsensitive) == 0) {
                part = "Alt";
            } else if (part.compare("Shift", Qt::CaseInsensitive) == 0) {
                part = "Shift";
            }
        } else {
            if (part.compare("BackSpace", Qt::CaseInsensitive) == 0) {
                part = "Backspace";
            } else if (part.compare("Escape", Qt::CaseInsensitive) == 0) {
                part = "Esc";
            } else if (part.compare("Prior", Qt::CaseInsensitive) == 0) {
                part = "PgUp";
            } else if (part.compare("Next", Qt::CaseInsensitive) == 0) {
                part = "PgDown";
            } else if (part.size() == 1 && part[0].isLower()) {
                part = part.toUpper();
            }
        }
    }
    return parts.join('+');
}

QStringList splitFcitxKeyList(std::string_view fcitx_key) {
    const QString raw = QString::fromUtf8(fcitx_key.data(),
                                          static_cast<qsizetype>(fcitx_key.size()))
                            .trimmed();
    if (raw.isEmpty()) {
        return {};
    }
    return raw.split(' ', Qt::SkipEmptyParts);
}

} // namespace

QString fcitxToQtKeySequenceString(std::string_view fcitx_key) {
    const QStringList tokens = splitFcitxKeyList(fcitx_key);
    if (tokens.isEmpty()) {
        return {};
    }
    return normalizeSingleFcitxBinding(tokens.value(0));
}

QList<QKeySequence> fcitxToQtKeySequences(std::string_view fcitx_key) {
    QList<QKeySequence> sequences;
    for (const QString &token : splitFcitxKeyList(fcitx_key)) {
        const QString normalized = normalizeSingleFcitxBinding(token);
        if (normalized.isEmpty()) {
            continue;
        }
        const QKeySequence seq =
            QKeySequence::fromString(normalized, QKeySequence::PortableText);
        if (!seq.isEmpty() && !sequences.contains(seq)) {
            sequences.append(seq);
        }
    }
    return sequences;
}

std::string qtKeySequenceToFcitx(const QKeySequence &seq) {
    if (seq.isEmpty()) {
        return {};
    }
    const QString text = seq.toString(QKeySequence::PortableText).trimmed();
    if (text.isEmpty()) {
        return {};
    }
    const QString first = text.split(',', Qt::SkipEmptyParts).value(0).trimmed();
    QStringList parts = first.split('+', Qt::SkipEmptyParts);
    for (int i = 0; i < parts.size(); ++i) {
        QString &part = parts[i];
        const bool is_last = (i + 1 == parts.size());
        if (!is_last && part.compare("Ctrl", Qt::CaseInsensitive) == 0) {
            part = "Control";
        } else if (!is_last && part.compare("Meta", Qt::CaseInsensitive) == 0) {
            part = "Super";
        } else if (is_last && part.size() == 1 && part[0].isLower()) {
            part = part.toUpper();
        }
    }
    return parts.join('+').toStdString();
}

} // namespace shadowbar::floating_ui
