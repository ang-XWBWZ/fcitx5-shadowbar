#include "shadowbar/utf8_util.hpp"

#include <algorithm>

namespace shadowbar::utf8 {

namespace {
bool isContinuationByte(unsigned char b) {
    return (b & 0xC0u) == 0x80u;
}
} // namespace

size_t prevCharOffset(std::string_view text, size_t byte_pos) {
    if (text.empty() || byte_pos == 0) {
        return 0;
    }
    size_t pos = std::min(byte_pos, text.size()) - 1;
    while (pos > 0 && isContinuationByte(static_cast<unsigned char>(text[pos]))) {
        --pos;
    }
    return pos;
}

size_t nextCharOffset(std::string_view text, size_t byte_pos) {
    if (byte_pos >= text.size()) {
        return text.size();
    }
    unsigned char first = static_cast<unsigned char>(text[byte_pos]);
    size_t len = 1;
    if ((first & 0x80u) == 0x00u) {
        len = 1;
    } else if ((first & 0xE0u) == 0xC0u) {
        len = 2;
    } else if ((first & 0xF0u) == 0xE0u) {
        len = 3;
    } else if ((first & 0xF8u) == 0xF0u) {
        len = 4;
    }
    size_t next = std::min(text.size(), byte_pos + len);
    // Guard against malformed continuation sequences.
    while (next < text.size() &&
           isContinuationByte(static_cast<unsigned char>(text[next]))) {
        ++next;
    }
    return next;
}

uint32_t decodeAt(std::string_view text, size_t byte_pos, size_t *next_pos) {
    if (byte_pos >= text.size()) {
        if (next_pos) {
            *next_pos = text.size();
        }
        return 0;
    }
    const size_t next = nextCharOffset(text, byte_pos);
    if (next_pos) {
        *next_pos = next;
    }
    const unsigned char b0 = static_cast<unsigned char>(text[byte_pos]);
    const size_t width = next - byte_pos;
    if (width == 1 && (b0 & 0x80u) == 0) {
        return b0;
    }
    if (width == 2) {
        return ((b0 & 0x1Fu) << 6) |
               (static_cast<unsigned char>(text[byte_pos + 1]) & 0x3Fu);
    }
    if (width == 3) {
        return ((b0 & 0x0Fu) << 12) |
               ((static_cast<unsigned char>(text[byte_pos + 1]) & 0x3Fu) << 6) |
               (static_cast<unsigned char>(text[byte_pos + 2]) & 0x3Fu);
    }
    if (width == 4) {
        return ((b0 & 0x07u) << 18) |
               ((static_cast<unsigned char>(text[byte_pos + 1]) & 0x3Fu) << 12) |
               ((static_cast<unsigned char>(text[byte_pos + 2]) & 0x3Fu) << 6) |
               (static_cast<unsigned char>(text[byte_pos + 3]) & 0x3Fu);
    }
    return 0xFFFDu;
}

std::string_view trimView(std::string_view s) {
    auto is_ws = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (!s.empty() && is_ws(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && is_ws(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

std::string trim(std::string_view s) {
    return std::string(trimView(s));
}

} // namespace shadowbar::utf8
