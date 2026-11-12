#include "shadowbar/cache.hpp"
#include "offline_dict_data.hpp"
#include "shadowbar/session.hpp"
#include "shadowbar/utf8_util.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

#ifdef SHADOWBAR_HAS_SQLITE3
#include <sqlite3.h>
#endif

#ifdef SHADOWBAR_HAS_ZLIB
#include <zlib.h>
#endif

namespace shadowbar {

LruTranslationCache::LruTranslationCache(size_t capacity)
    : capacity_(std::max<size_t>(1, capacity)) {}

void LruTranslationCache::setCapacity(size_t capacity) {
    std::lock_guard<std::mutex> lock(mu_);
    capacity_ = std::max<size_t>(1, capacity);
    while (items_.size() > capacity_) {
        auto last = std::prev(items_.end());
        index_.erase(last->first);
        items_.pop_back();
    }
}

std::string LruTranslationCache::makeKey(std::string_view preset_id,
                                         std::string_view target_lang,
                                         std::string_view source_text) {
    std::string key;
    key.reserve(preset_id.size() + target_lang.size() + source_text.size() + 2);
    key.append(preset_id);
    key.push_back('\x1f');
    key.append(target_lang);
    key.push_back('\x1f');
    key.append(source_text);
    return key;
}

std::optional<std::string> LruTranslationCache::get(std::string_view preset_id,
                                                    std::string_view target_lang,
                                                    std::string_view source_text) {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string key = makeKey(preset_id, target_lang, source_text);
    auto it = index_.find(key);
    if (it == index_.end()) {
        return std::nullopt;
    }
    items_.splice(items_.begin(), items_, it->second);
    return it->second->second;
}

void LruTranslationCache::put(std::string_view preset_id,
                              std::string_view target_lang,
                              std::string_view source_text,
                              std::string translated_text) {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string key = makeKey(preset_id, target_lang, source_text);
    auto it = index_.find(key);
    if (it != index_.end()) {
        it->second->second = std::move(translated_text);
        items_.splice(items_.begin(), items_, it->second);
        return;
    }
    items_.emplace_front(key, std::move(translated_text));
    index_[key] = items_.begin();
    if (items_.size() > capacity_) {
        auto last = std::prev(items_.end());
        index_.erase(last->first);
        items_.pop_back();
    }
}

void LruTranslationCache::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    items_.clear();
    index_.clear();
}

size_t LruTranslationCache::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return items_.size();
}

namespace {

using shadowbar::offline_detail::builtinDictIndex;

std::string toLowerAscii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

bool isAsciiSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

bool isAsciiAlnum(unsigned char c) {
    return std::isalnum(c) != 0;
}

bool isCjkCodepoint(uint32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0x20000 && cp <= 0x2CEAF);
}

bool containsCjk(std::string_view text) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t next = pos;
        uint32_t cp = shadowbar::utf8::decodeAt(text, pos, &next);
        if (isCjkCodepoint(cp)) {
            return true;
        }
        pos = (next > pos) ? next : pos + 1;
    }
    return false;
}

bool containsAsciiAlpha(std::string_view text) {
    for (unsigned char c : text) {
        if (std::isalpha(c) != 0) {
            return true;
        }
    }
    return false;
}

std::string expandUserPath(std::string_view raw) {
    std::string path = shadowbar::utf8::trim(raw);
    if (path.empty()) {
        return path;
    }
    if (path[0] == '~' && (path.size() == 1 || path[1] == '/')) {
        const char *home = std::getenv("HOME");
        if (home && *home) {
            return std::string(home) + path.substr(1);
        }
    }
    return path;
}

std::string defaultManagedDictDirPath() {
    if (const char *xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        return std::string(xdg) + "/shadowbar/dicts";
    }
    if (const char *home = std::getenv("HOME"); home && *home) {
        return std::string(home) + "/.local/share/shadowbar/dicts";
    }
    return "dicts";
}

bool hasExtension(std::string_view path, std::initializer_list<std::string_view> exts) {
    std::string lower = toLowerAscii(path);
    for (std::string_view ext : exts) {
        if (lower.size() >= ext.size() &&
            lower.compare(lower.size() - ext.size(), ext.size(), ext) == 0) {
            return true;
        }
    }
    return false;
}

bool isSqliteExtension(std::string_view path) {
    return hasExtension(path, {".db", ".sqlite", ".sqlite3"});
}

bool isSupportedDictFile(std::string_view path) {
    return hasExtension(path, {".tsv", ".txt", ".csv", ".u8", ".json", ".jsonl",
                               ".ifo", ".db", ".sqlite", ".sqlite3",
                               ".tsv.gz", ".txt.gz", ".csv.gz", ".u8.gz",
                               ".jsonl.gz"});
}

std::vector<std::string> defaultSearchPaths() {
    std::vector<std::string> paths;
    const char *home = std::getenv("HOME");
    if (home && *home) {
        const std::string h(home);
        paths.push_back(h + "/.local/share/shadowbar/ecdict.db");
        paths.push_back(h + "/.local/share/shadowbar/ecdict.sqlite");
        paths.push_back(h + "/.local/share/shadowbar/ecdict.sqlite3");
        paths.push_back(h + "/.local/share/shadowbar/ecdict.csv");
        paths.push_back(h + "/.local/share/shadowbar/cedict_ts.u8");
        paths.push_back(h + "/.local/share/shadowbar/dict.tsv");
        paths.push_back(h + "/.config/shadowbar/dict.tsv");
        paths.push_back(h + "/.config/shadowbar/dict.txt");
    }

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path managed(defaultManagedDictDirPath());
    if (fs::exists(managed, ec) && fs::is_directory(managed, ec)) {
        std::vector<std::string> managed_files;
        for (const auto &entry : fs::directory_iterator(managed, ec)) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            const std::string p = entry.path().string();
            if (isSupportedDictFile(p)) {
                managed_files.push_back(p);
            }
        }
        std::sort(managed_files.begin(), managed_files.end());
        for (auto &p : managed_files) {
            if (std::find(paths.begin(), paths.end(), p) == paths.end()) {
                paths.push_back(std::move(p));
            }
        }
    }
    return paths;
}

size_t countUtf8Codepoints(std::string_view s) {
    size_t count = 0;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t next = shadowbar::utf8::nextCharOffset(s, pos);
        pos = (next > pos) ? next : pos + 1;
        ++count;
    }
    return count;
}

size_t countAsciiWords(std::string_view s) {
    size_t count = 0;
    bool in_word = false;
    for (unsigned char c : s) {
        if (!isAsciiSpace(static_cast<char>(c))) {
            if (!in_word) {
                ++count;
                in_word = true;
            }
        } else {
            in_word = false;
        }
    }
    return count;
}

std::string normalizeAsciiSpaces(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool prev_space = true;
    for (char c : s) {
        if (isAsciiSpace(c)) {
            if (!prev_space) {
                out.push_back(' ');
                prev_space = true;
            }
        } else {
            out.push_back(c);
            prev_space = false;
        }
    }
    if (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

void stripUtf8Bom(std::string &s) {
    if (s.size() >= 3 &&
        static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
}

std::string unescapeNewlines(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            if (raw[i + 1] == 'n') {
                out.push_back('\n');
                ++i;
                continue;
            }
            if (raw[i + 1] == 'r') {
                ++i;
                continue;
            }
            if (raw[i + 1] == 't') {
                out.push_back(' ');
                ++i;
                continue;
            }
        }
        if (raw[i] != '\r') {
            out.push_back(raw[i]);
        }
    }
    return out;
}

std::string stripLeadingPosAndTags(std::string_view s) {
    std::string cur = shadowbar::utf8::trim(s);
    while (!cur.empty()) {
        if (cur[0] == '[') {
            size_t close = cur.find(']');
            if (close != std::string::npos && close < 24) {
                cur = shadowbar::utf8::trim(std::string_view(cur).substr(close + 1));
                continue;
            }
        }
        if (cur[0] == '(') {
            size_t close = cur.find(')');
            if (close != std::string::npos && close < 32) {
                cur = shadowbar::utf8::trim(std::string_view(cur).substr(close + 1));
                continue;
            }
        }
        // Strip short English POS abbreviations like "n.", "vt.", "vi.", "adj.", "adv."
        size_t dot = cur.find('.');
        if (dot > 0 && dot <= 6) {
            bool all_alpha = true;
            for (size_t i = 0; i < dot; ++i) {
                unsigned char c = static_cast<unsigned char>(cur[i]);
                if (!std::isalpha(c) && c != '&' && c != '/') {
                    all_alpha = false;
                    break;
                }
            }
            if (all_alpha) {
                cur = shadowbar::utf8::trim(std::string_view(cur).substr(dot + 1));
                continue;
            }
        }
        break;
    }
    return cur;
}

bool startsWithNoCase(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

bool containsNoCase(std::string_view text, std::string_view needle) {
    if (needle.empty() || text.size() < needle.size()) {
        return false;
    }
    for (size_t i = 0; i + needle.size() <= text.size(); ++i) {
        if (startsWithNoCase(text.substr(i), needle)) {
            return true;
        }
    }
    return false;
}

bool isNoisyGlossFragment(std::string_view cand) {
    cand = shadowbar::utf8::trimView(cand);
    if (cand.empty()) {
        return true;
    }
    static constexpr std::array<std::string_view, 15> kBadPrefixes = {
        "cl:", "variant of ", "old variant of ", "erhua variant of ",
        "archaic variant of ", "same as ", "used in ", "see ",
        "see also ", "also pr.", "taiwan pr.", "also written ",
        "abbr. for ", "abbr. to ", "surname ",
    };
    for (std::string_view pfx : kBadPrefixes) {
        if (startsWithNoCase(cand, pfx)) {
            return true;
        }
    }
    if (cand.find('|') != std::string_view::npos ||
        cand.find(']') != std::string_view::npos ||
        startsWithNoCase(cand, "family name")) {
        return true;
    }
    return false;
}

bool shouldStripTrailingParen(std::string_view inner) {
    if (inner.empty()) {
        return true;
    }
    // Strip ECDICT inflection/classifier suffixes and CC-CEDICT usage remarks
    // while keeping genuine parenthetical meanings short or descriptive if needed.
    if (containsAsciiAlpha(inner) ||
        inner.find("的") != std::string_view::npos ||
        inner.find("量") != std::string_view::npos ||
        inner.find("名") != std::string_view::npos ||
        inner.find("复数") != std::string_view::npos ||
        inner.find("人名") != std::string_view::npos) {
        return true;
    }
    return false;
}

std::string stripTrailingParenAnnotations(std::string_view s) {
    std::string cur = shadowbar::utf8::trim(s);
    while (!cur.empty()) {
        if (cur.back() == ')') {
            size_t open = cur.rfind('(');
            if (open != std::string::npos && open > 0 &&
                shouldStripTrailingParen(std::string_view(cur).substr(open + 1, cur.size() - open - 2))) {
                cur = shadowbar::utf8::trim(std::string_view(cur).substr(0, open));
                continue;
            }
        }
        // Check full-width Chinese closing parenthesis "）" (EF BC 89)
        if (cur.size() >= 3 && cur.compare(cur.size() - 3, 3, "）") == 0) {
            size_t open = cur.rfind("（");
            if (open != std::string::npos && open > 0 &&
                shouldStripTrailingParen(std::string_view(cur).substr(open + 3, cur.size() - open - 6))) {
                cur = shadowbar::utf8::trim(std::string_view(cur).substr(0, open));
                continue;
            }
        }
        break;
    }
    return cur;
}

template <typename Fn>
void forEachSplitAtDepthZero(
    std::string_view text,
    std::initializer_list<std::string_view> delims,
    Fn &&cb) {
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < text.size();) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= 0x80) {
            if (text.compare(i, 3, "（") == 0 || text.compare(i, 3, "【") == 0) {
                ++depth;
                i += 3;
                continue;
            }
            if (text.compare(i, 3, "）") == 0 || text.compare(i, 3, "】") == 0) {
                depth = std::max(0, depth - 1);
                i += 3;
                continue;
            }
        } else {
            if (c == '(' || c == '[' || c == '{') {
                ++depth;
                ++i;
                continue;
            }
            if (c == ')' || c == ']' || c == '}') {
                depth = std::max(0, depth - 1);
                ++i;
                continue;
            }
        }
        if (depth == 0) {
            size_t matched_len = 0;
            for (std::string_view d : delims) {
                if (text.size() - i >= d.size() && text.compare(i, d.size(), d) == 0) {
                    matched_len = d.size();
                    break;
                }
            }
            if (matched_len > 0) {
                cb(text.substr(start, i - start));
                i += matched_len;
                start = i;
                continue;
            }
        }
        if (c < 0x80) {
            ++i;
        } else {
            size_t next = shadowbar::utf8::nextCharOffset(text, i);
            i = (next > i) ? next : i + 1;
        }
    }
    cb(text.substr(start));
}

std::vector<std::string_view> splitAtDepthZero(
    std::string_view text,
    std::initializer_list<std::string_view> delims) {
    std::vector<std::string_view> parts;
    forEachSplitAtDepthZero(text, delims, [&](std::string_view part) {
        parts.push_back(part);
    });
    return parts;
}

bool hasTemplatePlaceholder(std::string_view s) {
    return s.find("…") != std::string_view::npos ||
           s.find("...") != std::string_view::npos ||
           containsNoCase(s, "sb.") ||
           containsNoCase(s, "sth.");
}

std::string stripHtmlMarkup(std::string_view raw) {
    if (raw.find('<') == std::string_view::npos &&
        raw.find('&') == std::string_view::npos) {
        return std::string(raw);
    }
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '<' && i + 1 < raw.size()) {
            const unsigned char nxt = static_cast<unsigned char>(raw[i + 1]);
            if (std::isalpha(nxt) || nxt == '/' || nxt == '!' || nxt == '?') {
                size_t close = raw.find('>', i + 1);
                if (close != std::string_view::npos && close - i <= 160) {
                    std::string tag = toLowerAscii(raw.substr(i + 1, close - i - 1));
                    if (tag.rfind("br", 0) == 0 || tag == "/p" || tag == "/div" ||
                        tag == "/li" || tag.rfind("hr", 0) == 0) {
                        if (!out.empty() && out.back() != '\n') {
                            out.push_back('\n');
                        }
                    }
                    i = close;
                    continue;
                }
            }
        }
        if (raw[i] == '&') {
            std::string_view rem = raw.substr(i);
            if (rem.rfind("&amp;", 0) == 0) {
                out.push_back('&');
                i += 4;
                continue;
            }
            if (rem.rfind("&lt;", 0) == 0) {
                out.push_back('<');
                i += 3;
                continue;
            }
            if (rem.rfind("&gt;", 0) == 0) {
                out.push_back('>');
                i += 3;
                continue;
            }
            if (rem.rfind("&quot;", 0) == 0) {
                out.push_back('"');
                i += 5;
                continue;
            }
            if (rem.rfind("&apos;", 0) == 0 || rem.rfind("&#39;", 0) == 0) {
                out.push_back('\'');
                i += (rem.rfind("&#39;", 0) == 0) ? 4 : 5;
                continue;
            }
            if (rem.rfind("&nbsp;", 0) == 0) {
                out.push_back(' ');
                i += 5;
                continue;
            }
        }
        out.push_back(raw[i]);
    }
    return out;
}

std::string cleanExternalGloss(std::string_view raw, bool sentence_mode) {
    std::string html_clean = stripHtmlMarkup(raw);
    std::string text = unescapeNewlines(shadowbar::utf8::trimView(html_clean));
    if (text.empty()) {
        return {};
    }

    if (sentence_mode) {
        std::string best_cand;
        std::string fallback_cand;
        std::string first_cleaned_line;
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t line_end = text.find('\n', pos);
            std::string_view raw_line =
                (line_end == std::string_view::npos)
                    ? std::string_view(text).substr(pos)
                    : std::string_view(text).substr(pos, line_end - pos);
            pos = (line_end == std::string_view::npos) ? text.size() + 1 : line_end + 1;
            raw_line = shadowbar::utf8::trimView(raw_line);
            if (raw_line.empty()) {
                continue;
            }
            std::string cleaned_line = stripLeadingPosAndTags(raw_line);
            if (first_cleaned_line.empty()) {
                first_cleaned_line = cleaned_line;
            }
            forEachSplitAtDepthZero(
                cleaned_line, {"；", "，", ";", ",", "/"}, [&](std::string_view sub) {
                    if (!best_cand.empty()) {
                        return;
                    }
                    std::string cand =
                        stripTrailingParenAnnotations(stripLeadingPosAndTags(sub));
                    if (isNoisyGlossFragment(cand)) {
                        return;
                    }
                    if (!hasTemplatePlaceholder(cand)) {
                        best_cand = std::move(cand);
                        return;
                    }
                    if (fallback_cand.empty()) {
                        fallback_cand = std::move(cand);
                    }
                });
            if (!best_cand.empty()) {
                return best_cand;
            }
        }
        if (!fallback_cand.empty()) {
            return fallback_cand;
        }
        return stripTrailingParenAnnotations(first_cleaned_line);
    }

    // Standalone lookup mode: gather up to 3 clean lines/senses joined nicely.
    std::vector<std::string> senses;
    std::istringstream iss(text);
    std::string line;
    bool has_cjk = containsCjk(text);
    while (std::getline(iss, line) && senses.size() < 3) {
        std::string trimmed = shadowbar::utf8::trim(line);
        if (isNoisyGlossFragment(trimmed)) {
            continue;
        }
        if (trimmed[0] == '[' && trimmed.find("人名") != std::string::npos) {
            continue;
        }
        senses.push_back(trimmed);
    }
    if (senses.empty()) {
        return shadowbar::utf8::trim(text);
    }
    std::string joined;
    const std::string sep = has_cjk ? "；" : "; ";
    for (size_t i = 0; i < senses.size(); ++i) {
        if (i > 0) {
            joined += sep;
        }
        joined += senses[i];
    }
    return joined;
}

std::string extractBestCedictGloss(std::string_view pinyin,
                                   std::string_view defs,
                                   int &out_score) {
    out_score = -10000;
    std::string best_gloss;
    const bool proper_pinyin =
        !pinyin.empty() && std::isupper(static_cast<unsigned char>(pinyin[0])) != 0;

    size_t slash_idx = 0;
    forEachSplitAtDepthZero(defs, {"/"}, [&](std::string_view slash_sv) {
        const size_t s_idx = slash_idx++;
        std::string_view raw_def = shadowbar::utf8::trimView(slash_sv);
        if (raw_def.empty() || isNoisyGlossFragment(raw_def)) {
            return;
        }
        const bool tagged_rare =
            containsNoCase(raw_def, "(bound form)") ||
            containsNoCase(raw_def, "(literary)") ||
            containsNoCase(raw_def, "(archaic)") ||
            containsNoCase(raw_def, "(dialect)") ||
            containsNoCase(raw_def, "(coll.)") ||
            containsNoCase(raw_def, "(slang)") ||
            containsNoCase(raw_def, "(tw)");

        size_t sub_idx = 0;
        forEachSplitAtDepthZero(raw_def, {";", ","}, [&](std::string_view sub_sv) {
            const size_t cur_sub = sub_idx++;
            std::string cand =
                stripTrailingParenAnnotations(stripLeadingPosAndTags(sub_sv));
            if (isNoisyGlossFragment(cand)) {
                return;
            }
            int score = 100;
            if (proper_pinyin) {
                score -= 220;
            }
            if (tagged_rare) {
                score -= 45;
            }
            if (startsWithNoCase(cand, "o'clock") ||
                startsWithNoCase(cand, "one of the ") ||
                containsNoCase(cand, "give equal emphasis")) {
                score -= 40;
            }
            if (hasTemplatePlaceholder(cand)) {
                score -= 30;
            }
            const size_t words = countAsciiWords(cand);
            if (words >= 1 && words <= 3) {
                score += 20;
            } else if (words > 4) {
                score -= static_cast<int>((words - 4) * 5);
            }
            score -= static_cast<int>(s_idx * 2 + cur_sub);
            if (score > out_score) {
                out_score = score;
                best_gloss = std::move(cand);
            }
        });
    });
    return best_gloss;
}

std::vector<std::string> parseCsvRow(std::string_view line) {
    std::vector<std::string> fields;
    std::string cur;
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    cur.push_back('"');
                    ++i;
                } else {
                    in_quotes = false;
                }
            } else {
                cur.push_back(c);
            }
        } else {
            if (c == '"') {
                in_quotes = true;
            } else if (c == ',') {
                fields.push_back(std::move(cur));
                cur.clear();
            } else if (c != '\r') {
                cur.push_back(c);
            }
        }
    }
    fields.push_back(std::move(cur));
    return fields;
}

