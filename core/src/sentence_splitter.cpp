#include "shadowbar/sentence_splitter.hpp"
#include "shadowbar/utf8_util.hpp"

#include <algorithm>
#include <cctype>

namespace shadowbar {

namespace {

bool isAsciiAlphaNum(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
}

bool isSentenceTerminatorCodepoint(uint32_t cp,
                                     std::string_view text,
                                     size_t byte_pos,
                                     size_t next_pos) {
    switch (cp) {
    case 0x3002: // 。
    case 0xFF01: // ！
    case 0xFF1F: // ？
    case 0xFF1B: // ；
    case '\n':
    case '!':
    case '?':
    case ';':
        return true;
    case '.': {
        // Avoid splitting numbers/versions/identifiers such as 3.14, v1.2, api.dev.
        bool prev_alnum = byte_pos > 0 && isAsciiAlphaNum(text[byte_pos - 1]);
        bool next_alnum = next_pos < text.size() && isAsciiAlphaNum(text[next_pos]);
        return !(prev_alnum && next_alnum);
    }
    default:
        return false;
    }
}

bool isClauseDelimiterCodepoint(uint32_t cp) {
    switch (cp) {
    case ',':
    case 0xFF0C: // ，
    case 0x3001: // 、
    case ':':
    case 0xFF1A: // ：
    case 0x2014: // —
    case 0x2026: // …
    case 0x00B7: // ·
    case ')':
    case 0xFF09: // ）
    case 0x300D: // 」
    case 0x300F: // 』
    case 0x3011: // 】
    case ']':
    case 0x300B: // 》
        return true;
    default:
        return false;
    }
}

bool isClosingAttachmentCodepoint(uint32_t cp) {
    switch (cp) {
    case '"':
    case '\'':
    case ')':
    case ']':
    case 0x201D: // ”
    case 0x2019: // ’
    case 0xFF09: // ）
    case 0x300D: // 」
    case 0x300F: // 』
    case 0x3011: // 】
    case 0x300B: // 》
        return true;
    default:
        return false;
    }
}

bool isLeadingSkipWhitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool isCjkCharacter(uint32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK Unified Ideographs
           (cp >= 0x3400 && cp <= 0x4DBF) ||   // CJK Ext A
           (cp >= 0x20000 && cp <= 0x323AF) || // CJK Ext B..G
           (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK Compatibility Ideographs
           (cp >= 0x3040 && cp <= 0x30FF) ||   // Hiragana / Katakana
           (cp >= 0xAC00 && cp <= 0xD7AF);     // Hangul Syllables
}

bool isWordBaseCodepoint(uint32_t cp) {
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
        (cp >= '0' && cp <= '9') || cp == '_') {
        return true;
    }
    if (cp >= 0x00C0 && cp <= 0x024F && cp != 0x00D7 && cp != 0x00F7) {
        return true;
    }
    if ((cp >= 0x0370 && cp <= 0x03FF) || // Greek
        (cp >= 0x0400 && cp <= 0x04FF) || // Cyrillic
        (cp >= 0x0600 && cp <= 0x06FF)) { // Arabic
        return true;
    }
    return false;
}

bool isWordJoinerCodepoint(uint32_t cp) {
    return cp == '\'' || cp == 0x2019 || cp == '-' || cp == '.' || cp == '+';
}

/// Scans a whole non-CJK word token starting at `start` and returns its end byte offset.
size_t scanWordEnd(std::string_view text, size_t start, size_t max_word_bytes = 4096) {
    size_t pos = start;
    const size_t hard_cap = std::min(text.size(), start + max_word_bytes);
    while (pos < hard_cap) {
        size_t next = pos;
        uint32_t cp = utf8::decodeAt(text, pos, &next);
        if (isWordBaseCodepoint(cp)) {
            pos = next;
            continue;
        }
        if (isWordJoinerCodepoint(cp) && next < hard_cap) {
            size_t look_next = next;
            uint32_t look_cp = utf8::decodeAt(text, next, &look_next);
            if (isWordBaseCodepoint(look_cp) ||
                (cp == '+' && look_cp == '+')) {
                pos = next;
                continue;
            }
        }
        break;
    }
    return pos;
}

} // namespace

ClauseSplitResult splitClauses(std::string_view text) {
    ClauseSplitResult res;
    size_t clause_start = 0;
    size_t i = 0;

    while (i < text.size()) {
        size_t next = i;
        uint32_t cp = utf8::decodeAt(text, i, &next);
        if (isSentenceTerminatorCodepoint(cp, text, i, next)) {
            // Consume consecutive terminators / spaces immediately attached to the boundary.
            size_t end_pos = next;
            while (end_pos < text.size()) {
                size_t look_next = end_pos;
                uint32_t look_cp = utf8::decodeAt(text, end_pos, &look_next);
                if (look_cp == ' ' || look_cp == '\t' ||
                    isSentenceTerminatorCodepoint(look_cp, text, end_pos, look_next)) {
                    end_pos = look_next;
                } else {
                    break;
                }
            }
            std::string clause = utf8::trim(text.substr(clause_start, end_pos - clause_start));
            if (!clause.empty()) {
                res.locked_clauses.push_back(std::move(clause));
            }
            clause_start = end_pos;
            i = end_pos;
        } else {
            i = next;
        }
    }

    if (clause_start < text.size()) {
        res.active_tail = std::string(text.substr(clause_start));
    }
    return res;
}

bool endsWithSentenceTerminator(std::string_view text) {
    std::string_view trimmed = utf8::trimView(text);
    if (trimmed.empty()) {
        return false;
    }
    size_t last_start = utf8::prevCharOffset(trimmed, trimmed.size());
    size_t next = last_start;
    uint32_t cp = utf8::decodeAt(trimmed, last_start, &next);
    return isSentenceTerminatorCodepoint(cp, trimmed, last_start, next);
}

TranslationUnitStats countTranslationUnits(std::string_view text) {
    TranslationUnitStats stats;
    size_t i = 0;
    while (i < text.size()) {
        size_t next = i;
        uint32_t cp = utf8::decodeAt(text, i, &next);
        if (isCjkCharacter(cp)) {
            ++stats.cjk_chars;
            ++stats.total_units;
            i = next;
        } else if (isWordBaseCodepoint(cp)) {
            ++stats.word_count;
            ++stats.total_units;
            i = scanWordEnd(text, i);
        } else {
            i = next;
        }
    }
    return stats;
}

std::vector<std::string> splitTextIntoScrollChunks(std::string_view text,
                                                   size_t max_units) {
    std::vector<std::string> chunks;
    std::string_view trimmed = utf8::trimView(text);
    if (trimmed.empty()) {
        return chunks;
    }
    max_units = std::max<size_t>(1, max_units);

    size_t pos = 0;
    while (pos < trimmed.size()) {
        while (pos < trimmed.size() && isLeadingSkipWhitespace(trimmed[pos])) {
            ++pos;
        }
        if (pos >= trimmed.size()) {
            break;
        }

        size_t i = pos;
        size_t units = 0;
        size_t last_strong_end = 0;
        size_t last_strong_units = 0;
        size_t last_weak_end = 0;
        size_t last_weak_units = 0;
        size_t last_unit_end = 0;

        auto attach_trailing_punct = [&](size_t from) {
            size_t end_pos = from;
            while (end_pos < trimmed.size()) {
                size_t look_next = end_pos;
                uint32_t look_cp = utf8::decodeAt(trimmed, end_pos, &look_next);
                if (isClosingAttachmentCodepoint(look_cp) ||
                    isSentenceTerminatorCodepoint(look_cp, trimmed, end_pos,
                                                  look_next)) {
                    end_pos = look_next;
                } else {
                    break;
                }
            }
            return end_pos;
        };

        while (i < trimmed.size() && units < max_units) {
            size_t next = i;
            uint32_t cp = utf8::decodeAt(trimmed, i, &next);

            if (isCjkCharacter(cp)) {
                ++units;
                last_unit_end = next;
                i = next;
            } else if (isWordBaseCodepoint(cp)) {
                size_t word_end = scanWordEnd(trimmed, i);
                ++units;
                last_unit_end = word_end;
                i = word_end;
            } else {
                if (isSentenceTerminatorCodepoint(cp, trimmed, i, next)) {
                    size_t end_pos = attach_trailing_punct(next);
                    last_strong_end = end_pos;
                    last_strong_units = units;
                    i = next;
                    continue;
                }
                if (isClauseDelimiterCodepoint(cp)) {
                    last_weak_end = next;
                    last_weak_units = units;
                }
                i = next;
            }
        }

        // Attach immediately following sentence/clause terminator to the final unit at limit.
        while (i < trimmed.size()) {
            size_t look_next = i;
            uint32_t look_cp = utf8::decodeAt(trimmed, i, &look_next);
            if (isSentenceTerminatorCodepoint(look_cp, trimmed, i, look_next)) {
                size_t end_pos = attach_trailing_punct(look_next);
                last_strong_end = end_pos;
                last_strong_units = units;
                i = end_pos;
                break;
            }
            if (isClauseDelimiterCodepoint(look_cp) ||
                isClosingAttachmentCodepoint(look_cp)) {
                last_weak_end = look_next;
                last_weak_units = units;
                i = look_next;
                continue;
            }
            break;
        }

        const size_t limit_end = i;
        if (limit_end >= trimmed.size()) {
            std::string tail = utf8::trim(trimmed.substr(pos));
            if (!tail.empty()) {
                chunks.push_back(std::move(tail));
            }
            break;
        }

        size_t cut_end = (last_unit_end > pos) ? last_unit_end : limit_end;
        if (last_strong_end > pos && last_strong_units * 2 >= max_units) {
            cut_end = last_strong_end;
        } else if (last_weak_end > pos && last_weak_units * 2 >= max_units) {
            cut_end = last_weak_end;
        } else if (last_strong_end > pos) {
            cut_end = last_strong_end;
        } else if (last_weak_end > pos) {
            cut_end = last_weak_end;
        } else {
            cut_end = limit_end;
        }
        if (cut_end <= pos) {
            cut_end = limit_end;
        }

        std::string piece = utf8::trim(trimmed.substr(pos, cut_end - pos));
        if (!piece.empty()) {
            chunks.push_back(std::move(piece));
        }
        pos = cut_end;
    }

    return chunks;
}

} // namespace shadowbar
