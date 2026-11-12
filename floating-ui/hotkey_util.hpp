#pragma once

#include <QKeySequence>
#include <QList>
#include <QString>

#include <string>
#include <string_view>

namespace shadowbar::floating_ui {

/** Convert a Fcitx5 KeyList string (e.g. "Control+Shift+T") to Qt QKeySequence string. */
QString fcitxToQtKeySequenceString(std::string_view fcitx_key);

/** Parse all space-separated bindings in a Fcitx5 KeyList into Qt QKeySequences. */
QList<QKeySequence> fcitxToQtKeySequences(std::string_view fcitx_key);

/** Convert a Qt QKeySequence into Fcitx5 KeyList syntax (e.g. "Control+Shift+T"). */
std::string qtKeySequenceToFcitx(const QKeySequence &seq);

} // namespace shadowbar::floating_ui