#ifdef SHADOWBAR_HAS_SQLITE3
std::string quoteSqliteIdent(std::string_view ident) {
    std::string out = "\"";
    for (char c : ident) {
        if (c == '"') {
            out += "\"\"";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}
#endif

enum class LookupDirection {
    None,
    ZhToEn,
    EnToZh,
};

LookupDirection resolveDirection(std::string_view source_text,
                                 std::string_view target_lang) {
    std::string tl = toLowerAscii(shadowbar::utf8::trimView(target_lang));
    const bool has_cjk = containsCjk(source_text);
    const bool has_alpha = containsAsciiAlpha(source_text);

    const bool target_auto = tl.empty() || tl == "auto";
    const bool target_en = tl.rfind("en", 0) == 0 || tl.find("english") != std::string::npos;
    const bool target_zh = tl.rfind("zh", 0) == 0 ||
                           tl.find("chinese") != std::string::npos ||
                           target_lang.find("中文") != std::string_view::npos;

    if (target_auto) {
        if (has_cjk) {
            return LookupDirection::ZhToEn;
        }
        if (has_alpha) {
            return LookupDirection::EnToZh;
        }
        return LookupDirection::None;
    }
    if (target_en) {
        return has_cjk ? LookupDirection::ZhToEn : LookupDirection::None;
    }
    if (target_zh) {
        return has_alpha ? LookupDirection::EnToZh : LookupDirection::None;
    }
    return LookupDirection::None;
}

struct EdgePunctSplit {
    std::string leading;
    std::string core;
    std::string trailing;
};

bool isEdgeWrapperToken(std::string_view cp_utf8) {
    static constexpr std::array<std::string_view, 26> kWrappers = {
        ".", ",", "!", "?", ":", ";", "\"", "'", "(", ")", "[", "]", "{", "}",
        "，", "。", "！", "？", "：", "；", "、", "“", "”", "‘", "’", "（）",
    };
    if (cp_utf8 == "（" || cp_utf8 == "）" || cp_utf8 == "【" || cp_utf8 == "】" ||
        cp_utf8 == "《" || cp_utf8 == "》") {
        return true;
    }
    for (std::string_view w : kWrappers) {
        if (cp_utf8 == w) {
            return true;
        }
    }
    return false;
}

std::string mapPunctuation(std::string_view tok, LookupDirection dir) {
    if (dir == LookupDirection::ZhToEn) {
        if (tok == "，") return ",";
        if (tok == "。") return ".";
        if (tok == "！") return "!";
        if (tok == "？") return "?";
        if (tok == "：") return ":";
        if (tok == "；") return ";";
        if (tok == "、") return ",";
        if (tok == "（") return "(";
        if (tok == "）") return ")";
        if (tok == "【") return "[";
        if (tok == "】") return "]";
        if (tok == "《" || tok == "》" || tok == "“" || tok == "”") return "\"";
        if (tok == "‘" || tok == "’") return "'";
        return std::string(tok);
    }
    if (dir == LookupDirection::EnToZh) {
        if (tok == ",") return "，";
        if (tok == ".") return "。";
        if (tok == "!") return "！";
        if (tok == "?") return "？";
        if (tok == ":") return "：";
        if (tok == ";") return "；";
        if (tok == "(") return "（";
        if (tok == ")") return "）";
        return std::string(tok);
    }
    return std::string(tok);
}

std::string mapEdgePunctuation(std::string_view raw, LookupDirection dir) {
    std::string out;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t next = shadowbar::utf8::nextCharOffset(raw, pos);
        if (next <= pos) {
            next = pos + 1;
        }
        out += mapPunctuation(raw.substr(pos, next - pos), dir);
        pos = next;
    }
    return out;
}

EdgePunctSplit splitEdgePunctuation(std::string_view s) {
    EdgePunctSplit res;
    size_t start = 0;
    while (start < s.size()) {
        size_t next = shadowbar::utf8::nextCharOffset(s, start);
        if (next <= start) {
            break;
        }
        std::string_view cp = s.substr(start, next - start);
        if (!isEdgeWrapperToken(cp) && !isAsciiSpace(cp[0])) {
            break;
        }
        res.leading.append(cp);
        start = next;
    }

    size_t end = s.size();
    while (end > start) {
        size_t prev = shadowbar::utf8::prevCharOffset(s, end);
        if (prev >= end) {
            break;
        }
        std::string_view cp = s.substr(prev, end - prev);
        if (!isEdgeWrapperToken(cp) && !isAsciiSpace(cp[0])) {
            break;
        }
        end = prev;
    }
    res.core = shadowbar::utf8::trim(s.substr(start, end - start));
    res.trailing = std::string(s.substr(end));
    res.leading = shadowbar::utf8::trim(res.leading);
    res.trailing = shadowbar::utf8::trim(res.trailing);
    return res;
}

bool isSentenceClosePunct(std::string_view s) {
    return s == "." || s == "," || s == "!" || s == "?" || s == ":" || s == ";" ||
           s == ")" || s == "]" || s == "}" || s == "，" || s == "。" || s == "！" ||
           s == "？" || s == "：" || s == "；" || s == "）" || s == "】" || s == "》" ||
           s == "”";
}

bool isOpenBracketOrQuote(std::string_view s) {
    return s == "(" || s == "[" || s == "{" || s == "（" || s == "【" || s == "《" ||
           s == "“";
}

void appendEnglishToken(std::string &out, std::string_view tok) {
    if (tok.empty()) {
        return;
    }
    if (out.empty()) {
        out.append(tok);
        return;
    }
    if (isSentenceClosePunct(tok)) {
        while (!out.empty() && out.back() == ' ') {
            out.pop_back();
        }
        out.append(tok);
        return;
    }
    char last = out.back();
    if (last == '(' || last == '[' || last == '{' || last == '\n') {
        out.append(tok);
        return;
    }
    out.push_back(' ');
    out.append(tok);
}

void appendChineseToken(std::string &out, std::string_view tok, bool is_punct) {
    if (tok.empty()) {
        return;
    }
    if (out.empty()) {
        out.append(tok);
        return;
    }
    if (is_punct) {
        while (!out.empty() && out.back() == ' ') {
            out.pop_back();
        }
        out.append(tok);
        return;
    }
    const char prev_ch = out.back();
    const bool prev_open_quote =
        (out.size() >= 3 && (out.compare(out.size() - 3, 3, "（") == 0 ||
                             out.compare(out.size() - 3, 3, "“") == 0 ||
                             out.compare(out.size() - 3, 3, "《") == 0)) ||
        prev_ch == '(' || prev_ch == '[' || prev_ch == '{';
    if (prev_open_quote || prev_ch == '\n' || prev_ch == ' ') {
        out.append(tok);
        return;
    }
    const bool prev_ascii_word =
        isAsciiAlnum(static_cast<unsigned char>(prev_ch));
    const bool curr_ascii_word =
        isAsciiAlnum(static_cast<unsigned char>(tok.front()));
    if (prev_ascii_word && curr_ascii_word) {
        out.push_back(' ');
    }
    out.append(tok);
}

std::vector<std::string> generateLemmaCandidates(std::string_view lower_word) {
    std::vector<std::string> cands;
    auto push = [&](std::string s) {
        if (!s.empty() &&
            std::find(cands.begin(), cands.end(), s) == cands.end()) {
            cands.push_back(std::move(s));
        }
    };

    std::string w(lower_word);
    push(w);

    if (w.size() > 2 && w.compare(w.size() - 2, 2, "'s") == 0) {
        w.resize(w.size() - 2);
        push(w);
    }

    const size_t n = w.size();
    // Plural / 3rd person singular
    if (n > 4 && w.compare(n - 3, 3, "ies") == 0) {
        push(w.substr(0, n - 3) + "y");
    }
    if (n > 4 && (w.compare(n - 4, 4, "ches") == 0 ||
                  w.compare(n - 4, 4, "shes") == 0 ||
                  w.compare(n - 4, 4, "sses") == 0 ||
                  w.compare(n - 3, 3, "xes") == 0 ||
                  w.compare(n - 3, 3, "zes") == 0 ||
                  w.compare(n - 3, 3, "oes") == 0)) {
        push(w.substr(0, n - 2));
    }
    if (n > 4 && w.compare(n - 3, 3, "ves") == 0) {
        push(w.substr(0, n - 3) + "f");
        push(w.substr(0, n - 3) + "fe");
    }
    if (n > 3 && w.back() == 's' &&
        w[n - 2] != 's' && w[n - 2] != 'u' && w[n - 2] != 'i') {
        push(w.substr(0, n - 1));
    }

    // Past tense / past participle (-ed)
    if (n > 4 && w.compare(n - 3, 3, "ied") == 0) {
        push(w.substr(0, n - 3) + "y");
    }
    if (n > 4 && w.compare(n - 2, 2, "ed") == 0) {
        push(w.substr(0, n - 1)); // configured -> configure, saved -> save
        push(w.substr(0, n - 2)); // checked -> check, failed -> fail
        if (n > 5 && w[n - 3] == w[n - 4]) {
            push(w.substr(0, n - 3)); // stopped -> stop, committed -> commit
        }
    }

    // Progressive / gerund (-ing)
    if (n > 5 && w.compare(n - 4, 4, "ying") == 0) {
        push(w.substr(0, n - 4) + "ie");
        push(w.substr(0, n - 3));
    }
    if (n > 5 && w.compare(n - 3, 3, "ing") == 0) {
        push(w.substr(0, n - 3)); // testing -> test, working -> work
        push(w.substr(0, n - 3) + "e"); // configuring -> configure, saving -> save
        if (n > 6 && w[n - 4] == w[n - 5]) {
            push(w.substr(0, n - 4)); // running -> run, logging -> log
        }
    }

    // Adverbs (-ly) & comparatives (-er/-est)
    if (n > 5 && w.compare(n - 4, 4, "ally") == 0) {
        push(w.substr(0, n - 2)); // automatically -> automatic
    }
    if (n > 4 && w.compare(n - 3, 3, "ily") == 0) {
        push(w.substr(0, n - 3) + "y"); // easily -> easy
    }
    if (n > 4 && w.compare(n - 2, 2, "ly") == 0) {
        push(w.substr(0, n - 2)); // quickly -> quick, directly -> direct
        if (w.size() > 3 && w[n - 3] == 'b') {
            push(w.substr(0, n - 2) + "e"); // possibly -> possible
        }
    }
    if (n > 4 && w.compare(n - 3, 3, "ier") == 0) {
        push(w.substr(0, n - 3) + "y");
    }
    if (n > 4 && w.compare(n - 2, 2, "er") == 0) {
        push(w.substr(0, n - 1));
        push(w.substr(0, n - 2));
    }
    if (n > 5 && w.compare(n - 4, 4, "iest") == 0) {
        push(w.substr(0, n - 4) + "y");
    }
    if (n > 5 && w.compare(n - 3, 3, "est") == 0) {
        push(w.substr(0, n - 2));
        push(w.substr(0, n - 3));
    }

    return cands;
}

bool isPreservedTechnicalToken(std::string_view tok) {
    if (tok.rfind("http://", 0) == 0 || tok.rfind("https://", 0) == 0 ||
        tok.rfind("ftp://", 0) == 0 || tok.rfind("~/", 0) == 0 ||
        tok.rfind("./", 0) == 0 || tok.rfind("../", 0) == 0) {
        return true;
    }
    if (!tok.empty() && tok.front() == '/' && tok.size() > 1) {
        return true;
    }
    if (tok.find("://") != std::string_view::npos ||
        tok.find("::") != std::string_view::npos ||
        tok.find('_') != std::string_view::npos ||
        tok.find('@') != std::string_view::npos) {
        return true;
    }
    // Pure numbers / version tags (e.g. 42, 3.14, v1.2.3)
    bool has_digit = false;
    bool all_num_ver = true;
    for (size_t i = 0; i < tok.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(tok[i]);
        if (std::isdigit(c)) {
            has_digit = true;
        } else if (c == '.' || c == '-' || ((c == 'v' || c == 'V') && i == 0)) {
            continue;
        } else {
            all_num_ver = false;
            break;
        }
    }
    return has_digit && all_num_ver;
}

bool isAsciiWordStart(unsigned char c) {
    return std::isalpha(c) != 0 || std::isdigit(c) != 0;
}

size_t scanAsciiSpan(std::string_view s, size_t pos, bool &is_tech) {
    // Recognize URLs / paths / identifiers cleanly without breaking on internal dots/slashes.
    if (s.compare(pos, 7, "http://") == 0 ||
        s.compare(pos, 8, "https://") == 0 ||
        s.compare(pos, 6, "ftp://") == 0 ||
        (pos + 1 < s.size() && (s[pos] == '/' || s[pos] == '~') && s[pos + 1] == '/')) {
        size_t end = pos;
        while (end < s.size()) {
            unsigned char ch = static_cast<unsigned char>(s[end]);
            if (isAsciiSpace(static_cast<char>(ch)) || ch >= 0x80 ||
                ch == ',' || ch == ';' || ch == ')' || ch == ']' || ch == '>' ||
                ch == '"' || ch == '\'') {
                break;
            }
            ++end;
        }
        is_tech = true;
        return end;
    }

    size_t end = pos;
    while (end < s.size()) {
        unsigned char ch = static_cast<unsigned char>(s[end]);
        if (isAsciiWordStart(ch)) {
            ++end;
            continue;
        }
        // Keep internal apostrophes (`don't`, `it's`, `user's`), slashes/dots/hyphens/underscores
        // inside words/versions/filenames (`v1.2.3`, `config.toml`, `snake_case`, `foo-bar`).
        if ((ch == '\'' || ch == '.' || ch == '-' || ch == '_' || ch == '/' || ch == '@') &&
            end + 1 < s.size() &&
            isAsciiWordStart(static_cast<unsigned char>(s[end + 1]))) {
            ++end;
            continue;
        }
        if (ch == ':' && end + 2 < s.size() && s[end + 1] == ':' &&
            isAsciiWordStart(static_cast<unsigned char>(s[end + 2]))) {
            end += 2;
            continue;
        }
        break;
    }
    std::string_view tok = s.substr(pos, end - pos);
    is_tech = isPreservedTechnicalToken(tok);
    return end;
}

std::string canonicalDictLang(std::string_view code) {
    std::string norm = shadowbar::normalizeLanguageCode(code);
    return norm.empty() ? std::string("auto") : norm;
}

std::string detectDictTextLang(std::string_view text, std::string_view hint) {
    std::string canon_hint = canonicalDictLang(hint);
    if (canon_hint != "auto") {
        return canon_hint;
    }
    std::string det = shadowbar::detectSourceLang(text, "");
    if (!det.empty() && det != "auto") {
        return det;
    }
    if (containsCjk(text)) {
        return "zh";
    }
    if (containsAsciiAlpha(text) && !containsNonAscii(text)) {
        return "en";
    }
    return "auto";
}

bool matchLangToken(std::string_view raw_tok, std::string &out_canon) {
    std::string lower = toLowerAscii(raw_tok);
    if (lower.size() < 2 || lower.size() > 5) {
        if (lower != "zh-tw" && lower != "zh_tw" && lower != "zh-cn" && lower != "zh_cn") {
            return false;
        }
    }
    static constexpr std::pair<std::string_view, std::string_view> kStrict[] = {
        {"zh", "zh"}, {"zh-cn", "zh"}, {"zh_cn", "zh"},
        {"zh-tw", "zh-TW"}, {"zh_tw", "zh-TW"},
        {"en", "en"}, {"ja", "ja"}, {"jp", "ja"},
        {"ko", "ko"}, {"kr", "ko"}, {"fr", "fr"},
        {"de", "de"}, {"es", "es"}, {"pt", "pt"},
        {"ru", "ru"}, {"it", "it"}, {"ar", "ar"},
        {"th", "th"}, {"vi", "vi"}, {"id", "id"},
        {"hi", "hi"}, {"nl", "nl"}, {"pl", "pl"},
        {"tr", "tr"}, {"uk", "uk"},
    };
    for (const auto &[k, v] : kStrict) {
        if (lower == k) {
            out_canon = std::string(v);
            return true;
        }
    }
    return false;
}

void inferLangPairFromPath(std::string_view path,
                           std::string &out_src,
                           std::string &out_tgt) {
    namespace fs = std::filesystem;
    std::string stem = toLowerAscii(fs::path(std::string(path)).filename().string());
    for (std::string_view ext : {".gz", ".dz", ".tsv", ".txt", ".csv", ".jsonl",
                                  ".json", ".u8", ".ifo", ".dict", ".idx",
                                  ".sqlite3", ".sqlite", ".db"}) {
        if (stem.size() >= ext.size() &&
            stem.compare(stem.size() - ext.size(), ext.size(), ext) == 0) {
            stem.erase(stem.size() - ext.size());
        }
    }

    // Tokenize stem around non-alphanumeric separators (preserving zh-tw / zh_cn compounds).
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < stem.size()) {
        if (!std::isalnum(static_cast<unsigned char>(stem[i]))) {
            ++i;
            continue;
        }
        if ((stem.compare(i, 5, "zh-tw") == 0 || stem.compare(i, 5, "zh_tw") == 0 ||
             stem.compare(i, 5, "zh-cn") == 0 || stem.compare(i, 5, "zh_cn") == 0) &&
            (i + 5 == stem.size() || !std::isalnum(static_cast<unsigned char>(stem[i + 5])))) {
            parts.push_back(stem.substr(i, 5));
            i += 5;
            continue;
        }
        size_t j = i;
        while (j < stem.size() && std::isalnum(static_cast<unsigned char>(stem[j]))) {
            ++j;
        }
        parts.push_back(stem.substr(i, j - i));
        i = j;
    }

    for (size_t k = 0; k + 1 < parts.size(); ++k) {
        // Also recognize "ko2zh" or "ja2en".
        std::string a, b;
        if (parts[k].size() >= 5 && parts[k][2] == '2' &&
            matchLangToken(std::string_view(parts[k]).substr(0, 2), a) &&
            matchLangToken(std::string_view(parts[k]).substr(3), b) && a != b) {
            out_src = a;
            out_tgt = b;
            return;
        }
        if (matchLangToken(parts[k], a) && matchLangToken(parts[k + 1], b) && a != b) {
            out_src = a;
            out_tgt = b;
            return;
        }
    }
    if (parts.size() == 1 && parts[0].size() >= 5 && parts[0][2] == '2') {
        std::string a, b;
        if (matchLangToken(std::string_view(parts[0]).substr(0, 2), a) &&
            matchLangToken(std::string_view(parts[0]).substr(3), b) && a != b) {
            out_src = a;
            out_tgt = b;
        }
    }
}

bool readFileBytesPossiblyGzip(const std::string &path,
                               std::string &out_bytes,
                               std::string *error) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        if (error) {
            *error = "无法打开文件";
        }
        return false;
    }
    in.seekg(0, std::ios::end);
    std::streamoff sz = in.tellg();
    in.seekg(0, std::ios::beg);
    std::string raw;
    if (sz > 0) {
        if (sz > 256LL * 1024 * 1024) {
            if (error) {
                *error = "词典文件过大（超过 256MB）";
            }
            return false;
        }
        raw.resize(static_cast<size_t>(sz));
        if (!in.read(raw.data(), sz)) {
            if (error) {
                *error = "读取文件失败";
            }
            return false;
        }
    } else {
        std::ostringstream oss;
        oss << in.rdbuf();
        raw = oss.str();
    }

    const bool is_gzip_magic =
        raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0x1F &&
        static_cast<unsigned char>(raw[1]) == 0x8B;
    if (!is_gzip_magic && !hasExtension(path, {".gz", ".dz"})) {
        out_bytes = std::move(raw);
        return true;
    }
    if (!is_gzip_magic) {
        out_bytes = std::move(raw);
        return true;
    }

#ifdef SHADOWBAR_HAS_ZLIB
    z_stream zs{};
    zs.next_in = reinterpret_cast<Bytef *>(raw.data());
    zs.avail_in = static_cast<uInt>(raw.size());
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) {
        if (error) {
            *error = "zlib 初始化解压失败";
        }
        return false;
    }
    out_bytes.clear();
    char buf[65536];
    int ret = Z_OK;
    do {
        zs.next_out = reinterpret_cast<Bytef *>(buf);
        zs.avail_out = sizeof(buf);
        ret = inflate(&zs, 0);
        if (out_bytes.size() < zs.total_out) {
            out_bytes.append(buf, zs.total_out - out_bytes.size());
        }
        if (out_bytes.size() > 256ULL * 1024 * 1024) {
            inflateEnd(&zs);
            if (error) {
                *error = "解压后词典超过 256MB 上限";
            }
            return false;
        }
    } while (ret == Z_OK);
    inflateEnd(&zs);
    if (ret != Z_STREAM_END) {
        if (error) {
            *error = "gzip/dictzip 解压损坏或格式错误";
        }
        return false;
    }
    return true;
