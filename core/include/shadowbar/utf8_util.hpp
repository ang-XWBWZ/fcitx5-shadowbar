#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace shadowbar::utf8 {

/// Returns byte index of the previous UTF-8 codepoint before `byte_pos`.
size_t prevCharOffset(std::string_view text, size_t byte_pos);

/// Returns byte index of the next UTF-8 codepoint after `byte_pos`.
size_t nextCharOffset(std::string_view text, size_t byte_pos);

/// Decodes one UTF-8 codepoint starting at `byte_pos` and advances `next_pos`.
uint32_t decodeAt(std::string_view text, size_t byte_pos, size_t *next_pos = nullptr);

/// Returns trimmed view (ASCII whitespace).
std::string_view trimView(std::string_view s);
std::string trim(std::string_view s);

} // namespace shadowbar::utf8