#else
    if (error) {
        *error = "当前构建未启用 zlib，无法读取 .gz/.dz 压缩词典";
    }
    return false;
#endif
}

bool parseHeaderLangDirective(std::string_view comment_line,
                              std::string &hint_src,
                              std::string &hint_tgt) {
    if (comment_line.empty() || comment_line[0] != '#') {
        return false;
    }
    std::string_view body = shadowbar::utf8::trimView(comment_line.substr(1));
    std::string lower = toLowerAscii(body);
    auto extractAfter = [&](std::string_view prefix) -> std::string_view {
        if (lower.rfind(prefix, 0) == 0) {
            return shadowbar::utf8::trimView(body.substr(prefix.size()));
        }
        return {};
    };
    std::string_view val = extractAfter("pair:");
    if (val.empty()) val = extractAfter("pair=");
    if (val.empty()) val = extractAfter("lang:");
    if (val.empty()) val = extractAfter("lang=");
    if (!val.empty()) {
        for (std::string_view sep : {"->", "=>", "|", "-", "_", ":"}) {
            size_t p = val.find(sep);
            if (p != std::string_view::npos) {
                std::string s = canonicalDictLang(val.substr(0, p));
                std::string t = canonicalDictLang(val.substr(p + sep.size()));
                if (s != "auto") hint_src = s;
                if (t != "auto") hint_tgt = t;
                return true;
            }
        }
    }
    if (auto s = extractAfter("source_lang:"); !s.empty()) {
        hint_src = canonicalDictLang(s);
        return true;
    }
    if (auto s = extractAfter("target_lang:"); !s.empty()) {
        hint_tgt = canonicalDictLang(s);
        return true;
    }
    return false;
}

bool isConciseReverseHeadword(std::string_view s) {
    if (s.empty() || s.size() > 48) {
        return false;
    }
    if (countUtf8Codepoints(s) > 12) {
        return false;
    }
    return s.find(',') == std::string_view::npos &&
           s.find(';') == std::string_view::npos &&
           s.find('/') == std::string_view::npos &&
           s.find('\n') == std::string_view::npos &&
           s.find("，") == std::string_view::npos &&
           s.find("；") == std::string_view::npos &&
           s.find("、") == std::string_view::npos &&
           s.find("。") == std::string_view::npos;
}

std::string jsonValueToGloss(const nlohmann::json &v) {
    if (v.is_string()) {
        return shadowbar::utf8::trim(v.get<std::string>());
    }
    if (v.is_array()) {
        std::string joined;
        for (const auto &item : v) {
            if (!item.is_string()) {
                continue;
            }
            std::string s = shadowbar::utf8::trim(item.get<std::string>());
            if (s.empty()) {
                continue;
            }
            if (!joined.empty()) {
                joined += "; ";
            }
            joined += s;
        }
        return joined;
    }
    return {};
}

uint32_t readBe32(const unsigned char *p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

uint64_t readBe64(const unsigned char *p) {
    return (static_cast<uint64_t>(readBe32(p)) << 32) |
           static_cast<uint64_t>(readBe32(p + 4));
}

std::string extractStarDictTextPayload(std::string_view slice,
                                       std::string_view sametypesequence) {
    if (slice.empty()) {
        return {};
    }
    std::vector<std::string> parts;
    auto pushField = [&](char type_ch, std::string_view payload) {
        // Ignore phonetic/binary media sections when textual meaning is available.
        if (type_ch == 'y' || type_ch == 'r' || type_ch == 'W' ||
            type_ch == 'P' || type_ch == 'X') {
            return;
        }
        std::string cleaned = (type_ch == 'h' || type_ch == 'x' || type_ch == 'k' || type_ch == 'g')
                                  ? stripHtmlMarkup(payload)
                                  : std::string(payload);
        cleaned = shadowbar::utf8::trim(cleaned);
        if (!cleaned.empty()) {
            parts.push_back(std::move(cleaned));
        }
    };

    if (!sametypesequence.empty()) {
        size_t pos = 0;
        for (size_t i = 0; i < sametypesequence.size() && pos < slice.size(); ++i) {
            const char t = sametypesequence[i];
            const bool is_last = (i + 1 == sametypesequence.size());
            if (is_last) {
                pushField(t, slice.substr(pos));
                break;
            }
            if (t == 'W' || t == 'P' || t == 'X') {
                if (pos + 4 > slice.size()) {
                    break;
                }
                uint32_t len = readBe32(reinterpret_cast<const unsigned char *>(slice.data() + pos));
                pos += 4;
                size_t take = std::min<size_t>(len, slice.size() - pos);
                pos += take;
            } else {
                size_t nul = slice.find('\0', pos);
                if (nul == std::string_view::npos) {
                    pushField(t, slice.substr(pos));
                    break;
                }
                pushField(t, slice.substr(pos, nul - pos));
                pos = nul + 1;
            }
        }
    } else {
        size_t pos = 0;
        while (pos < slice.size()) {
            const char t = slice[pos++];
            if (t == 'W' || t == 'P' || t == 'X') {
                if (pos + 4 > slice.size()) {
                    break;
                }
                uint32_t len = readBe32(reinterpret_cast<const unsigned char *>(slice.data() + pos));
                pos += 4;
                size_t take = std::min<size_t>(len, slice.size() - pos);
                pos += take;
            } else {
                size_t nul = slice.find('\0', pos);
                if (nul == std::string_view::npos) {
                    pushField(t, slice.substr(pos));
                    break;
                }
                pushField(t, slice.substr(pos, nul - pos));
                pos = nul + 1;
            }
        }
    }

    if (parts.empty()) {
        return {};
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            out.push_back('\n');
        }
        out += parts[i];
    }
    return out;
}

struct DictSink {
    OfflineDictMap *zh_to_en = nullptr;
    OfflineDictMap *en_to_zh = nullptr;
    std::unordered_map<std::string, OfflineDictionary::PairDictBucket> *pair_maps = nullptr;
    size_t *max_zh_cp = nullptr;
    size_t *max_en_words = nullptr;
    const OfflineDictMap *builtin_zh_to_en = nullptr;
    const OfflineDictMap *builtin_en_to_zh = nullptr;
    std::vector<OfflineDictionary::SqliteDictHandle> *sqlite_handles = nullptr;
    bool inspect_only = false;

    size_t inserted_count = 0;
    std::unordered_map<std::string, size_t> pair_counts;

    void notePair(std::string_view src_lang, std::string_view tgt_lang) {
        if (src_lang.empty() || tgt_lang.empty() || src_lang == "auto" ||
            tgt_lang == "auto" || src_lang == tgt_lang) {
            return;
        }
        std::string key;
        key.reserve(src_lang.size() + tgt_lang.size() + 1);
        key.append(src_lang);
        key.push_back('|');
        key.append(tgt_lang);
        ++pair_counts[std::move(key)];
    }

    void addPairEntry(std::string_view src_lang,
                      std::string_view tgt_lang,
                      std::string_view src,
                      std::string_view tgt,
                      bool ecdict_csv_mode,
                      bool allow_reverse) {
        if (src.empty() || tgt.empty() || src_lang == "auto" || tgt_lang == "auto" ||
            src_lang == tgt_lang) {
            return;
        }
        notePair(src_lang, tgt_lang);
        if (inspect_only) {
            ++inserted_count;
            return;
        }

        if (src_lang == "zh" && tgt_lang == "en" && zh_to_en) {
            std::string key(src);
            (*zh_to_en)[key] = std::string(tgt);
            if (max_zh_cp) {
                *max_zh_cp = std::max(*max_zh_cp, countUtf8Codepoints(key));
            }
            if (allow_reverse && en_to_zh) {
                std::string rev_key = toLowerAscii(tgt);
                if (countAsciiWords(rev_key) <= 4 && !en_to_zh->count(rev_key)) {
                    (*en_to_zh)[rev_key] = std::move(key);
                    if (max_en_words) {
                        *max_en_words = std::max(*max_en_words, countAsciiWords(rev_key));
                    }
                }
            }
            ++inserted_count;
            return;
        }

        if (src_lang == "en" && tgt_lang == "zh" && en_to_zh) {
            std::string lower_src = toLowerAscii(src);
            if (ecdict_csv_mode) {
                if ((builtin_en_to_zh && builtin_en_to_zh->count(lower_src)) ||
                    en_to_zh->count(lower_src)) {
                    return;
                }
            }
            (*en_to_zh)[lower_src] = std::string(tgt);
            if (max_en_words) {
                *max_en_words = std::max(*max_en_words, countAsciiWords(lower_src));
            }
            if (allow_reverse && zh_to_en && !ecdict_csv_mode &&
                countUtf8Codepoints(tgt) <= 8 && isConciseReverseHeadword(tgt) &&
                !zh_to_en->count(tgt)) {
                (*zh_to_en)[std::string(tgt)] = std::string(src);
                if (max_zh_cp) {
                    *max_zh_cp = std::max(*max_zh_cp, countUtf8Codepoints(tgt));
                }
            }
            ++inserted_count;
            return;
        }

        if (!pair_maps) {
            return;
        }
        std::string pkey = std::string(src_lang) + "|" + std::string(tgt_lang);
        auto &bucket = (*pair_maps)[pkey];
        std::string s_key(src);
        std::string s_lower = toLowerAscii(src);
        bucket.exact_map[s_key] = std::string(tgt);
        if (s_lower != s_key || containsAsciiAlpha(s_lower)) {
            bucket.lower_map[s_lower] = std::string(tgt);
        }
        bucket.max_cp = std::max(bucket.max_cp, countUtf8Codepoints(s_key));
        bucket.max_words = std::max(bucket.max_words, countAsciiWords(s_key));
        ++inserted_count;

        if (allow_reverse && isConciseReverseHeadword(tgt)) {
            std::string rev_pkey = std::string(tgt_lang) + "|" + std::string(src_lang);
            auto &rev_bucket = (*pair_maps)[rev_pkey];
            if (!rev_bucket.exact_map.count(tgt)) {
                std::string t_key(tgt);
                std::string t_lower = toLowerAscii(tgt);
                rev_bucket.exact_map[t_key] = s_key;
                if (t_lower != t_key || containsAsciiAlpha(t_lower)) {
                    rev_bucket.lower_map[t_lower] = s_key;
                }
                rev_bucket.max_cp = std::max(rev_bucket.max_cp, countUtf8Codepoints(t_key));
                rev_bucket.max_words = std::max(rev_bucket.max_words, countAsciiWords(t_key));
            }
        }
    }

    void addAutoOrTypedEntry(std::string_view src_raw,
                             std::string_view tgt_raw,
                             std::string_view hint_src,
                             std::string_view hint_tgt,
                             bool ecdict_csv_mode,
                             bool allow_reverse) {
        std::string src = shadowbar::utf8::trim(src_raw);
        std::string tgt = shadowbar::utf8::trim(tgt_raw);
        if (src.empty() || tgt.empty()) {
            return;
        }
        std::string sl = detectDictTextLang(src, hint_src);
        std::string tl = detectDictTextLang(tgt, hint_tgt);
        if (sl == "auto" && tl != "auto") {
            sl = containsCjk(src) ? "zh" : (containsAsciiAlpha(src) ? "en" : "auto");
        }
        if (tl == "auto" && sl != "auto") {
            tl = containsCjk(tgt) ? "zh" : (containsAsciiAlpha(tgt) ? "en" : "auto");
        }
        if (sl == "auto" && tl == "auto") {
            if (containsCjk(src) && !containsCjk(tgt)) {
                sl = "zh";
                tl = "en";
            } else if (!containsCjk(src) && containsCjk(tgt)) {
                sl = "en";
                tl = "zh";
            }
        }
        addPairEntry(sl, tl, src, tgt, ecdict_csv_mode, allow_reverse);
    }
};

void finalizeReportPairs(const DictSink &sink,
                         std::string_view hint_src,
                         std::string_view hint_tgt,
                         DictSourceReport &report) {
    report.source_lang = canonicalDictLang(hint_src);
    report.target_lang = canonicalDictLang(hint_tgt);
    if ((report.source_lang == "auto" || report.target_lang == "auto") &&
        !sink.pair_counts.empty()) {
        auto best = std::max_element(
            sink.pair_counts.begin(), sink.pair_counts.end(),
            [](const auto &a, const auto &b) { return a.second < b.second; });
        size_t bar = best->first.find('|');
        if (bar != std::string::npos) {
            if (report.source_lang == "auto") {
                report.source_lang = best->first.substr(0, bar);
            }
            if (report.target_lang == "auto") {
                report.target_lang = best->first.substr(bar + 1);
            }
        }
    }
}

void processSqliteDictSpec(const std::string &path,
                           std::string_view hint_src,
                           std::string_view hint_tgt,
                           bool inspect_only,
                           DictSink &sink,
                           DictSourceReport &report) {
    report.format = "sqlite";
#ifdef SHADOWBAR_HAS_SQLITE3
    sqlite3 *conn = nullptr;
    if (sqlite3_open_v2(path.c_str(), &conn,
                        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        if (conn) {
            sqlite3_close_v2(conn);
        }
        report.message = "无法打开 SQLite 数据库";
        return;
    }

    std::vector<std::string> tables;
    sqlite3_stmt *tbl_stmt = nullptr;
    if (sqlite3_prepare_v2(conn,
                           "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'",
                           -1, &tbl_stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(tbl_stmt) == SQLITE_ROW) {
            const unsigned char *txt = sqlite3_column_text(tbl_stmt, 0);
            if (txt) {
                tables.emplace_back(reinterpret_cast<const char *>(txt));
            }
        }
        sqlite3_finalize(tbl_stmt);
    }

    auto rankTable = [](std::string_view t) -> int {
        std::string l = toLowerAscii(t);
        if (l == "stardict") return 0;
        if (l == "ecdict") return 1;
        if (l == "dict" || l == "words" || l == "entries") return 2;
        return 3;
    };
    std::sort(tables.begin(), tables.end(), [&](const std::string &a, const std::string &b) {
        return rankTable(a) < rankTable(b);
    });

    for (const std::string &table : tables) {
        std::string sql = "PRAGMA table_info(" + quoteSqliteIdent(table) + ")";
        sqlite3_stmt *col_stmt = nullptr;
        if (sqlite3_prepare_v2(conn, sql.c_str(), -1, &col_stmt, nullptr) != SQLITE_OK) {
            continue;
        }
        std::vector<std::string> cols;
        while (sqlite3_step(col_stmt) == SQLITE_ROW) {
            const unsigned char *txt = sqlite3_column_text(col_stmt, 1);
            if (txt) {
                cols.emplace_back(reinterpret_cast<const char *>(txt));
            }
        }
        sqlite3_finalize(col_stmt);

        auto findCol = [&](std::initializer_list<std::string_view> names) -> std::string {
            for (std::string_view target : names) {
                for (const std::string &c : cols) {
                    if (toLowerAscii(c) == target) {
                        return c;
                    }
                }
            }
            return {};
        };

        std::string word_col = findCol({"word", "en", "english", "term", "headword", "src", "source", "key", "sw"});
        std::string trans_col = findCol({"translation", "trans", "zh", "chinese", "meaning", "def", "definition", "tgt", "target", "value"});
        if (word_col.empty() || trans_col.empty() || word_col == trans_col) {
            continue;
        }

        const std::string q_tbl = quoteSqliteIdent(table);
        const std::string q_word = quoteSqliteIdent(word_col);
        const std::string q_trans = quoteSqliteIdent(trans_col);

        size_t row_count = 0;
        {
            sqlite3_stmt *cnt_stmt = nullptr;
            const std::string cnt_sql = "SELECT COUNT(*) FROM " + q_tbl;
            if (sqlite3_prepare_v2(conn, cnt_sql.c_str(), -1, &cnt_stmt, nullptr) == SQLITE_OK) {
                if (sqlite3_step(cnt_stmt) == SQLITE_ROW) {
                    row_count = static_cast<size_t>(std::max<sqlite3_int64>(0, sqlite3_column_int64(cnt_stmt, 0)));
                }
                sqlite3_finalize(cnt_stmt);
            }
        }

        std::string eff_src = canonicalDictLang(hint_src);
        std::string eff_tgt = canonicalDictLang(hint_tgt);
        if (eff_src == "auto" || eff_tgt == "auto") {
            sqlite3_stmt *sample_stmt = nullptr;
            const std::string sample_sql =
                "SELECT " + q_word + ", " + q_trans + " FROM " + q_tbl + " LIMIT 24";
            if (sqlite3_prepare_v2(conn, sample_sql.c_str(), -1, &sample_stmt, nullptr) == SQLITE_OK) {
                while (sqlite3_step(sample_stmt) == SQLITE_ROW) {
                    const unsigned char *w = sqlite3_column_text(sample_stmt, 0);
                    const unsigned char *t = sqlite3_column_text(sample_stmt, 1);
                    if (w && t) {
                        sink.notePair(
                            detectDictTextLang(reinterpret_cast<const char *>(w), eff_src),
                            detectDictTextLang(reinterpret_cast<const char *>(t), eff_tgt));
                    }
                }
                sqlite3_finalize(sample_stmt);
            }
        }
        finalizeReportPairs(sink, eff_src, eff_tgt, report);
        report.entry_count = row_count;
        report.loaded = true;
        report.message = "SQLite 词库表: " + table;

        if (inspect_only || !sink.sqlite_handles) {
            sqlite3_close_v2(conn);
            return;
        }

        sqlite3_stmt *ci_stmt = nullptr;
        const std::string ci_sql =
            "SELECT " + q_trans + " FROM " + q_tbl +
            " WHERE " + q_word + " = ?1 COLLATE NOCASE LIMIT 1";
        sqlite3_prepare_v2(conn, ci_sql.c_str(), -1, &ci_stmt, nullptr);

        bool has_non_ascii = false;
        sqlite3_stmt *probe_stmt = nullptr;
        const std::string probe_sql =
            "SELECT 1 FROM " + q_tbl + " WHERE " + q_word + " > '~' LIMIT 1";
        if (sqlite3_prepare_v2(conn, probe_sql.c_str(), -1, &probe_stmt, nullptr) == SQLITE_OK) {
            has_non_ascii = (sqlite3_step(probe_stmt) == SQLITE_ROW);
            sqlite3_finalize(probe_stmt);
        }

        sqlite3_stmt *cs_stmt = nullptr;
        if (has_non_ascii || (report.source_lang != "auto" && report.source_lang != "en")) {
            const std::string cs_sql =
                "SELECT " + q_trans + " FROM " + q_tbl +
                " WHERE " + q_word + " = ?1 LIMIT 1";
            sqlite3_prepare_v2(conn, cs_sql.c_str(), -1, &cs_stmt, nullptr);
        }

        OfflineDictionary::SqliteDictHandle h;
        h.path = path;
        h.source_lang = report.source_lang;
        h.target_lang = report.target_lang;
        h.db = conn;
        h.stmt_ci = ci_stmt;
        h.stmt_cs = cs_stmt;
        h.has_non_ascii_keys = has_non_ascii;
        h.word_table = table;
        h.word_col = std::move(word_col);
        h.trans_col = std::move(trans_col);
        sink.sqlite_handles->push_back(std::move(h));
        return;
    }
    sqlite3_close_v2(conn);
    report.message = "未在 SQLite 中找到包含词头与释义列的词典表";
#else
    (void)path;
    (void)hint_src;
    (void)hint_tgt;
    (void)inspect_only;
    (void)sink;
    report.message = "当前构建未链接 SQLite3";
#endif
}

void processStarDictSpec(const std::string &raw_path,
                         std::string_view hint_src,
                         std::string_view hint_tgt,
                         bool inspect_only,
                         DictSink &sink,
                         DictSourceReport &report) {
    namespace fs = std::filesystem;
    report.format = "stardict";

    fs::path p(raw_path);
    std::string stem = p.string();
    for (std::string_view ext : {".ifo", ".idx.gz", ".idx", ".dict.dz", ".dict.gz", ".dict"}) {
        if (stem.size() >= ext.size()) {
            std::string tail = toLowerAscii(stem.substr(stem.size() - ext.size()));
            if (tail == ext) {
                stem.erase(stem.size() - ext.size());
                break;
            }
        }
    }

    const fs::path ifo_path(stem + ".ifo");
    std::error_code ec;
    if (!fs::exists(ifo_path, ec)) {
        report.message = "缺少 StarDict 元数据文件 (.ifo): " + ifo_path.filename().string();
        return;
    }

    std::string ifo_bytes;
    std::string io_err;
    if (!readFileBytesPossiblyGzip(ifo_path.string(), ifo_bytes, &io_err)) {
        report.message = io_err;
        return;
    }

    int idx_offset_bits = 32;
    size_t declared_wordcount = 0;
    std::string sametypesequence;
    std::string bookname;
    {
        std::istringstream iss(ifo_bytes);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            size_t eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            std::string k = toLowerAscii(shadowbar::utf8::trimView(std::string_view(line).substr(0, eq)));
            std::string v = shadowbar::utf8::trim(std::string_view(line).substr(eq + 1));
            if (k == "idxoffsetbits") {
                try {
                    idx_offset_bits = (std::stoi(v) == 64) ? 64 : 32;
                } catch (...) {
                }
            } else if (k == "wordcount") {
                try {
                    declared_wordcount = static_cast<size_t>(std::stoull(v));
                } catch (...) {
                }
            } else if (k == "sametypesequence") {
                sametypesequence = v;
            } else if (k == "bookname") {
                bookname = v;
            }
        }
    }

    fs::path idx_path = fs::path(stem + ".idx");
    if (!fs::exists(idx_path, ec)) {
        idx_path = fs::path(stem + ".idx.gz");
    }
    if (!fs::exists(idx_path, ec)) {
        report.message = "缺少 StarDict 索引文件 (.idx / .idx.gz)";
        return;
    }

    bool dict_is_gz = false;
    fs::path dict_path = fs::path(stem + ".dict");
    if (!fs::exists(dict_path, ec)) {
        if (fs::exists(fs::path(stem + ".dict.dz"), ec)) {
            dict_path = fs::path(stem + ".dict.dz");
            dict_is_gz = true;
        } else if (fs::exists(fs::path(stem + ".dict.gz"), ec)) {
            dict_path = fs::path(stem + ".dict.gz");
            dict_is_gz = true;
        } else {
            report.message = "缺少 StarDict 数据文件 (.dict / .dict.dz)";
            return;
        }
    }

    std::string idx_bytes;
    if (!readFileBytesPossiblyGzip(idx_path.string(), idx_bytes, &io_err)) {
        report.message = "读取 .idx 失败: " + io_err;
        return;
    }

    std::string dict_buf;
    std::ifstream dict_stream;
    if (dict_is_gz) {
        if (!readFileBytesPossiblyGzip(dict_path.string(), dict_buf, &io_err)) {
            report.message = "读取 .dict.dz 失败: " + io_err;
            return;
        }
    } else {
        dict_stream.open(dict_path, std::ios::binary);
        if (!dict_stream.is_open()) {
            report.message = "无法打开 StarDict .dict 文件";
            return;
        }
    }

    std::string eff_src = canonicalDictLang(hint_src);
    std::string eff_tgt = canonicalDictLang(hint_tgt);
    if ((eff_src == "auto" || eff_tgt == "auto") && !bookname.empty()) {
        std::string bsrc = "auto", btgt = "auto";
        inferLangPairFromPath(bookname, bsrc, btgt);
        if (eff_src == "auto") eff_src = bsrc;
        if (eff_tgt == "auto") eff_tgt = btgt;
    }

    const size_t offset_bytes = (idx_offset_bits == 64) ? 8 : 4;
    size_t pos = 0;
    size_t entry_index = 0;
    std::string read_scratch;
    while (pos < idx_bytes.size()) {
        size_t nul = idx_bytes.find('\0', pos);
        if (nul == std::string::npos || nul + 1 + offset_bytes + 4 > idx_bytes.size()) {
            break;
        }
        std::string_view headword(idx_bytes.data() + pos, nul - pos);
        const unsigned char *meta =
            reinterpret_cast<const unsigned char *>(idx_bytes.data() + nul + 1);
        const uint64_t off = (offset_bytes == 8) ? readBe64(meta) : readBe32(meta);
        const uint32_t dsize = readBe32(meta + offset_bytes);
        pos = nul + 1 + offset_bytes + 4;

        if (inspect_only && entry_index >= 64) {
            ++entry_index;
            continue;
        }

        if (dsize == 0 || dsize > 1024 * 1024) {
            continue;
        }
        std::string_view slice;
        if (dict_is_gz) {
            if (off + dsize > dict_buf.size()) {
                continue;
            }
            slice = std::string_view(dict_buf.data() + off, dsize);
        } else {
            dict_stream.seekg(static_cast<std::streamoff>(off), std::ios::beg);
            read_scratch.resize(dsize);
            if (!dict_stream.read(read_scratch.data(), static_cast<std::streamsize>(dsize))) {
                dict_stream.clear();
                continue;
            }
            slice = read_scratch;
        }

        std::string gloss = extractStarDictTextPayload(slice, sametypesequence);
        if (!gloss.empty()) {
            sink.addAutoOrTypedEntry(headword, gloss, eff_src, eff_tgt,
                                     /*ecdict_csv_mode=*/false, /*allow_reverse=*/false);
        }
        ++entry_index;
    }

    finalizeReportPairs(sink, eff_src, eff_tgt, report);
    report.entry_count = inspect_only
                             ? std::max(declared_wordcount, entry_index)
                             : sink.inserted_count;
    report.loaded = (report.entry_count > 0);
    if (!bookname.empty()) {
        report.message = bookname;
    } else if (!report.loaded) {
        report.message = "StarDict 词库未解析到有效词条";
    }
}

void processJsonDictSpec(const std::string &path,
                         std::string_view hint_src,
                         std::string_view hint_tgt,
                         bool is_jsonl,
                         DictSink &sink,
                         DictSourceReport &report) {
    report.format = is_jsonl ? "jsonl" : "json";
    std::string bytes;
    std::string err;
    if (!readFileBytesPossiblyGzip(path, bytes, &err)) {
        report.message = err;
        return;
    }
    stripUtf8Bom(bytes);

    std::string eff_src = canonicalDictLang(hint_src);
    std::string eff_tgt = canonicalDictLang(hint_tgt);

    auto handleObj = [&](const nlohmann::json &obj) {
        if (obj.is_array() && obj.size() >= 2 && obj[0].is_string()) {
            std::string tgt = jsonValueToGloss(obj[1]);
            std::string rs = (obj.size() >= 3 && obj[2].is_string())
                                 ? canonicalDictLang(obj[2].get<std::string>())
                                 : eff_src;
            std::string rt = (obj.size() >= 4 && obj[3].is_string())
                                 ? canonicalDictLang(obj[3].get<std::string>())
                                 : eff_tgt;
            sink.addAutoOrTypedEntry(obj[0].get<std::string>(), tgt, rs, rt,
                                     /*ecdict_csv_mode=*/false, /*allow_reverse=*/true);
            return;
        }
        if (!obj.is_object()) {
            return;
        }
        std::string src;
        for (const char *k : {"word", "src", "source", "headword", "term", "query", "key", "q"}) {
            if (obj.contains(k) && obj[k].is_string()) {
                src = obj[k].get<std::string>();
                break;
            }
        }
        std::string tgt;
        for (const char *k : {"translation", "translations", "trans", "tgt", "target",
                              "meaning", "meanings", "def", "definition", "definitions", "value", "a"}) {
            if (obj.contains(k)) {
                tgt = jsonValueToGloss(obj[k]);
                if (!tgt.empty()) {
                    break;
                }
            }
        }
        if (src.empty() || tgt.empty()) {
            return;
        }
        std::string rs = eff_src;
        std::string rt = eff_tgt;
        for (const char *k : {"source_lang", "src_lang", "from"}) {
            if (obj.contains(k) && obj[k].is_string()) {
                rs = canonicalDictLang(obj[k].get<std::string>());
                break;
            }
        }
        for (const char *k : {"target_lang", "tgt_lang", "to"}) {
            if (obj.contains(k) && obj[k].is_string()) {
                rt = canonicalDictLang(obj[k].get<std::string>());
                break;
            }
        }
        sink.addAutoOrTypedEntry(src, tgt, rs, rt,
                                 /*ecdict_csv_mode=*/false, /*allow_reverse=*/true);
    };

    try {
        if (is_jsonl) {
            std::istringstream iss(bytes);
            std::string line;
            while (std::getline(iss, line)) {
                std::string t = shadowbar::utf8::trim(line);
                if (t.empty() || t[0] == '#') {
                    continue;
                }
                handleObj(nlohmann::json::parse(t, nullptr, false));
            }
        } else {
            auto root = nlohmann::json::parse(bytes);
            if (root.is_object()) {
                if (root.contains("source_lang") && root["source_lang"].is_string() && eff_src == "auto") {
                    eff_src = canonicalDictLang(root["source_lang"].get<std::string>());
                }
                if (root.contains("target_lang") && root["target_lang"].is_string() && eff_tgt == "auto") {
                    eff_tgt = canonicalDictLang(root["target_lang"].get<std::string>());
                }
                const nlohmann::json *arr = nullptr;
                for (const char *k : {"entries", "words", "items", "data", "dictionary"}) {
                    if (root.contains(k) && root[k].is_array()) {
                        arr = &root[k];
                        break;
                    }
                }
                if (arr) {
                    for (const auto &item : *arr) {
                        handleObj(item);
                    }
                } else {
                    for (auto it = root.begin(); it != root.end(); ++it) {
                        if (it.key() == "source_lang" || it.key() == "target_lang" ||
                            it.key() == "_meta" || it.key() == "metadata") {
                            continue;
                        }
                        if (it.value().is_string() || it.value().is_array()) {
                            sink.addAutoOrTypedEntry(it.key(), jsonValueToGloss(it.value()),
                                                     eff_src, eff_tgt, false, true);
                        } else if (it.value().is_object()) {
                            nlohmann::json copy = it.value();
                            if (!copy.contains("word") && !copy.contains("src")) {
                                copy["word"] = it.key();
                            }
                            handleObj(copy);
                        }
                    }
                }
            } else if (root.is_array()) {
                for (const auto &item : root) {
                    handleObj(item);
                }
            }
        }
    } catch (const std::exception &e) {
        report.message = std::string("JSON 解析失败: ") + e.what();
        return;
    }

    finalizeReportPairs(sink, eff_src, eff_tgt, report);
    report.entry_count = sink.inserted_count;
    report.loaded = (sink.inserted_count > 0);
    if (!report.loaded && report.message.empty()) {
        report.message = "JSON 中未找到有效词条";
    }
}

void processDelimitedDictSpec(const std::string &path,
                              std::string_view hint_src,
                              std::string_view hint_tgt,
                              DictSink &sink,
                              DictSourceReport &report) {
    const bool is_cedict = hasExtension(path, {".u8", ".u8.gz"});
    const bool is_csv = hasExtension(path, {".csv", ".csv.gz"});
    const bool is_gz = hasExtension(path, {".gz", ".dz"});
    report.format = is_cedict ? "cedict" : (is_csv ? "csv" : "tsv");

    std::string decompressed_buf;
    std::unique_ptr<std::istringstream> mem_stream;
    std::ifstream file_stream;
    std::istream *in = nullptr;
    if (is_gz) {
        std::string err;
        if (!readFileBytesPossiblyGzip(path, decompressed_buf, &err)) {
            report.message = err;
            return;
        }
        mem_stream = std::make_unique<std::istringstream>(std::move(decompressed_buf));
        in = mem_stream.get();
    } else {
        file_stream.open(path);
        if (!file_stream.is_open()) {
            report.message = "无法打开词典文件";
            return;
        }
        in = &file_stream;
    }

    std::string eff_src = canonicalDictLang(hint_src);
    std::string eff_tgt = canonicalDictLang(hint_tgt);

    std::unordered_map<std::string, int, TransparentStringHash, std::equal_to<>> cedict_scores;
    if (is_cedict && !sink.inspect_only && sink.zh_to_en) {
        cedict_scores.reserve(163840);
        sink.zh_to_en->reserve(sink.zh_to_en->size() + 163840);
    }

    int word_col = -1;
    int trans_col = -1;
    int src_lang_col = -1;
    int tgt_lang_col = -1;
    bool checked_header = false;
    bool ecdict_mode = is_csv && (eff_src == "auto" || eff_src == "en") &&
                       (eff_tgt == "auto" || eff_tgt == "zh");
    std::string ecdict_col_src_hint;
    std::string ecdict_col_tgt_hint;

    std::string line;
    bool first_line = true;
    while (std::getline(*in, line)) {
        if (first_line) {
            stripUtf8Bom(line);
            first_line = false;
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::string trimmed = shadowbar::utf8::trim(line);
        if (trimmed.empty() || trimmed.rfind("//", 0) == 0) {
            continue;
        }
        if (trimmed[0] == '#') {
            parseHeaderLangDirective(trimmed, eff_src, eff_tgt);
            continue;
        }

        if (is_cedict) {
            size_t sp1 = trimmed.find(' ');
            if (sp1 == std::string::npos) continue;
            size_t sp2 = trimmed.find(' ', sp1 + 1);
            if (sp2 == std::string::npos) continue;
            size_t slash = trimmed.find('/', sp2);
            if (slash == std::string::npos) continue;

            std::string_view trad(trimmed.data(), sp1);
            std::string_view simp(trimmed.data() + sp1 + 1, sp2 - sp1 - 1);
            std::string_view pinyin;
            size_t p_open = trimmed.find('[', sp2);
            if (p_open != std::string::npos && p_open < slash) {
                size_t p_close = trimmed.find(']', p_open + 1);
                if (p_close != std::string::npos && p_close < slash) {
                    pinyin = std::string_view(trimmed.data() + p_open + 1, p_close - p_open - 1);
                }
            }
            std::string_view defs(trimmed.data() + slash + 1, trimmed.size() - slash - 1);
            if (!defs.empty() && defs.back() == '/') {
                defs.remove_suffix(1);
            }
            int score = -10000;
            std::string best_gloss = extractBestCedictGloss(pinyin, defs, score);
            if (best_gloss.empty() || score < -500) {
                continue;
            }
            sink.notePair("zh", "en");
            if (sink.inspect_only || !sink.zh_to_en) {
                ++sink.inserted_count;
                if (trad != simp) ++sink.inserted_count;
                continue;
            }
            auto insertScored = [&](std::string_view key_sv) {
                if (sink.builtin_zh_to_en && sink.builtin_zh_to_en->count(key_sv) > 0) {
                    return;
                }
                auto existing = sink.zh_to_en->find(key_sv);
                auto sc_it = cedict_scores.find(key_sv);
                if (existing != sink.zh_to_en->end() && sc_it == cedict_scores.end()) {
                    return;
                }
                if (sc_it == cedict_scores.end()) {
                    std::string key(key_sv);
                    sink.zh_to_en->emplace(key, best_gloss);
                    cedict_scores.emplace(std::move(key), score);
                    if (sink.max_zh_cp) {
                        *sink.max_zh_cp = std::max(*sink.max_zh_cp, countUtf8Codepoints(key_sv));
                    }
                    ++sink.inserted_count;
                } else if (score > sc_it->second) {
                    sc_it->second = score;
                    existing->second = best_gloss;
                }
            };
            insertScored(simp);
            if (trad != simp) {
                insertScored(trad);
            }
            continue;
        }

        if (is_csv) {
            std::vector<std::string> cols = parseCsvRow(trimmed);
            if (!checked_header) {
                checked_header = true;
                bool looks_like_header = false;
                for (size_t i = 0; i < cols.size(); ++i) {
                    std::string col = toLowerAscii(shadowbar::utf8::trimView(cols[i]));
                    std::string lang_tok;
                    if (col == "word" || col == "src" || col == "source" ||
                        col == "headword" || col == "term" || col == "query" || col == "key") {
                        word_col = static_cast<int>(i);
                        looks_like_header = true;
                    } else if (col == "translation" || col == "trans" || col == "tgt" ||
                               col == "target" || col == "meaning" || col == "def" ||
                               col == "definition" || col == "value") {
                        trans_col = static_cast<int>(i);
                        looks_like_header = true;
                    } else if (col == "source_lang" || col == "src_lang" || col == "from") {
                        src_lang_col = static_cast<int>(i);
                        looks_like_header = true;
                    } else if (col == "target_lang" || col == "tgt_lang" || col == "to") {
                        tgt_lang_col = static_cast<int>(i);
                        looks_like_header = true;
                    } else if (matchLangToken(col, lang_tok)) {
                        if (word_col < 0) {
                            word_col = static_cast<int>(i);
                            ecdict_col_src_hint = lang_tok;
                        } else if (trans_col < 0) {
                            trans_col = static_cast<int>(i);
                            ecdict_col_tgt_hint = lang_tok;
                        }
                        looks_like_header = true;
                    }
                }
                if (word_col >= 0 && trans_col >= 0 && looks_like_header) {
                    if (eff_src == "auto" && !ecdict_col_src_hint.empty()) {
                        eff_src = ecdict_col_src_hint;
                    }
                    if (eff_tgt == "auto" && !ecdict_col_tgt_hint.empty()) {
                        eff_tgt = ecdict_col_tgt_hint;
                    }
                    ecdict_mode = (eff_src == "auto" || eff_src == "en") &&
                                  (eff_tgt == "auto" || eff_tgt == "zh");
                    continue;
                }
                if (cols.size() >= 2) {
                    word_col = 0;
                    trans_col = 1;
                } else {
                    continue;
                }
            }
            if (static_cast<size_t>(std::max(word_col, trans_col)) >= cols.size()) {
                continue;
            }
            std::string rs = (src_lang_col >= 0 && static_cast<size_t>(src_lang_col) < cols.size())
                                 ? canonicalDictLang(cols[src_lang_col])
                                 : eff_src;
            std::string rt = (tgt_lang_col >= 0 && static_cast<size_t>(tgt_lang_col) < cols.size())
                                 ? canonicalDictLang(cols[tgt_lang_col])
                                 : eff_tgt;
            sink.addAutoOrTypedEntry(cols[word_col], cols[trans_col], rs, rt,
                                     ecdict_mode, /*allow_reverse=*/!ecdict_mode);
            continue;
        }

        // TSV / TXT
        if (trimmed.find('\t') != std::string::npos) {
            std::vector<std::string_view> cols;
            size_t p = 0;
            while (p <= trimmed.size()) {
                size_t tab = trimmed.find('\t', p);
                if (tab == std::string::npos) {
                    cols.push_back(shadowbar::utf8::trimView(std::string_view(trimmed).substr(p)));
                    break;
                }
                cols.push_back(shadowbar::utf8::trimView(std::string_view(trimmed).substr(p, tab - p)));
                p = tab + 1;
            }
            if (!checked_header) {
                checked_header = true;
                int w = -1, t = -1, sc = -1, tc = -1;
                std::string hs, ht;
                bool has_hdr_keyword = false;
                for (size_t i = 0; i < cols.size(); ++i) {
                    std::string col = toLowerAscii(cols[i]);
                    std::string lang_tok;
                    if (col == "word" || col == "src" || col == "source" ||
                        col == "headword" || col == "term" || col == "query") {
                        w = static_cast<int>(i);
                        has_hdr_keyword = true;
                    } else if (col == "translation" || col == "trans" || col == "tgt" ||
                               col == "target" || col == "meaning" || col == "def" ||
                               col == "definition") {
                        t = static_cast<int>(i);
                        has_hdr_keyword = true;
                    } else if (col == "source_lang" || col == "src_lang" || col == "from") {
                        sc = static_cast<int>(i);
                        has_hdr_keyword = true;
                    } else if (col == "target_lang" || col == "tgt_lang" || col == "to") {
                        tc = static_cast<int>(i);
                        has_hdr_keyword = true;
                    } else if (matchLangToken(col, lang_tok)) {
                        if (w < 0) {
                            w = static_cast<int>(i);
                            hs = lang_tok;
                        } else if (t < 0) {
                            t = static_cast<int>(i);
                            ht = lang_tok;
                        }
                        has_hdr_keyword = true;
                    }
                }
                if (has_hdr_keyword && w >= 0 && t >= 0) {
                    word_col = w;
                    trans_col = t;
                    src_lang_col = sc;
                    tgt_lang_col = tc;
                    if (eff_src == "auto" && !hs.empty()) eff_src = hs;
                    if (eff_tgt == "auto" && !ht.empty()) eff_tgt = ht;
                    continue;
                }
            }
            const size_t wc = (word_col >= 0) ? static_cast<size_t>(word_col) : 0;
            const size_t tc_idx = (trans_col >= 0) ? static_cast<size_t>(trans_col) : 1;
            if (cols.size() <= std::max(wc, tc_idx)) {
                continue;
            }
            std::string rs = (src_lang_col >= 0 && static_cast<size_t>(src_lang_col) < cols.size())
                                 ? canonicalDictLang(cols[src_lang_col])
                                 : eff_src;
            std::string rt = (tgt_lang_col >= 0 && static_cast<size_t>(tgt_lang_col) < cols.size())
                                 ? canonicalDictLang(cols[tgt_lang_col])
                                 : eff_tgt;
            sink.addAutoOrTypedEntry(cols[wc], cols[tc_idx], rs, rt,
                                     /*ecdict_csv_mode=*/false, /*allow_reverse=*/true);
            continue;
        }

        checked_header = true;
        std::string_view src_sv;
        std::string_view tgt_sv;
        for (std::string_view sep : {" ::: ", " -> ", " => ", "="}) {
            size_t p = trimmed.find(sep);
            if (p != std::string::npos) {
                src_sv = shadowbar::utf8::trimView(std::string_view(trimmed).substr(0, p));
                tgt_sv = shadowbar::utf8::trimView(std::string_view(trimmed).substr(p + sep.size()));
                break;
            }
        }
        if (!src_sv.empty() && !tgt_sv.empty()) {
            sink.addAutoOrTypedEntry(src_sv, tgt_sv, eff_src, eff_tgt,
                                     /*ecdict_csv_mode=*/false, /*allow_reverse=*/true);
        }
    }

    finalizeReportPairs(sink, eff_src, eff_tgt, report);
    report.entry_count = sink.inserted_count;
    report.loaded = (sink.inserted_count > 0);
    if (!report.loaded && report.message.empty()) {
        report.message = "文件中未解析到有效词条";
    }
}

DictSourceReport processExternalDictSpec(const ExternalDictSpec &spec,
                                         bool inspect_only,
                                         DictSink &sink) {
    namespace fs = std::filesystem;
    DictSourceReport report;
    std::string expanded = expandUserPath(spec.path);
    report.path = expanded;
    std::string hint_src = canonicalDictLang(spec.source_lang);
    std::string hint_tgt = canonicalDictLang(spec.target_lang);
    if (hint_src == "auto" || hint_tgt == "auto") {
        std::string p_src = "auto", p_tgt = "auto";
        inferLangPairFromPath(expanded, p_src, p_tgt);
        if (hint_src == "auto") hint_src = p_src;
        if (hint_tgt == "auto") hint_tgt = p_tgt;
    }
    report.source_lang = hint_src;
    report.target_lang = hint_tgt;

    std::error_code ec;
    if (!fs::exists(expanded, ec)) {
        report.message = "词典文件不存在";
        return report;
    }
    if (isSqliteExtension(expanded)) {
        processSqliteDictSpec(expanded, hint_src, hint_tgt, inspect_only, sink, report);
    } else if (hasExtension(expanded, {".ifo", ".idx", ".idx.gz", ".dict", ".dict.dz", ".dict.gz"})) {
        processStarDictSpec(expanded, hint_src, hint_tgt, inspect_only, sink, report);
    } else if (hasExtension(expanded, {".json"})) {
        processJsonDictSpec(expanded, hint_src, hint_tgt, /*is_jsonl=*/false, sink, report);
    } else if (hasExtension(expanded, {".jsonl", ".jsonl.gz"})) {
        processJsonDictSpec(expanded, hint_src, hint_tgt, /*is_jsonl=*/true, sink, report);
    } else {
        processDelimitedDictSpec(expanded, hint_src, hint_tgt, sink, report);
    }
    return report;
}

} // namespace

OfflineDictionary::OfflineDictionary(std::string_view custom_path) {
    reload(custom_path);
}

OfflineDictionary::~OfflineDictionary() {
    std::lock_guard<std::mutex> db_lock(db_mu_);
    closeSqlite();
}

void OfflineDictionary::closeSqlite() {
#ifdef SHADOWBAR_HAS_SQLITE3
    for (auto &h : sqlite_handles_) {
        if (h.stmt_ci) {
            sqlite3_finalize(h.stmt_ci);
            h.stmt_ci = nullptr;
        }
        if (h.stmt_cs) {
            sqlite3_finalize(h.stmt_cs);
            h.stmt_cs = nullptr;
        }
        if (h.db) {
            sqlite3_close_v2(h.db);
            h.db = nullptr;
        }
    }
#endif
    sqlite_handles_.clear();
}

std::string OfflineDictionary::defaultManagedDictDir() {
    return defaultManagedDictDirPath();
}

DictSourceReport OfflineDictionary::inspectFile(std::string_view path,
                                                std::string_view hint_source_lang,
                                                std::string_view hint_target_lang) {
    ExternalDictSpec spec;
    spec.path = std::string(path);
    spec.source_lang =
        hint_source_lang.empty() ? std::string("auto") : std::string(hint_source_lang);
    spec.target_lang =
        hint_target_lang.empty() ? std::string("auto") : std::string(hint_target_lang);

    DictSink sink;
    sink.inspect_only = true;
    return processExternalDictSpec(spec, /*inspect_only=*/true, sink);
}

bool OfflineDictionary::copyToManagedDir(std::string_view src_path,
                                         std::string &out_managed_path,
                                         std::string *error) {
    namespace fs = std::filesystem;
    std::string expanded = expandUserPath(src_path);
    if (expanded.empty()) {
        if (error) *error = "源词典路径为空";
        return false;
    }
    std::error_code ec;
    fs::path src(expanded);
    if (!fs::exists(src, ec) || !fs::is_regular_file(src, ec)) {
        if (error) *error = "源词典文件不存在: " + expanded;
        return false;
    }

    const fs::path managed_dir(defaultManagedDictDirPath());
    fs::create_directories(managed_dir, ec);
    if (ec) {
        if (error) *error = "创建托管词典目录失败: " + ec.message();
        return false;
    }

    auto canonicalOrLex = [](const fs::path &p) -> fs::path {
        std::error_code c_ec;
        fs::path c = fs::weakly_canonical(p, c_ec);
        return c_ec ? p.lexically_normal() : c;
    };

    const fs::path canon_src_dir = canonicalOrLex(src.parent_path());
    const fs::path canon_mgr_dir = canonicalOrLex(managed_dir);
    if (canon_src_dir == canon_mgr_dir) {
        out_managed_path = canonicalOrLex(src).string();
        return true;
    }

    fs::path dst = managed_dir / src.filename();
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error) *error = "复制词典文件失败: " + ec.message();
        return false;
    }

    // Copy companion StarDict data files (.idx, .dict, .dict.dz, .syn) alongside .ifo.
    std::string lower_name = toLowerAscii(src.filename().string());
    if (lower_name.size() >= 4 &&
        lower_name.compare(lower_name.size() - 4, 4, ".ifo") == 0) {
        fs::path src_stem = src;
        src_stem.replace_extension();
        for (const char *ext : {".idx", ".idx.gz", ".dict", ".dict.dz", ".dict.gz", ".syn"}) {
            fs::path comp_src(src_stem.string() + ext);
            if (fs::exists(comp_src, ec) && fs::is_regular_file(comp_src, ec)) {
                fs::path comp_dst = managed_dir / comp_src.filename();
                std::error_code c_ec;
                fs::copy_file(comp_src, comp_dst, fs::copy_options::overwrite_existing, c_ec);
            }
        }
    }

    out_managed_path = canonicalOrLex(dst).string();
    return true;
}

DictSourceReport OfflineDictionary::loadExternalSpec(const ExternalDictSpec &spec) {
    const auto &builtin = builtinDictIndex();
    DictSink sink;
    sink.zh_to_en = &custom_zh_to_en_;
    sink.en_to_zh = &custom_en_to_zh_;
    sink.pair_maps = &pair_maps_;
    sink.max_zh_cp = &custom_max_zh_cp_;
    sink.max_en_words = &custom_max_en_words_;
    sink.builtin_zh_to_en = &builtin.zh_to_en;
    sink.builtin_en_to_zh = &builtin.en_to_zh;
    sink.sqlite_handles = &sqlite_handles_;
    sink.inspect_only = false;

    DictSourceReport report = processExternalDictSpec(spec, /*inspect_only=*/false, sink);
    if (report.loaded) {
        loaded_sources_.push_back(report.path);
    }
    return report;
}

void OfflineDictionary::reload(std::string_view custom_path) {
    std::vector<ExternalDictSpec> specs;
    if (!shadowbar::utf8::trimView(custom_path).empty()) {
        specs.push_back(ExternalDictSpec{
            .id = "custom",
            .path = std::string(custom_path),
            .source_lang = "auto",
            .target_lang = "auto",
            .enabled = true,
            .label = "",
        });
    }
    reload(specs);
}

void OfflineDictionary::reload(const std::vector<ExternalDictSpec> &specs) {
    namespace fs = std::filesystem;

    struct Candidate {
        ExternalDictSpec spec;
        bool is_default = false;
    };

    std::vector<ExternalDictSpec> explicit_specs;
    std::unordered_set<std::string> explicit_paths;
    for (const auto &raw : specs) {
        if (!raw.enabled || shadowbar::utf8::trimView(raw.path).empty()) {
            continue;
        }
        ExternalDictSpec clean = raw;
        clean.path = expandUserPath(raw.path);
        clean.source_lang = canonicalDictLang(raw.source_lang);
        clean.target_lang = canonicalDictLang(raw.target_lang);
        explicit_paths.insert(clean.path);
        explicit_specs.push_back(std::move(clean));
    }

    std::vector<Candidate> candidates;
    for (const std::string &dp : defaultSearchPaths()) {
        if (explicit_paths.count(dp)) {
            continue;
        }
        candidates.push_back(Candidate{
            .spec = ExternalDictSpec{
                .id = "default",
                .path = dp,
                .source_lang = "auto",
                .target_lang = "auto",
                .enabled = true,
                .label = "",
            },
            .is_default = true,
        });
    }
    for (auto &es : explicit_specs) {
        candidates.push_back(Candidate{.spec = es, .is_default = false});
    }

    std::ostringstream sig;
    sig << defaultManagedDictDirPath() << '|';
    std::vector<Candidate> active_candidates;
    std::error_code ec;
    for (const auto &c : candidates) {
        if (fs::exists(c.spec.path, ec) && fs::is_regular_file(c.spec.path, ec)) {
            auto fsize = fs::file_size(c.spec.path, ec);
            auto mtime = fs::last_write_time(c.spec.path, ec).time_since_epoch().count();
            sig << c.spec.path << ':' << c.spec.source_lang << "->" << c.spec.target_lang
                << ':' << fsize << ':' << mtime << ';';
            active_candidates.push_back(c);
        } else if (!c.is_default) {
            sig << c.spec.path << ":missing;";
            active_candidates.push_back(c);
        }
    }
    const std::string new_sig = sig.str();

    {
        std::shared_lock<std::shared_mutex> rlock(rw_mu_);
        if (new_sig == last_load_signature_) {
            return;
        }
    }

    std::unique_lock<std::shared_mutex> wlock(rw_mu_);
    if (new_sig == last_load_signature_) {
        return;
    }

    custom_path_ = explicit_specs.empty() ? std::string() : explicit_specs.back().path;
    last_load_signature_ = new_sig;
    custom_zh_to_en_.clear();
    custom_en_to_zh_.clear();
    pair_maps_.clear();
    custom_max_zh_cp_ = 0;
    custom_max_en_words_ = 0;
    loaded_sources_.clear();
    source_reports_.clear();

    {
        std::lock_guard<std::mutex> db_lock(db_mu_);
        closeSqlite();
    }

    std::vector<ExternalDictSpec> sqlite_specs;
    for (const auto &c : active_candidates) {
        if (isSqliteExtension(c.spec.path)) {
            sqlite_specs.push_back(c.spec);
        } else {
            source_reports_.push_back(loadExternalSpec(c.spec));
        }
    }
    if (!sqlite_specs.empty()) {
        std::lock_guard<std::mutex> db_lock(db_mu_);
        for (const auto &ss : sqlite_specs) {
            source_reports_.push_back(loadExternalSpec(ss));
        }
    }
}

namespace {

#ifdef SHADOWBAR_HAS_SQLITE3
bool stepCachedSqliteQuery(sqlite3_stmt *stmt,
                           std::string_view key,
                           int script_mode, // 1=want CJK, 0=want ASCII alpha, -1=any
                           bool sentence_mode,
                           std::string &out) {
    if (!stmt || key.empty()) {
        return false;
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_text(stmt, 1, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *txt = sqlite3_column_text(stmt, 0);
        if (txt) {
            std::string_view raw(reinterpret_cast<const char *>(txt));
            const bool script_ok = (script_mode < 0)
                                       ? !shadowbar::utf8::trimView(raw).empty()
                                       : (script_mode == 1 ? containsCjk(raw) : containsAsciiAlpha(raw));
            if (script_ok) {
                std::string cleaned = cleanExternalGloss(raw, sentence_mode);
                if (!cleaned.empty()) {
                    out = std::move(cleaned);
                    found = true;
                }
            }
        }
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return found;
}
#endif

} // namespace

bool OfflineDictionary::querySqliteEnToZh(std::string_view en_lower,
                                          bool sentence_mode,
                                          std::string &out) const {
    return querySqliteForPair(en_lower, "en", "zh", sentence_mode, out);
}

bool OfflineDictionary::querySqliteZhToEn(std::string_view zh,
                                          bool sentence_mode,
                                          std::string &out) const {
    return querySqliteForPair(zh, "zh", "en", sentence_mode, out);
}

bool OfflineDictionary::querySqliteForPair(std::string_view key,
                                           std::string_view src_lang,
                                           std::string_view tgt_lang,
                                           bool sentence_mode,
                                           std::string &out) const {
#ifdef SHADOWBAR_HAS_SQLITE3
    std::lock_guard<std::mutex> lock(db_mu_);
    if (sqlite_handles_.empty() || key.empty()) {
        return false;
    }
    const std::string c_src = canonicalDictLang(src_lang);
    const std::string c_tgt = canonicalDictLang(tgt_lang);
    const int script_mode =
        (c_src == "en" && c_tgt == "zh") ? 1 : ((c_src == "zh" && c_tgt == "en") ? 0 : -1);

    // Search newest / highest-priority explicit handles first.
    for (auto it = sqlite_handles_.rbegin(); it != sqlite_handles_.rend(); ++it) {
        const auto &h = *it;
        const bool src_match = (h.source_lang == "auto" || c_src == "auto" || h.source_lang == c_src);
        const bool tgt_match = (h.target_lang == "auto" || c_tgt == "auto" || h.target_lang == c_tgt);
        if (!src_match || !tgt_match) {
            continue;
        }
        if (script_mode == 0 && !h.has_non_ascii_keys) {
            continue;
        }
        if (h.stmt_cs &&
            stepCachedSqliteQuery(h.stmt_cs, key, script_mode, sentence_mode, out)) {
            return true;
        }
        if (h.stmt_ci &&
            stepCachedSqliteQuery(h.stmt_ci, key, script_mode, sentence_mode, out)) {
            return true;
        }
    }
    return false;
#else
    (void)key;
    (void)src_lang;
    (void)tgt_lang;
    (void)sentence_mode;
    (void)out;
    return false;
#endif
}

bool OfflineDictionary::lookupExactZhToEn(std::string_view zh,
                                          bool sentence_mode,
                                          std::string &out,
                                          bool *out_from_builtin) const {
    const auto &builtin = builtinDictIndex();
    if (auto it = custom_zh_to_en_.find(zh); it != custom_zh_to_en_.end()) {
        out = cleanExternalGloss(it->second, sentence_mode);
        if (out_from_builtin) {
            *out_from_builtin = (builtin.zh_to_en.find(zh) != builtin.zh_to_en.end());
        }
        return !out.empty() || sentence_mode;
    }
    if (auto it = builtin.zh_to_en.find(zh); it != builtin.zh_to_en.end()) {
        if (out_from_builtin) {
            *out_from_builtin = true;
        }
        if (it->second.empty()) {
            out.clear();
            return sentence_mode;
        }
        out = it->second;
        return true;
    }
    if (out_from_builtin) {
        *out_from_builtin = false;
    }
    return querySqliteZhToEn(zh, sentence_mode, out);
}

bool OfflineDictionary::lookupExactEnToZh(std::string_view en,
                                          bool sentence_mode,
                                          std::string &out,
                                          bool *out_from_builtin) const {
    std::string lower = toLowerAscii(normalizeAsciiSpaces(en));
    if (lower.empty()) {
        return false;
    }
    const auto &builtin = builtinDictIndex();
    if (auto it = custom_en_to_zh_.find(std::string_view(lower));
        it != custom_en_to_zh_.end()) {
        out = cleanExternalGloss(it->second, sentence_mode);
        if (out_from_builtin) {
            *out_from_builtin =
                (builtin.en_to_zh.find(std::string_view(lower)) != builtin.en_to_zh.end());
        }
        return !out.empty();
    }
    if (auto it = builtin.en_to_zh.find(std::string_view(lower));
        it != builtin.en_to_zh.end()) {
        if (out_from_builtin) {
            *out_from_builtin = true;
        }
        out = it->second;
        return !out.empty();
    }
    if (out_from_builtin) {
        *out_from_builtin = false;
    }
    return querySqliteEnToZh(lower, sentence_mode, out);
}

namespace {

bool isZhFunctionChar(std::string_view ch) {
    static constexpr std::array<std::string_view, 55> kFuncs = {
        "的", "地", "得", "了", "着", "过", "吗", "呢", "吧", "啊",
        "和", "与", "或", "并", "但", "而", "且", "及", "之", "于",
        "在", "比", "被", "把", "从", "到", "对", "向", "给", "让",
        "使", "将", "能", "会", "想", "要", "可", "很", "太", "更",
        "较", "最", "都", "也", "只", "就", "才", "还", "已", "未",
        "正", "不", "没", "中", "里",
    };
    for (std::string_view f : kFuncs) {
        if (ch == f) {
            return true;
        }
    }
    return false;
}

bool endsWithZhParticleSuffix(std::string_view slice) {
    return slice.ends_with("的") || slice.ends_with("了") ||
           slice.ends_with("着") || slice.ends_with("得") ||
           slice.ends_with("地") || slice.ends_with("吗") ||
           slice.ends_with("呢") || slice.ends_with("吧");
}

int scoreZhSegmentStep(std::string_view slice,
                       size_t cp_len,
                       bool matched,
                       bool is_builtin) {
    if (!matched) {
        return 1000;
    }
    if (cp_len <= 1) {
        if (is_builtin) {
            return isZhFunctionChar(slice) ? 95 : 105;
        }
        return 145;
    }
    int cost = 90 - std::min<int>(static_cast<int>(cp_len) - 2, 6) * 4;
    if (is_builtin) {
        cost -= 18;
    } else if (cp_len == 2 && endsWithZhParticleSuffix(slice)) {
        cost += 42;
    }
    return cost;
}

struct ZhSegTok {
    enum Kind { Word, Tech, Punct } kind = Word;
    std::string zh;
    std::string en;
    bool matched = false;
    bool is_builtin = false;
    bool is_comparative = false;
};

bool isZhCommonVerbToken(std::string_view zh) {
    static constexpr std::string_view kVerbs[] = {
        "修改", "删除", "保存", "加载", "重新加载", "安装", "卸载", "更新",
        "升级", "重启", "重新启动", "启动", "打开", "关闭", "出现", "收到",
        "找到", "发现", "发生", "发送", "发给", "发送给", "发给我", "发给你",
        "给我", "排查", "检查", "设置", "配置", "翻译", "测试", "运行",
        "执行", "连接", "断开", "拒绝", "允许", "禁用", "启用", "调用",
        "使用", "用", "看", "查看", "写", "写入", "读", "读取",
        "做", "复制", "粘贴", "移动", "重置", "清理", "清除", "解决",
        "修复", "优化", "改进", "支持", "创建", "生成", "编译", "构建",
        "加快", "加速", "开始", "准备", "继续", "想要", "希望", "需要",
        "可以", "能够", "应该", "必须", "获得", "提供", "建设",
    };
    for (std::string_view v : kVerbs) {
        if (zh == v) {
            return true;
        }
    }
    return false;
}

bool isZhModifierForDe(const ZhSegTok &t) {
    if (t.zh.rfind("越来越", 0) == 0) {
        return true;
    }
    static constexpr std::string_view kModZh[] = {
        "快速", "本地", "离线", "在线", "默认", "常见", "主要", "重要",
        "实际", "具体", "当前", "最新", "不同", "相同", "完整", "简单",
        "复杂", "流畅", "稳定", "安全", "免费", "开源", "普通", "容易",
        "方便", "越来越多", "更多", "更少", "更好", "更快", "更长", "更大",
        "更小", "长", "短", "大", "小", "多", "少", "好",
        "坏", "快", "慢", "新", "旧", "高", "低", "强",
        "弱", "正常", "正确", "错误", "有效", "无效", "所有", "全部",
    };
    for (std::string_view m : kModZh) {
        if (t.zh == m) {
            return true;
        }
    }
    if (t.zh == "其他" || t.zh == "相关" || t.zh == "详细" ||
        t.zh == "较长" || t.zh == "较快" || t.zh == "较好" ||
        t.zh == "较多" || t.zh == "较少" || t.zh == "较小" ||
        t.zh == "较大" || t.zh == "很好" || t.zh == "很快" ||
        t.zh == "很多" || t.zh == "非常" || t.zh == "较") {
        return true;
    }
    std::string low = toLowerAscii(t.en);
    static constexpr std::string_view kAdjSuffix[] = {
        "ic", "al", "ive", "ous", "ful", "less", "able", "ible", "ent", "ant", "ary", "ory",
    };
    for (std::string_view sfx : kAdjSuffix) {
        if (low.size() > sfx.size() + 2 && low.ends_with(sfx)) {
            return true;
        }
    }
    static constexpr std::string_view kAdjWords[] = {
        "fast", "quick", "long", "short", "large", "small", "good", "bad",
        "new", "old", "high", "low", "local", "remote", "simple", "complex",
        "easy", "hard",
    };
    for (std::string_view w : kAdjWords) {
        if (low == w) {
            return true;
        }
    }
    return false;
}

bool isClauseIntroducerOrPrep(const ZhSegTok &t) {
    if (t.kind == ZhSegTok::Punct) {
        return true;
    }
    static constexpr std::string_view kStops[] = {
        "如果", "假如", "因为", "所以", "因此", "虽然", "尽管", "但是",
        "然而", "不过", "而且", "并且", "随着", "通过", "根据", "基于",
        "关于", "对于", "为了", "在", "从", "到", "对", "向", "比",
        "时", "的时候",
    };
    for (std::string_view s : kStops) {
        if (t.zh == s) {
            return true;
        }
    }
    return t.zh == "是" || t.zh == "有" || t.zh == "被" || t.zh == "把" ||
           t.zh == "请" || isZhCommonVerbToken(t.zh);
}

bool isZhDeN2Stop(const ZhSegTok &t, bool after_head) {
    if (isClauseIntroducerOrPrep(t) || t.zh == "的" || t.is_comparative) {
        return true;
    }
    if (!after_head) {
        return false;
    }
    return t.zh == "更" || t.zh.rfind("更", 0) == 0 ||
           t.zh.rfind("最", 0) == 0 || t.zh.rfind("越来越", 0) == 0 ||
           t.zh == "非常" || t.zh == "很" || t.zh == "太" ||
           t.zh == "真" || t.zh == "真的" || t.zh == "也" || t.zh == "都";
}

std::string toPastParticipleEn(std::string_view verb) {
    std::string v = shadowbar::utf8::trim(verb);
    if (v.empty() || v.find(' ') != std::string_view::npos) {
        return v;
    }
    if (v == "write") return "written";
    if (v == "read") return "read";
    if (v == "set" || v == "reset" || v == "cut" || v == "put") return v;
    if (v == "send") return "sent";
    if (v == "build") return "built";
    if (v == "find") return "found";
    if (v == "keep") return "kept";
    if (v == "make") return "made";
    if (v == "get") return "gotten";
    if (v == "break") return "broken";
    if (v == "run") return "run";
    if (v.ends_with("e")) {
        return v + "d";
    }
    if (v.size() >= 2 && v.back() == 'y') {
        char prev = static_cast<char>(std::tolower(static_cast<unsigned char>(v[v.size() - 2])));
        if (prev != 'a' && prev != 'e' && prev != 'i' && prev != 'o' && prev != 'u') {
            return v.substr(0, v.size() - 1) + "ied";
        }
    }
    return v + "ed";
}

std::string toComparativeEn(std::string_view word) {
    std::string w = shadowbar::utf8::trim(word);
    if (w == "good" || w == "well") return "better";
    if (w == "bad") return "worse";
    if (w == "fast") return "faster";
    if (w == "slow") return "slower";
    if (w == "long") return "longer";
    if (w == "short") return "shorter";
    if (w == "high") return "higher";
    if (w == "low") return "lower";
    if (w == "large") return "larger";
    if (w == "big") return "bigger";
    if (w == "small") return "smaller";
    if (w == "new") return "newer";
    if (w == "old") return "older";
    if (w == "easy") return "easier";
    if (w == "hard") return "harder";
    if (w == "many" || w == "much" || w == "more") return "more";
    if (w.rfind("more ", 0) == 0 || w.ends_with("er")) {
        return w;
    }
    return "more " + w;
}

bool isZhClassifier(std::string_view zh) {
    static constexpr std::array<std::string_view, 12> kClfs = {
        "个", "段", "条", "种", "份", "套", "款", "台", "张", "篇", "项", "次",
    };
    for (std::string_view c : kClfs) {
        if (zh == c) {
            return true;
        }
    }
    return false;
}

bool isZhNumericPrefix(std::string_view zh) {
    static constexpr std::array<std::string_view, 11> kNums = {
        "一", "二", "两", "三", "四", "五", "六", "七", "八", "九", "十",
    };
    for (std::string_view n : kNums) {
        if (zh == n) {
            return true;
        }
    }
    return zh == "几" || zh == "多";
}

bool isZhLocativePostposition(std::string_view zh) {
    return zh == "中" || zh == "里" || zh == "里面" ||
           zh == "内" || zh == "上" || zh == "下";
}

bool isObjectContextEn(std::string_view en) {
    static constexpr std::array<std::string_view, 17> kTriggers = {
        "to", "for", "with", "from", "by", "in", "on", "at", "than", "about",
        "help", "send", "tell", "ask", "give", "let", "check",
    };
    std::string low = toLowerAscii(shadowbar::utf8::trimView(en));
    for (std::string_view t : kTriggers) {
        if (low == t || low.ends_with(" to")) {
            return true;
        }
    }
    return false;
}

template <typename Tok, typename KeepPred>
void eraseMatchedEmptyIf(std::vector<Tok> &clause,
                         std::string Tok::*out_field,
                         KeepPred &&keep) {
    clause.erase(
        std::remove_if(clause.begin(), clause.end(),
                       [out_field, &keep](const Tok &t) {
                           return t.kind == Tok::Word && t.matched &&
                                  (t.*out_field).empty() && !keep(t);
                       }),
        clause.end());
}

template <typename Tok>
void eraseMatchedEmpty(std::vector<Tok> &clause, std::string Tok::*out_field) {
    eraseMatchedEmptyIf(clause, out_field, [](const Tok &) { return false; });
}

void rewriteZhClause(std::vector<ZhSegTok> &clause, bool has_trailing_qmark) {
    if (clause.empty()) {
        return;
    }

    // 1. Sentence-clean verb infinitives, quantifiers, and particle elisions.
    for (size_t i = 0; i < clause.size(); ++i) {
        ZhSegTok &t = clause[i];
        if (t.kind != ZhSegTok::Word) {
            continue;
        }
        if (!t.is_builtin && t.en.rfind("to ", 0) == 0 && t.en.size() > 3) {
            t.en.erase(0, 3);
        }
        if (t.zh == "比") {
            t.en = "than";
        } else if (t.zh == "较") {
            t.en = "relatively";
        } else if (t.zh == "并") {
            t.en = "and";
        } else if (t.zh == "该" && i + 1 < clause.size() &&
                   clause[i + 1].kind == ZhSegTok::Word) {
            t.en = "this";
        } else if (t.zh == "越来越" && i + 1 < clause.size() &&
                   clause[i + 1].kind == ZhSegTok::Word) {
            const std::string_view next_zh = clause[i + 1].zh;
            if (next_zh == "多") {
                t.en = "more and more";
                t.is_comparative = true;
                clause[i + 1].en.clear();
            } else if (next_zh == "快" || next_zh == "好" || next_zh == "大" ||
                       next_zh == "小" || next_zh == "高" || next_zh == "低" ||
                       next_zh == "慢" || isZhModifierForDe(clause[i + 1])) {
                std::string comp = toComparativeEn(clause[i + 1].en);
                if (comp.rfind("more ", 0) == 0 && comp.size() > 5) {
                    t.en = "more and more " + comp.substr(5);
                } else {
                    t.en = comp + " and " + comp;
                }
                t.is_comparative = true;
                clause[i + 1].en.clear();
            }
        }
    }

    // Collapse "一 + classifier" -> "a", number + "个" -> number,
    // and strip trailing softeners/aspect particles.
    for (size_t i = 0; i < clause.size(); ++i) {
        if (clause[i].kind != ZhSegTok::Word) {
            continue;
        }
        if (i + 1 < clause.size() && clause[i + 1].kind == ZhSegTok::Word) {
            if (clause[i].zh == "一" && isZhClassifier(clause[i + 1].zh)) {
                clause[i].en = (clause[i + 1].zh == "次") ? "once" : "a";
                clause[i + 1].en.clear();
            } else if (isZhNumericPrefix(clause[i].zh) &&
                       clause[i + 1].zh == "个") {
                clause[i + 1].en.clear();
            }
        }
        if ((clause[i].zh == "吗" || clause[i].zh == "呢") && has_trailing_qmark) {
            clause[i].en.clear();
        }
        if (clause[i].zh == "得" || clause[i].zh == "地" ||
            clause[i].zh == "着" || clause[i].zh == "过" ||
            clause[i].zh == "的话") {
            clause[i].en.clear();
        }
        if (clause[i].zh == "一下" && i > 0 &&
            isZhCommonVerbToken(clause[i - 1].zh)) {
            clause[i].en.clear();
        }
    }

    auto keep_ba = [](const ZhSegTok &t) { return t.zh == "把"; };
    eraseMatchedEmptyIf(clause, &ZhSegTok::en, keep_ba);

    // 2. Locative circumpositions: "在 <span 1..6> 中/里/里面/内/上/下"
    for (size_t i = 0; i < clause.size(); ++i) {
        if (clause[i].zh != "在") {
            continue;
        }
        size_t limit = std::min(clause.size(), i + 8);
        for (size_t j = i + 1; j < limit; ++j) {
            if (clause[j].zh == "在") {
                break;
            }
            if (isZhLocativePostposition(clause[j].zh)) {
                clause[i].en = (clause[j].zh == "上")   ? "on"
                             : (clause[j].zh == "下")   ? "under"
                                                        : "in";
                clause[j].en.clear();
                break;
            }
        }
    }
    eraseMatchedEmptyIf(clause, &ZhSegTok::en, keep_ba);

    // Progressive/state "<Verb> 中" without leading "在"
    for (size_t i = 1; i < clause.size(); ++i) {
        if (clause[i].zh == "中" && clause[i - 1].kind == ZhSegTok::Word) {
            if (isZhCommonVerbToken(clause[i - 1].zh)) {
                clause[i].en = "in progress";
            } else {
                clause[i].en = "within";
            }
        }
    }

    // Clause-final "... 时" / "... 的时候" -> prepend "when"
    if (clause.size() >= 2 &&
        (clause.back().zh == "时" || clause.back().zh == "的时候")) {
        clause.pop_back();
        if (!clause.empty()) {
            size_t insert_at = 0;
            if (clause[0].zh == "如果" || clause[0].zh == "因为" ||
                clause[0].zh == "虽然" || clause[0].zh == "但是" ||
                clause[0].zh == "所以") {
                insert_at = 1;
            }
            if (insert_at < clause.size() && clause[insert_at].zh == "当") {
                clause[insert_at].en = "when";
            } else {
                clause.insert(
                    clause.begin() + static_cast<std::ptrdiff_t>(insert_at),
                    ZhSegTok{.kind = ZhSegTok::Word,
                             .zh = "当",
                             .en = "when",
                             .matched = true,
                             .is_builtin = true});
            }
        }
    }

    // 3. Negation "没有 + Verb" & Passive "被 + Verb"
    for (size_t i = 0; i < clause.size(); ++i) {
        if (clause[i].zh == "没有" && i + 1 < clause.size()) {
            if (isZhCommonVerbToken(clause[i + 1].zh)) {
                if (i > 0 && clause[i - 1].zh == "还") {
                    clause[i].en = "have not";
                } else {
                    clause[i].en = "did not";
                }
            } else {
                clause[i].en = "no";
            }
        } else if (clause[i].zh == "被" && i + 1 < clause.size() &&
                   isZhCommonVerbToken(clause[i + 1].zh)) {
            if (i > 0 && clause[i - 1].zh == "已经") {
                clause[i - 1].en = "has already";
                clause[i].en = "been";
            } else {
                clause[i].en = "is";
            }
            clause[i + 1].en = toPastParticipleEn(clause[i + 1].en);
        }
    }

    // 4. Dispositive "把 <Obj 1..3> <Verb>" -> "<Verb> <Obj>"
    for (size_t i = 0; i < clause.size(); ++i) {
        if (clause[i].zh != "把") {
            continue;
        }
        size_t limit = std::min(clause.size(), i + 5);
        for (size_t j = i + 1; j < limit; ++j) {
            if (isZhCommonVerbToken(clause[j].zh)) {
                ZhSegTok verb_tok = std::move(clause[j]);
                std::string recipient_tail;
                for (std::string_view sfx : {" to me", " to you", " to him", " to her", " to them"}) {
                    if (verb_tok.en.size() > sfx.size() && verb_tok.en.ends_with(sfx)) {
                        recipient_tail.assign(sfx.substr(1));
                        verb_tok.en.erase(verb_tok.en.size() - sfx.size());
                        break;
                    }
                }
                clause.erase(clause.begin() + static_cast<std::ptrdiff_t>(j));
                clause[i] = std::move(verb_tok);
                if (!recipient_tail.empty()) {
                    clause.insert(clause.begin() + static_cast<std::ptrdiff_t>(j),
                                  ZhSegTok{.kind = ZhSegTok::Word,
                                           .zh = {},
                                           .en = std::move(recipient_tail),
                                           .matched = true,
                                           .is_builtin = true});
                }
                break;
            }
        }
    }

    // 5. Comparative "比 <NP 1..3> [更] <Adj>" -> "<Adj-er> than <NP>"
    for (size_t i = 0; i < clause.size(); ++i) {
        if (clause[i].zh != "比") {
            continue;
        }
        size_t limit = std::min(clause.size(), i + 5);
        for (size_t j = i + 2; j < limit; ++j) {
            size_t adj_idx = std::string::npos;
            size_t erase_end = 0;
            if (clause[j].zh == "更" && j + 1 < clause.size()) {
                adj_idx = j + 1;
                erase_end = j + 2;
            } else if (clause[j].zh.rfind("更", 0) == 0 &&
                       clause[j].zh.size() > 3 &&
                       isZhModifierForDe(clause[j])) {
                adj_idx = j;
                erase_end = j + 1;
            }
            if (adj_idx == std::string::npos) {
                continue;
            }
            std::vector<ZhSegTok> np(
                std::make_move_iterator(clause.begin() + static_cast<std::ptrdiff_t>(i + 1)),
                std::make_move_iterator(clause.begin() + static_cast<std::ptrdiff_t>(j)));
            ZhSegTok comp_adj = std::move(clause[adj_idx]);
            comp_adj.en = toComparativeEn(comp_adj.en);
            comp_adj.is_comparative = true;
            clause[i] = std::move(comp_adj);
            clause.erase(clause.begin() + static_cast<std::ptrdiff_t>(i + 1),
                         clause.begin() + static_cast<std::ptrdiff_t>(erase_end));
            size_t pos = i + 1;
            clause.insert(clause.begin() + static_cast<std::ptrdiff_t>(pos),
                          ZhSegTok{.kind = ZhSegTok::Word,
                                   .zh = "比",
                                   .en = "than",
                                   .matched = true,
                                   .is_builtin = true});
            clause.insert(clause.begin() + static_cast<std::ptrdiff_t>(pos + 1),
                          std::make_move_iterator(np.begin()),
                          std::make_move_iterator(np.end()));
            break;
        }
    }

    // 5b. Standalone "更 + Adj" -> "<Adj-er>" / "more <Adj>"
    for (size_t i = 0; i + 1 < clause.size(); ++i) {
        if (clause[i].zh == "更" && clause[i + 1].kind == ZhSegTok::Word &&
            !isZhCommonVerbToken(clause[i + 1].zh) &&
            isZhModifierForDe(clause[i + 1]) && !clause[i + 1].en.empty()) {
            clause[i + 1].en = toComparativeEn(clause[i + 1].en);
            clause[i + 1].is_comparative = true;
            clause[i].en.clear();
        }
    }
    eraseMatchedEmpty(clause, &ZhSegTok::en);

    // 6. Object pronoun case after verbs or prepositions ("to I" -> "to me", "help I" -> "help me")
    for (size_t i = 1; i < clause.size(); ++i) {
        if (clause[i].kind == ZhSegTok::Word && isObjectContextEn(clause[i - 1].en)) {
            if (clause[i].en == "I") clause[i].en = "me";
            else if (clause[i].en == "we") clause[i].en = "us";
            else if (clause[i].en == "he") clause[i].en = "him";
            else if (clause[i].en == "she") clause[i].en = "her";
            else if (clause[i].en == "they") clause[i].en = "them";
        }
    }

    // 7. Modifier / Possessive "的":
    // - Adjective / temporal "时" + "的" + Head -> elide "的"
    // - Noun1 (1..3) + "的" + Noun2 (1..2) -> Noun2 "of" Noun1 (or Noun2 + locative Noun1)
    for (size_t i = 0; i < clause.size(); ++i) {
        if (clause[i].zh != "的") {
            continue;
        }
        if (i == 0 || i + 1 >= clause.size()) {
            clause[i].en.clear();
            continue;
        }
        if (isZhModifierForDe(clause[i - 1]) ||
            isZhCommonVerbToken(clause[i - 1].zh) ||
            clause[i - 1].zh == "时" ||
            clause[i - 1].zh == "的时候") {
            clause[i].en.clear();
            continue;
        }
        size_t n1_start = i;
        while (n1_start > 0 && (i - n1_start) < 4) {
            const ZhSegTok &cand = clause[n1_start - 1];
            if (isClauseIntroducerOrPrep(cand) || cand.zh == "的") {
                break;
            }
            if (cand.zh == "我" || cand.zh == "你" || cand.zh == "他" ||
                cand.zh == "她" || cand.zh == "它" || cand.zh == "我们" ||
                cand.zh == "他们") {
                break;
            }
            --n1_start;
        }
        size_t n2_end = i + 1;
        while (n2_end < clause.size() && (n2_end - (i + 1)) < 2) {
            if (isZhDeN2Stop(clause[n2_end], n2_end > i + 1)) {
                break;
            }
            ++n2_end;
        }
        if (n1_start < i && n2_end > i + 1) {
            std::vector<ZhSegTok> n1(
                std::make_move_iterator(clause.begin() + static_cast<std::ptrdiff_t>(n1_start)),
                std::make_move_iterator(clause.begin() + static_cast<std::ptrdiff_t>(i)));
            std::vector<ZhSegTok> n2(
                std::make_move_iterator(clause.begin() + static_cast<std::ptrdiff_t>(i + 1)),
                std::make_move_iterator(clause.begin() + static_cast<std::ptrdiff_t>(n2_end)));
            const bool n1_locative =
                !n1.empty() &&
                (n1[0].en == "in" || n1[0].en == "on" || n1[0].en == "at" ||
                 n1[0].en.rfind("in ", 0) == 0);

            std::vector<ZhSegTok> rebuilt;
            rebuilt.reserve(n2.size() + 1 + n1.size());
            for (auto &t : n2) rebuilt.push_back(std::move(t));
            if (!n1_locative) {
                rebuilt.push_back(ZhSegTok{.kind = ZhSegTok::Word,
                                           .zh = "的",
                                           .en = "of",
                                           .matched = true,
                                           .is_builtin = true});
            }
            for (auto &t : n1) rebuilt.push_back(std::move(t));

            clause.erase(clause.begin() + static_cast<std::ptrdiff_t>(n1_start),
                         clause.begin() + static_cast<std::ptrdiff_t>(n2_end));
            clause.insert(clause.begin() + static_cast<std::ptrdiff_t>(n1_start),
                          std::make_move_iterator(rebuilt.begin()),
                          std::make_move_iterator(rebuilt.end()));
            i = n1_start + rebuilt.size() - 1;
        }
    }
    eraseMatchedEmpty(clause, &ZhSegTok::en);
}

struct EnSegTok {
    enum Kind { Word, Tech, Punct } kind = Word;
    std::string orig;
    std::string lower;
    std::string zh;
    bool matched = false;
    bool is_ed_past_form = false;
};

bool isEnPronounSubject(std::string_view lower) {
    return lower == "i" || lower == "we" || lower == "you" ||
           lower == "they" || lower == "he" || lower == "she" || lower == "it";
}

bool isEnTimeOrMathUnit(std::string_view lower) {
    return lower == "second" || lower == "seconds" || lower == "minute" ||
           lower == "minutes" || lower == "hour" || lower == "hours" ||
           lower == "day" || lower == "days" || lower == "week" ||
           lower == "weeks" || lower == "month" || lower == "months" ||
           lower == "year" || lower == "years" || lower == "times" ||
           lower == "percent" || lower == "hundred" || lower == "thousand";
}

bool isEnComparativeTok(const EnSegTok &t) {
    return t.lower == "faster" || t.lower == "slower" || t.lower == "better" ||
           t.lower == "worse" || t.lower == "larger" || t.lower == "bigger" ||
           t.lower == "smaller" || t.lower == "higher" || t.lower == "lower" ||
           t.lower == "longer" || t.lower == "shorter" || t.lower == "earlier" ||
           t.lower == "later" || t.lower == "easier" || t.lower == "harder" ||
           t.lower == "cheaper" || t.lower == "stronger" || t.lower == "more";
}

void rewriteEnToZhClause(std::vector<EnSegTok> &clause) {
    if (clause.empty()) {
        return;
    }

    // 1. "How do/does/did/can/should <Subj> <Verb>" -> "<Subj> 如何 <Verb>"
    if (clause.size() >= 4 && clause[0].lower == "how" &&
        (clause[1].lower == "do" || clause[1].lower == "does" ||
         clause[1].lower == "did" || clause[1].lower == "can" ||
         clause[1].lower == "should") &&
        isEnPronounSubject(clause[2].lower)) {
        clause[0].zh = "如何";
        if (clause[1].lower == "do" || clause[1].lower == "does" ||
            clause[1].lower == "did") {
            clause[1].zh.clear();
        }
        EnSegTok subj = std::move(clause[2]);
        clause[2] = std::move(clause[1]);
        clause[1] = std::move(clause[0]);
        clause[0] = std::move(subj);
    }

    // 2. "two" -> "两个" before non-unit nouns, or "两" before units
    for (size_t i = 0; i + 1 < clause.size(); ++i) {
        if (clause[i].lower == "two" && clause[i + 1].kind == EnSegTok::Word &&
            clause[i + 1].lower != "of") {
            clause[i].zh = isEnTimeOrMathUnit(clause[i + 1].lower) ? "两" : "两个";
        }
    }

    // 3. Past finite "-ed" verbs after subject pronoun or coordinated "and <verb-ed>"
    for (size_t i = 1; i < clause.size(); ++i) {
        if (clause[i].kind != EnSegTok::Word || !clause[i].is_ed_past_form) {
            continue;
        }
        const bool after_subj = isEnPronounSubject(clause[i - 1].lower);
        const bool after_and_verb =
            (i >= 3 && clause[i - 1].lower == "and" &&
             (clause[i - 2].is_ed_past_form ||
              (i >= 4 && clause[i - 3].is_ed_past_form) ||
              (i >= 5 && clause[i - 4].is_ed_past_form)));
        if (after_subj || after_and_verb) {
            if (!clause[i].zh.empty() && !clause[i].zh.ends_with("了")) {
                clause[i].zh += "了";
            }
            if (after_and_verb && clause[i - 1].lower == "and") {
                clause[i - 1].zh = "并";
            }
        }
    }

    // 4. "much/far <Comp>" -> "<Comp 去掉'更'?> + 得多", plus "than <NP>" comparative reorder
    for (size_t i = 0; i + 1 < clause.size(); ++i) {
        if ((clause[i].lower == "much" || clause[i].lower == "far") &&
            isEnComparativeTok(clause[i + 1])) {
            std::string base = clause[i + 1].zh;
            if (base.rfind("更", 0) == 0 && base.size() > 3) {
                base.erase(0, 3);
            }
            clause[i].zh.clear();
            clause[i + 1].zh = base + "得多";
        }
    }
    eraseMatchedEmpty(clause, &EnSegTok::zh);

    for (size_t i = 1; i < clause.size(); ++i) {
        if (clause[i].lower == "than" && i + 1 < clause.size() &&
            (isEnComparativeTok(clause[i - 1]) || clause[i - 1].zh.ends_with("得多"))) {
            size_t np_end = std::min(clause.size(), i + 4);
            std::vector<EnSegTok> np(
                std::make_move_iterator(clause.begin() + static_cast<std::ptrdiff_t>(i)),
                std::make_move_iterator(clause.begin() + static_cast<std::ptrdiff_t>(np_end)));
            EnSegTok comp = std::move(clause[i - 1]);
            clause.erase(clause.begin() + static_cast<std::ptrdiff_t>(i - 1),
                         clause.begin() + static_cast<std::ptrdiff_t>(np_end));
            size_t pos = i - 1;
            clause.insert(clause.begin() + static_cast<std::ptrdiff_t>(pos),
                          std::make_move_iterator(np.begin()),
                          std::make_move_iterator(np.end()));
            clause.insert(clause.begin() + static_cast<std::ptrdiff_t>(pos + np.size()),
                          std::move(comp));
            break;
        }
    }

    // 5. "while <V-ing ...>" -> "在 <V ...> 时"
    for (size_t i = 0; i + 1 < clause.size(); ++i) {
        if (clause[i].lower == "while" && clause[i + 1].lower.ends_with("ing")) {
            clause[i].zh = "在";
            if (i + 2 < clause.size() && clause[i + 2].lower == "to") {
                clause[i + 2].zh.clear();
            }
            clause.push_back(EnSegTok{.kind = EnSegTok::Word,
                                      .orig = "",
                                      .lower = "",
                                      .zh = "时",
                                      .matched = true});
            break;
        }
    }
    eraseMatchedEmpty(clause, &EnSegTok::zh);
}

} // namespace

std::optional<OfflineLookupResult> OfflineDictionary::lookupMultiPairDetailed(
    std::string_view source_text,
    std::string_view src_lang,
    std::string_view tgt_lang) const {
    std::string trimmed = utf8::trim(source_text);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    const std::string c_src = canonicalDictLang(src_lang);
    const std::string c_tgt = canonicalDictLang(tgt_lang);
    if (c_src == "auto" || c_tgt == "auto" || c_src == c_tgt) {
        return std::nullopt;
    }

    std::shared_lock<std::shared_mutex> rlock(rw_mu_);
    const std::string pkey = c_src + "|" + c_tgt;
    auto b_it = pair_maps_.find(pkey);
    const PairDictBucket *bucket = (b_it != pair_maps_.end()) ? &b_it->second : nullptr;

    bool has_sqlite = false;
    {
        std::lock_guard<std::mutex> db_lock(db_mu_);
        for (const auto &h : sqlite_handles_) {
            if ((h.source_lang == c_src || h.source_lang == "auto") &&
                (h.target_lang == c_tgt || h.target_lang == "auto")) {
                has_sqlite = true;
                break;
            }
        }
    }
    if (!bucket && !has_sqlite) {
        return std::nullopt;
    }

    const bool tgt_cjk_join = (c_tgt == "zh" || c_tgt == "zh-TW" || c_tgt == "ja");
    const size_t max_cp = std::clamp<size_t>(bucket ? bucket->max_cp : 12, 1, 16);
    const size_t max_words = std::clamp<size_t>(bucket ? bucket->max_words : 4, 1, 6);
    const LookupDirection punct_dir =
        tgt_cjk_join ? LookupDirection::EnToZh : LookupDirection::ZhToEn;

    auto lookupTerm = [&](std::string_view raw_term,
                          bool sentence_mode,
                          std::string &out) -> bool {
        std::string term = utf8::trim(raw_term);
        if (term.empty()) {
            return false;
        }
        if (bucket) {
            if (auto it = bucket->exact_map.find(term); it != bucket->exact_map.end()) {
                out = cleanExternalGloss(it->second, sentence_mode);
                if (!out.empty()) return true;
            }
            std::string lower = toLowerAscii(normalizeAsciiSpaces(term));
            if (auto it = bucket->lower_map.find(lower); it != bucket->lower_map.end()) {
                out = cleanExternalGloss(it->second, sentence_mode);
                if (!out.empty()) return true;
            }
        }
        if (has_sqlite) {
            if (querySqliteForPair(term, c_src, c_tgt, sentence_mode, out)) {
                return true;
            }
            std::string lower = toLowerAscii(normalizeAsciiSpaces(term));
            if (lower != term &&
                querySqliteForPair(lower, c_src, c_tgt, sentence_mode, out)) {
                return true;
            }
        }
        return false;
    };

    auto appendOut = [&](std::string &out, std::string_view tok, bool is_punct) {
        if (tgt_cjk_join) {
            appendChineseToken(out, tok, is_punct);
        } else {
            appendEnglishToken(out, tok);
        }
    };

    auto translateMultiLine = [&](std::string_view line,
                                  int &matched_units,
                                  int &total_units,
                                  bool &line_exact) -> std::string {
        EdgePunctSplit edge = splitEdgePunctuation(line);
        if (edge.core.empty()) {
            return std::string(line);
        }
        std::string exact_gloss;
        if (lookupTerm(edge.core, /*sentence_mode=*/false, exact_gloss)) {
            matched_units += 1;
            total_units += 1;
            line_exact = true;
            std::string out = mapEdgePunctuation(edge.leading, punct_dir);
            if (!out.empty() && !tgt_cjk_join) {
                out.push_back(' ');
            }
            out += exact_gloss;
            std::string trail = mapEdgePunctuation(edge.trailing, punct_dir);
            if (!trail.empty()) {
                appendOut(out, trail, /*is_punct=*/true);
            }
            return out;
        }

        line_exact = false;
        std::string out = mapEdgePunctuation(edge.leading, punct_dir);
        const bool is_unspaced_cjk =
            (c_src == "zh" || c_src == "zh-TW" || c_src == "ja" || c_src == "th") ||
            (containsCjk(edge.core) && edge.core.find(' ') == std::string_view::npos);

        if (is_unspaced_cjk) {
            // Greedy longest codepoint span lookup over unspaced scripts.
            std::vector<size_t> offsets;
            offsets.push_back(0);
            for (size_t p = 0; p < edge.core.size();) {
                size_t nxt = shadowbar::utf8::nextCharOffset(edge.core, p);
                p = (nxt > p) ? nxt : p + 1;
                offsets.push_back(p);
            }
            const size_t n = offsets.size() - 1;
            for (size_t i = 0; i < n;) {
                std::string_view ch(edge.core.data() + offsets[i],
                                    offsets[i + 1] - offsets[i]);
                if (isAsciiSpace(ch[0])) {
                    ++i;
                    continue;
                }
                if (isEdgeWrapperToken(ch)) {
                    appendOut(out, mapPunctuation(ch, punct_dir), /*is_punct=*/true);
                    ++i;
                    continue;
                }
                size_t matched_end = 0;
                std::string matched_gloss;
                const size_t limit = std::min(n, i + max_cp);
                for (size_t j = limit; j > i; --j) {
                    std::string_view span(edge.core.data() + offsets[i],
                                          offsets[j] - offsets[i]);
                    if (lookupTerm(span, /*sentence_mode=*/true, matched_gloss)) {
                        matched_end = j;
                        break;
                    }
                }
                total_units += 1;
                if (matched_end > i) {
                    matched_units += 1;
                    appendOut(out, matched_gloss, /*is_punct=*/false);
                    i = matched_end;
                } else {
                    appendOut(out, ch, /*is_punct=*/false);
                    ++i;
                }
            }
        } else {
            // Word/punctuation tokenization for space-delimited scripts (fr, de, es, ru, ko...).
            struct WordTok {
                std::string text;
                bool is_word = false;
            };
            std::vector<WordTok> toks;
            for (size_t p = 0; p < edge.core.size();) {
                unsigned char c = static_cast<unsigned char>(edge.core[p]);
                if (isAsciiSpace(static_cast<char>(c))) {
                    ++p;
                    continue;
                }
                size_t nxt = shadowbar::utf8::nextCharOffset(edge.core, p);
                std::string_view cp(edge.core.data() + p, nxt - p);
                if (isEdgeWrapperToken(cp)) {
                    toks.push_back({std::string(cp), false});
                    p = nxt;
                    continue;
                }
                size_t end = p;
                while (end < edge.core.size()) {
                    unsigned char ec = static_cast<unsigned char>(edge.core[end]);
                    if (isAsciiSpace(static_cast<char>(ec))) break;
                    size_t cn = shadowbar::utf8::nextCharOffset(edge.core, end);
                    std::string_view cs(edge.core.data() + end, cn - end);
                    if (isEdgeWrapperToken(cs)) break;
                    end = cn;
                }
                toks.push_back({std::string(edge.core.substr(p, end - p)), true});
                p = end;
            }
            for (size_t i = 0; i < toks.size();) {
                if (!toks[i].is_word) {
                    appendOut(out, mapPunctuation(toks[i].text, punct_dir), /*is_punct=*/true);
                    ++i;
                    continue;
                }
                size_t matched_end = 0;
                std::string matched_gloss;
                std::string cand = toks[i].text;
                std::vector<size_t> word_positions = {i};
                for (size_t j = i + 1; j < toks.size() && word_positions.size() < max_words; ++j) {
                    if (!toks[j].is_word) break;
                    word_positions.push_back(j);
                }
                for (size_t k = word_positions.size(); k >= 1; --k) {
                    std::string phrase;
                    for (size_t wi = 0; wi < k; ++wi) {
                        if (wi > 0) phrase.push_back(' ');
                        phrase += toks[word_positions[wi]].text;
                    }
                    if (lookupTerm(phrase, /*sentence_mode=*/true, matched_gloss)) {
                        matched_end = word_positions[k - 1] + 1;
                        break;
                    }
                }
                total_units += 1;
                if (matched_end > i) {
                    matched_units += 1;
                    appendOut(out, matched_gloss, /*is_punct=*/false);
                    i = matched_end;
                } else {
                    appendOut(out, toks[i].text, /*is_punct=*/false);
                    ++i;
                }
            }
        }

        std::string trail = mapEdgePunctuation(edge.trailing, punct_dir);
        if (!trail.empty()) {
            appendOut(out, trail, /*is_punct=*/true);
        }
        return out;
    };

    std::vector<std::string_view> lines;
    {
        size_t start = 0;
        while (start <= trimmed.size()) {
            size_t nl = trimmed.find('\n', start);
            if (nl == std::string::npos) {
                lines.push_back(std::string_view(trimmed).substr(start));
                break;
            }
            lines.push_back(std::string_view(trimmed).substr(start, nl - start));
            start = nl + 1;
        }
    }

    int total_matched = 0;
    int total_units = 0;
    bool all_exact = !lines.empty();
    std::string final_out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) final_out.push_back('\n');
        std::string_view line = utf8::trimView(lines[i]);
        if (line.empty()) continue;
        int lm = 0, lu = 0;
        bool le = false;
        std::string t = translateMultiLine(line, lm, lu, le);
        total_matched += lm;
        total_units += lu;
        all_exact = all_exact && le;
        final_out += t;
    }

    if (total_matched <= 0 || utf8::trimView(final_out).empty()) {
        return std::nullopt;
    }
    return OfflineLookupResult{
        .text = std::move(final_out),
        .matched_units = total_matched,
        .total_units = total_units,
        .exact_match = all_exact,
    };
}

std::optional<OfflineLookupResult> OfflineDictionary::lookupDetailed(
    std::string_view source_text,
    std::string_view target_lang,
    std::string_view source_lang) const {
    std::string trimmed = utf8::trim(source_text);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    // Never falsely translate a lone ASCII byte such as "h" while typing.
    if (trimmed.size() == 1 && static_cast<unsigned char>(trimmed[0]) < 0x80) {
        return std::nullopt;
    }

    const std::string c_src = canonicalDictLang(source_lang);
    const std::string c_tgt = canonicalDictLang(target_lang);
    const std::string det_src =
        (c_src == "auto") ? shadowbar::detectSourceLang(trimmed, "") : c_src;

    // Try explicit or detected non-(ZH<->EN) language pair first.
    if (!det_src.empty() && det_src != "auto" && c_tgt != "auto" &&
        !((det_src == "zh" && c_tgt == "en") || (det_src == "en" && c_tgt == "zh"))) {
        if (auto hit = lookupMultiPairDetailed(trimmed, det_src, c_tgt)) {
            return hit;
        }
    } else if (!det_src.empty() && det_src != "auto" && det_src != "zh" &&
               det_src != "en" && c_tgt == "auto") {
        if (auto hit = lookupMultiPairDetailed(trimmed, det_src, "zh")) {
            return hit;
        }
        if (auto hit = lookupMultiPairDetailed(trimmed, det_src, "en")) {
            return hit;
        }
    }

    // Check any loaded bucket targeting `c_tgt` when source script was ambiguous (e.g. "café").
    if ((det_src.empty() || det_src == "auto") && c_tgt != "auto") {
        std::vector<std::string> candidate_srcs;
        {
            std::shared_lock<std::shared_mutex> rlock(rw_mu_);
            for (const auto &[pkey, _] : pair_maps_) {
                size_t bar = pkey.find('|');
                if (bar != std::string::npos &&
                    std::string_view(pkey).substr(bar + 1) == c_tgt) {
                    candidate_srcs.push_back(pkey.substr(0, bar));
                }
            }
        }
        for (const auto &cs : candidate_srcs) {
            if (auto hit = lookupMultiPairDetailed(trimmed, cs, c_tgt)) {
                return hit;
            }
        }
    }

    // Wait: if caller explicitly chose non-ZH/EN source (e.g. "fr" -> "zh"),
    // don't accidentally hit English entries in built-in EN->ZH.
    if (c_src != "auto" && c_src != "zh" && c_src != "en") {
        return std::nullopt;
    }

    const LookupDirection dir = resolveDirection(trimmed, target_lang);
    if (dir == LookupDirection::None) {
        return std::nullopt;
    }

    std::shared_lock<std::shared_mutex> rlock(rw_mu_);
    const auto &builtin = builtinDictIndex();
        const size_t max_zh_cp = std::clamp<size_t>(
            std::max(builtin.max_zh_cp, custom_max_zh_cp_), 1, 16);
        const size_t max_en_words = std::clamp<size_t>(
            std::max(builtin.max_en_words, custom_max_en_words_), 1, 6);

    auto lookupEnMem = [&](std::string_view lower_key,
                           bool sentence_mode,
                           std::string &out) -> bool {
        if (auto it = custom_en_to_zh_.find(lower_key); it != custom_en_to_zh_.end()) {
            out = cleanExternalGloss(it->second, sentence_mode);
            return !out.empty();
        }
        if (auto it = builtin.en_to_zh.find(lower_key); it != builtin.en_to_zh.end()) {
            out = it->second;
            return !out.empty();
        }
        return false;
    };

    auto lookupEnWithLemmas = [&](std::string_view word,
                                  bool sentence_mode,
                                  std::string &out) -> bool {
        std::string lower = toLowerAscii(normalizeAsciiSpaces(word));
        if (lower.empty()) {
            return false;
        }
        // 1. Check custom & curated built-in lexicon first (exact, acronym plural, lemmas)
        if (lookupEnMem(lower, sentence_mode, out)) {
            return true;
        }
        // Acronym plural like "APIs" / "URLs" / "IDs" -> try lowercase base before SQLite homographs ("Apis"!)
        if (word.size() >= 3 && word.back() == 's' &&
            std::isupper(static_cast<unsigned char>(word[0])) &&
            std::isupper(static_cast<unsigned char>(word[1]))) {
            std::string_view base(lower.data(), lower.size() - 1);
            if (lookupEnMem(base, sentence_mode, out)) {
                return true;
            }
        }
        std::vector<std::string> lemmas = generateLemmaCandidates(lower);
        for (const std::string &cand : lemmas) {
            if (cand != lower && lookupEnMem(cand, sentence_mode, out)) {
                return true;
            }
        }
        // 2. Fallback to external SQLite dictionary (exact then lemmas)
        if (querySqliteEnToZh(lower, sentence_mode, out)) {
            return true;
        }
        for (const std::string &cand : lemmas) {
            if (cand != lower && querySqliteEnToZh(cand, sentence_mode, out)) {
                return true;
            }
        }
        return false;
    };

    auto translateLineZhToEn = [&](std::string_view line,
                                   int &matched_units,
                                   int &total_units,
                                   bool &line_exact) -> std::string {
        EdgePunctSplit edge = splitEdgePunctuation(line);
        if (edge.core.empty()) {
            return std::string(line);
        }

        std::string exact;
        if (lookupExactZhToEn(edge.core, /*sentence_mode=*/false, exact) && !exact.empty()) {
            matched_units += 1;
            total_units += 1;
            line_exact = true;
            std::string out = mapEdgePunctuation(edge.leading, dir);
            out += exact;
            out += mapEdgePunctuation(edge.trailing, dir);
            return out;
        }

        line_exact = false;
        std::vector<ZhSegTok> toks;
        size_t pos = 0;
        const std::string_view s = edge.core;
        while (pos < s.size()) {
            unsigned char b = static_cast<unsigned char>(s[pos]);
            if (isAsciiSpace(static_cast<char>(b))) {
                ++pos;
                continue;
            }

            if (b < 0x80) {
                if (isAsciiWordStart(b) || b == '/' || b == '~') {
                    bool is_tech = false;
                    size_t end = scanAsciiSpan(s, pos, is_tech);
                    if (end > pos) {
                        std::string_view sv = s.substr(pos, end - pos);
                        toks.push_back(ZhSegTok{
                            .kind = is_tech ? ZhSegTok::Tech : ZhSegTok::Word,
                            .zh = "",
                            .en = std::string(sv),
                            .matched = false,
                            .is_builtin = true,
                        });
                        pos = end;
                        continue;
                    }
                }
                std::string mapped = mapPunctuation(s.substr(pos, 1), dir);
                const bool is_punct =
                    isSentenceClosePunct(mapped) || isOpenBracketOrQuote(mapped);
                toks.push_back(ZhSegTok{
                    .kind = is_punct ? ZhSegTok::Punct : ZhSegTok::Tech,
                    .zh = "",
                    .en = std::move(mapped),
                });
                ++pos;
                continue;
            }

            size_t next_cp = pos;
            uint32_t cp = utf8::decodeAt(s, pos, &next_cp);
            if (next_cp <= pos) {
                next_cp = pos + 1;
            }
            if (!isCjkCodepoint(cp)) {
                std::string mapped = mapPunctuation(s.substr(pos, next_cp - pos), dir);
                toks.push_back(ZhSegTok{
                    .kind = ZhSegTok::Punct,
                    .zh = "",
                    .en = std::move(mapped),
                });
                pos = next_cp;
                continue;
            }

            // Collect contiguous CJK run and run O(N * max_zh_cp) DP minimum-cost segmentation.
            std::vector<size_t> offsets;
            offsets.push_back(pos);
            size_t scan = pos;
            while (scan < s.size()) {
                size_t nxt = scan;
                uint32_t c = utf8::decodeAt(s, scan, &nxt);
                if (nxt <= scan || !isCjkCodepoint(c)) {
                    break;
                }
                offsets.push_back(nxt);
                scan = nxt;
            }

            const size_t n = offsets.size() - 1;
            std::vector<int> dp_cost(n + 1, 0);
            std::vector<size_t> dp_next(n, 1);
            std::vector<ZhSegTok> dp_tok(n);

            std::string hit_buf;
            for (size_t i = n; i-- > 0;) {
                int best_cost = 10000000;
                size_t best_j = i + 1;
                ZhSegTok best_tok;

                const size_t lim = std::min(n, i + max_zh_cp);
                for (size_t j = lim; j > i; --j) {
                    const size_t len = j - i;
                    std::string_view slice =
                        s.substr(offsets[i], offsets[j] - offsets[i]);
                    bool is_builtin = false;
                    hit_buf.clear();
                    const bool found =
                        lookupExactZhToEn(slice, /*sentence_mode=*/true, hit_buf, &is_builtin);
                    if (!found && len > 1) {
                        continue;
                    }
                    const int step = scoreZhSegmentStep(slice, len, found, is_builtin);
                    const int total = step + dp_cost[j];
                    if (total < best_cost) {
                        best_cost = total;
                        best_j = j;
                        best_tok.kind = ZhSegTok::Word;
                        best_tok.zh = std::string(slice);
                        best_tok.en = found ? hit_buf : std::string(slice);
                        best_tok.matched = found;
                        best_tok.is_builtin = is_builtin;
                    }
                }
                dp_cost[i] = best_cost;
                dp_next[i] = best_j;
                dp_tok[i] = std::move(best_tok);
            }

            for (size_t i = 0; i < n; i = dp_next[i]) {
                ZhSegTok &chosen = dp_tok[i];
                if (chosen.matched) {
                    matched_units += 1;
                }
                total_units += 1;
                if (!chosen.matched && !toks.empty() &&
                    toks.back().kind == ZhSegTok::Word && !toks.back().matched &&
                    !toks.back().zh.empty()) {
                    // Merge adjacent unmatched Hanzi into one clean literal span.
                    toks.back().zh += chosen.zh;
                    toks.back().en += chosen.en;
                } else {
                    toks.push_back(std::move(chosen));
                }
            }
            pos = offsets.back();
        }

        const std::string mapped_lead = mapEdgePunctuation(edge.leading, dir);
        const std::string mapped_trail = mapEdgePunctuation(edge.trailing, dir);

        // Apply clause-local grammar rewrites cleanly between punctuation anchors.
        std::vector<ZhSegTok> rewritten;
        rewritten.reserve(toks.size() + 2);
        for (size_t idx = 0; idx < toks.size();) {
            if (toks[idx].kind == ZhSegTok::Punct) {
                rewritten.push_back(std::move(toks[idx]));
                ++idx;
                continue;
            }
            size_t end = idx;
            while (end < toks.size() && toks[end].kind != ZhSegTok::Punct) {
                ++end;
            }
            const bool clause_qmark =
                (end < toks.size() && toks[end].en == "?") ||
                (end == toks.size() && mapped_trail.find('?') != std::string::npos);
            std::vector<ZhSegTok> clause(
                std::make_move_iterator(toks.begin() + static_cast<std::ptrdiff_t>(idx)),
                std::make_move_iterator(toks.begin() + static_cast<std::ptrdiff_t>(end)));
            rewriteZhClause(clause, clause_qmark);
            for (auto &ct : clause) {
                rewritten.push_back(std::move(ct));
            }
            idx = end;
        }

        std::string out = mapped_lead;
        for (const ZhSegTok &t : rewritten) {
            if (t.kind == ZhSegTok::Punct) {
                if (isOpenBracketOrQuote(t.en)) {
                    if (!out.empty() && out.back() != ' ' &&
                        out.back() != '(' && out.back() != '[') {
                        out.push_back(' ');
                    }
                    out += t.en;
                } else {
                    appendEnglishToken(out, t.en);
                }
            } else {
                appendEnglishToken(out, t.en);
            }
        }

        if (!mapped_trail.empty()) {
            size_t tpos = 0;
            while (tpos < mapped_trail.size()) {
                size_t tnext = utf8::nextCharOffset(mapped_trail, tpos);
                if (tnext <= tpos) {
                    tnext = tpos + 1;
                }
                std::string_view p(mapped_trail.data() + tpos, tnext - tpos);
                if ((p == "?" || p == "!") && !out.empty() && out.back() == p[0]) {
                    tpos = tnext;
                    continue;
                }
                if (isSentenceClosePunct(p) || p == "\"" || p == "'") {
                    while (!out.empty() && out.back() == ' ') {
                        out.pop_back();
                    }
                    out += p;
                } else {
                    appendEnglishToken(out, p);
                }
                tpos = tnext;
            }
        }
        return out;
    };

    auto translateLineEnToZh = [&](std::string_view line,
                                   int &matched_units,
                                   int &total_units,
                                   bool &line_exact) -> std::string {
        EdgePunctSplit edge = splitEdgePunctuation(line);
        if (edge.core.empty()) {
            return std::string(line);
        }

        const size_t raw_word_count = countAsciiWords(edge.core);
        std::string exact;
        if (raw_word_count <= max_en_words) {
            bool hit_exact = false;
            if (raw_word_count == 1) {
                hit_exact = lookupEnWithLemmas(edge.core, /*sentence_mode=*/false, exact);
            } else {
                hit_exact = lookupExactEnToZh(edge.core, /*sentence_mode=*/false, exact);
            }
            if (hit_exact && !exact.empty()) {
                matched_units += 1;
                total_units += 1;
                line_exact = true;
                std::string out = mapEdgePunctuation(edge.leading, dir);
                out += exact;
                out += mapEdgePunctuation(edge.trailing, dir);
                return out;
            }
        }

        line_exact = false;
        struct RawTok {
            enum Kind { Word, Tech, Punct } kind;
            std::string text;
        };
        std::vector<RawTok> tokens;
        size_t pos = 0;
        const std::string_view s = edge.core;
        while (pos < s.size()) {
            unsigned char b = static_cast<unsigned char>(s[pos]);
            if (isAsciiSpace(static_cast<char>(b))) {
                ++pos;
                continue;
            }
            if (b < 0x80 && (isAsciiWordStart(b) || b == '/' || b == '~')) {
                bool is_tech = false;
                size_t end = scanAsciiSpan(s, pos, is_tech);
                if (end > pos) {
                    tokens.push_back({is_tech ? RawTok::Tech : RawTok::Word,
                                      std::string(s.substr(pos, end - pos))});
                    pos = end;
                    continue;
                }
            }
            size_t next = utf8::nextCharOffset(s, pos);
            if (next <= pos) {
                next = pos + 1;
            }
            tokens.push_back({RawTok::Punct, std::string(s.substr(pos, next - pos))});
            pos = next;
        }

        size_t word_tokens = 0;
        for (const auto &t : tokens) {
            if (t.kind == RawTok::Word) {
                ++word_tokens;
            }
        }

        std::vector<EnSegTok> seg_toks;
        seg_toks.reserve(tokens.size());
        for (size_t i = 0; i < tokens.size();) {
            const RawTok &t = tokens[i];
            if (t.kind == RawTok::Punct) {
                seg_toks.push_back(EnSegTok{
                    .kind = EnSegTok::Punct,
                    .orig = t.text,
                    .lower = t.text,
                    .zh = mapPunctuation(t.text, dir),
                    .matched = true,
                });
                ++i;
                continue;
            }
            if (t.kind == RawTok::Tech) {
                seg_toks.push_back(EnSegTok{
                    .kind = EnSegTok::Tech,
                    .orig = t.text,
                    .lower = toLowerAscii(t.text),
                    .zh = t.text,
                    .matched = true,
                });
                ++i;
                continue;
            }

            size_t max_span = 0;
            while (i + max_span < tokens.size() &&
                   tokens[i + max_span].kind == RawTok::Word &&
                   max_span < max_en_words) {
                ++max_span;
            }

            bool matched = false;
            std::string hit;
            for (size_t len = max_span; len >= 1; --len) {
                std::string phrase = tokens[i].text;
                for (size_t j = 1; j < len; ++j) {
                    phrase.push_back(' ');
                    phrase += tokens[i + j].text;
                }
                if (len > 1) {
                    if (lookupExactEnToZh(phrase, /*sentence_mode=*/true, hit) && !hit.empty()) {
                        seg_toks.push_back(EnSegTok{
                            .kind = EnSegTok::Word,
                            .orig = phrase,
                            .lower = toLowerAscii(phrase),
                            .zh = std::move(hit),
                            .matched = true,
                        });
                        matched_units += static_cast<int>(len);
                        total_units += static_cast<int>(len);
                        i += len;
                        matched = true;
                        break;
                    }
                } else {
                    std::string lower_tok = toLowerAscii(phrase);
                    if (word_tokens > 1 &&
                        (lower_tok == "the" || lower_tok == "a" || lower_tok == "an")) {
                        matched_units += 1;
                        total_units += 1;
                        i += 1;
                        matched = true;
                        break;
                    }
                    if (lookupEnWithLemmas(phrase, /*sentence_mode=*/true, hit) && !hit.empty()) {
                        const bool is_ed =
                            lower_tok.size() > 3 && lower_tok.ends_with("ed");
                        seg_toks.push_back(EnSegTok{
                            .kind = EnSegTok::Word,
                            .orig = phrase,
                            .lower = std::move(lower_tok),
                            .zh = std::move(hit),
                            .matched = true,
                            .is_ed_past_form = is_ed,
                        });
                        matched_units += 1;
                        total_units += 1;
                        i += 1;
                        matched = true;
                        break;
                    }
                    // Fallback: hyphenated compounds like "high-speed" -> sub-word join
                    if (phrase.find('-') != std::string::npos) {
                        std::vector<std::string_view> parts = splitAtDepthZero(phrase, {"-"});
                        std::string combined;
                        bool all_ok = parts.size() >= 2;
                        for (std::string_view part : parts) {
                            std::string part_hit;
                            if (part.empty() ||
                                !lookupEnWithLemmas(part, /*sentence_mode=*/true, part_hit) ||
                                part_hit.empty()) {
                                all_ok = false;
                                break;
                            }
                            combined += part_hit;
                        }
                        if (all_ok && !combined.empty()) {
                            seg_toks.push_back(EnSegTok{
                                .kind = EnSegTok::Word,
                                .orig = phrase,
                                .lower = std::move(lower_tok),
                                .zh = std::move(combined),
                                .matched = true,
                            });
                            matched_units += 1;
                            total_units += 1;
                            i += 1;
                            matched = true;
                            break;
                        }
                    }
                }
            }
            if (!matched) {
                seg_toks.push_back(EnSegTok{
                    .kind = EnSegTok::Word,
                    .orig = t.text,
                    .lower = toLowerAscii(t.text),
                    .zh = t.text,
                    .matched = false,
                });
                total_units += 1;
                ++i;
            }
        }

        // Apply clause-local EN -> ZH rewrites between punctuation tokens.
        std::vector<EnSegTok> rewritten;
        rewritten.reserve(seg_toks.size() + 2);
        for (size_t idx = 0; idx < seg_toks.size();) {
            if (seg_toks[idx].kind == EnSegTok::Punct) {
                rewritten.push_back(std::move(seg_toks[idx]));
                ++idx;
                continue;
            }
            size_t end = idx;
            while (end < seg_toks.size() && seg_toks[end].kind != EnSegTok::Punct) {
                ++end;
            }
            std::vector<EnSegTok> clause(
                std::make_move_iterator(seg_toks.begin() + static_cast<std::ptrdiff_t>(idx)),
                std::make_move_iterator(seg_toks.begin() + static_cast<std::ptrdiff_t>(end)));
            rewriteEnToZhClause(clause);
            for (auto &ct : clause) {
                rewritten.push_back(std::move(ct));
            }
            idx = end;
        }

        std::string out = mapEdgePunctuation(edge.leading, dir);
        for (const EnSegTok &t : rewritten) {
            if (t.kind == EnSegTok::Punct) {
                appendChineseToken(out, t.zh, /*is_punct=*/true);
            } else {
                appendChineseToken(out, t.zh, /*is_punct=*/false);
            }
        }

        std::string trail = mapEdgePunctuation(edge.trailing, dir);
        if (!trail.empty()) {
            appendChineseToken(out, trail, /*is_punct=*/true);
        }
        return out;
    };

    // Process line by line so multiline selections preserve formatting.
    std::vector<std::string_view> lines;
    {
        size_t start = 0;
        while (start <= trimmed.size()) {
            size_t nl = trimmed.find('\n', start);
            if (nl == std::string::npos) {
                lines.push_back(std::string_view(trimmed).substr(start));
                break;
            }
            lines.push_back(std::string_view(trimmed).substr(start, nl - start));
            start = nl + 1;
        }
    }

    int total_matched = 0;
    int total_units = 0;
    bool all_exact = !lines.empty();
    std::string final_out;

    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            final_out.push_back('\n');
        }
        std::string_view line = utf8::trimView(lines[i]);
        if (line.empty()) {
            continue;
        }
        int line_matched = 0;
        int line_units = 0;
        bool line_exact = false;
        std::string translated = (dir == LookupDirection::ZhToEn)
                                     ? translateLineZhToEn(line, line_matched, line_units, line_exact)
                                     : translateLineEnToZh(line, line_matched, line_units, line_exact);
        total_matched += line_matched;
        total_units += line_units;
        all_exact = all_exact && line_exact;
        final_out += translated;
    }

    if (total_matched <= 0 || utf8::trimView(final_out).empty()) {
        // When auto source looked like ASCII English (e.g. "bonjour") and EN->ZH missed,
        // check other imported Latin/script -> zh pairs as a fallback.
        if (c_src == "auto" && (c_tgt == "zh" || c_tgt == "auto")) {
            rlock.unlock();
            std::vector<std::string> extra_srcs;
            {
                std::shared_lock<std::shared_mutex> prlock(rw_mu_);
                const std::string want_tgt = (c_tgt == "auto") ? "zh" : c_tgt;
                for (const auto &[pkey, _] : pair_maps_) {
                    size_t bar = pkey.find('|');
                    if (bar != std::string::npos &&
                        std::string_view(pkey).substr(bar + 1) == want_tgt) {
                        extra_srcs.push_back(pkey.substr(0, bar));
                    }
                }
            }
            for (const auto &es : extra_srcs) {
                if (auto hit = lookupMultiPairDetailed(trimmed, es, c_tgt == "auto" ? "zh" : c_tgt)) {
                    return hit;
                }
            }
        }
        return std::nullopt;
    }

    OfflineLookupResult result;
    result.text = std::move(final_out);
    result.matched_units = total_matched;
    result.total_units = total_units;
    result.exact_match = all_exact;
    return result;
}

std::optional<std::string> OfflineDictionary::lookup(std::string_view source_text,
                                                     std::string_view target_lang,
                                                     std::string_view source_lang) const {
    auto detailed = lookupDetailed(source_text, target_lang, source_lang);
    if (!detailed) {
        return std::nullopt;
    }
    return std::move(detailed->text);
}

OfflineDictStats OfflineDictionary::stats() const {
    std::shared_lock<std::shared_mutex> rlock(rw_mu_);
    const auto &builtin = builtinDictIndex();
    OfflineDictStats s;
    s.zh_to_en_entries = builtin.zh_to_en.size() + custom_zh_to_en_.size();
    s.en_to_zh_entries = builtin.en_to_zh.size() + custom_en_to_zh_.size();

    size_t ml_entries = 0;
    s.supported_pairs = {"zh→en", "en→zh"};
    std::vector<std::string> extra_pairs;
    for (const auto &[pkey, bucket] : pair_maps_) {
        if (bucket.exact_map.empty()) {
            continue;
        }
        ml_entries += bucket.exact_map.size();
        size_t bar = pkey.find('|');
        if (bar != std::string::npos) {
            std::string formatted = pkey.substr(0, bar) + "→" + pkey.substr(bar + 1);
            if (formatted != "zh→en" && formatted != "en→zh") {
                extra_pairs.push_back(std::move(formatted));
            }
        }
    }
    std::sort(extra_pairs.begin(), extra_pairs.end());
    for (auto &ep : extra_pairs) {
        s.supported_pairs.push_back(std::move(ep));
    }

#ifdef SHADOWBAR_HAS_SQLITE3
    s.sqlite_enabled = true;
#else
    s.sqlite_enabled = false;
#endif
    {
        std::lock_guard<std::mutex> db_lock(db_mu_);
        s.sqlite_connected = !sqlite_handles_.empty();
        for (const auto &h : sqlite_handles_) {
            if (h.source_lang != "auto" && h.target_lang != "auto") {
                std::string formatted = h.source_lang + "→" + h.target_lang;
                if (std::find(s.supported_pairs.begin(), s.supported_pairs.end(), formatted) ==
                    s.supported_pairs.end()) {
                    s.supported_pairs.push_back(std::move(formatted));
                }
            }
        }
    }

    s.multi_lang_entries = ml_entries;
    s.total_custom_entries =
        custom_zh_to_en_.size() + custom_en_to_zh_.size() + ml_entries;
    s.loaded_sources = loaded_sources_;
    s.source_reports = source_reports_;
    return s;
}

} // namespace shadowbar
