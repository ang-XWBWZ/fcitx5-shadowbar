#pragma once

#include "shadowbar/types.hpp"

#include <cstddef>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace shadowbar {

struct TransparentStringHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
};

using OfflineDictMap = std::unordered_map<std::string,
                                          std::string,
                                          TransparentStringHash,
                                          std::equal_to<>>;

/// Thread-safe bounded LRU cache for translated clauses and phrases.
class LruTranslationCache {
public:
    explicit LruTranslationCache(size_t capacity = 2000);

    void setCapacity(size_t capacity);
    std::optional<std::string> get(std::string_view preset_id,
                                   std::string_view target_lang,
                                   std::string_view source_text);
    void put(std::string_view preset_id,
             std::string_view target_lang,
             std::string_view source_text,
             std::string translated_text);
    void clear();
    size_t size() const;

private:
    static std::string makeKey(std::string_view preset_id,
                               std::string_view target_lang,
                               std::string_view source_text);

    size_t capacity_;
    mutable std::mutex mu_;
    std::list<std::pair<std::string, std::string>> items_;
    std::unordered_map<std::string, decltype(items_)::iterator> index_;
};

struct OfflineLookupResult {
    std::string text;
    int matched_units = 0;
    int total_units = 0;
    bool exact_match = false;

    double coverage() const {
        return total_units > 0
                   ? static_cast<double>(matched_units) /
                         static_cast<double>(total_units)
                   : 0.0;
    }
};

struct DictSourceReport {
    std::string path;
    std::string format;
    std::string source_lang = "auto";
    std::string target_lang = "auto";
    size_t entry_count = 0;
    bool loaded = false;
    std::string message;
};

struct OfflineDictStats {
    size_t zh_to_en_entries = 0;
    size_t en_to_zh_entries = 0;
    size_t total_custom_entries = 0;
    size_t multi_lang_entries = 0;
    bool sqlite_enabled = false;
    bool sqlite_connected = false;
    std::vector<std::string> loaded_sources;
    std::vector<std::string> supported_pairs;
    std::vector<DictSourceReport> source_reports;
};

/// Multilingual offline dictionary supporting built-in ZH<->EN lexicon,
/// external TSV/TXT/CSV/CC-CEDICT/JSON/JSONL/StarDict (.ifo+.idx+.dict[.dz]),
/// and one or more SQLite databases across arbitrary configured language pairs.
class OfflineDictionary {
public:
    explicit OfflineDictionary(std::string_view custom_path = "");
    ~OfflineDictionary();

    OfflineDictionary(const OfflineDictionary &) = delete;
    OfflineDictionary &operator=(const OfflineDictionary &) = delete;

    void reload(std::string_view custom_path = "");
    void reload(const std::vector<ExternalDictSpec> &specs);

    std::optional<std::string> lookup(std::string_view source_text,
                                      std::string_view target_lang,
                                      std::string_view source_lang = "auto") const;

    std::optional<OfflineLookupResult> lookupDetailed(
        std::string_view source_text,
        std::string_view target_lang,
        std::string_view source_lang = "auto") const;

    OfflineDictStats stats() const;

    /// Returns managed dictionary directory (`~/.local/share/shadowbar/dicts`).
    static std::string defaultManagedDictDir();

    /// Inspects a dictionary file (format, language pair, entries, error) without activating it.
    static DictSourceReport inspectFile(std::string_view path,
                                        std::string_view hint_source_lang = "auto",
                                        std::string_view hint_target_lang = "auto");

    /// Copies a dictionary file (and StarDict `.idx`/`.dict`/`.dict.dz` companions) into managed dir.
    static bool copyToManagedDir(std::string_view src_path,
                                   std::string &out_managed_path,
                                   std::string *error = nullptr);

    struct PairDictBucket {
        OfflineDictMap exact_map;      // original key -> gloss
        OfflineDictMap lower_map;      // ASCII-lowercased key -> gloss (for space/Latin lookup)
        size_t max_cp = 0;             // max codepoints for non-space languages
        size_t max_words = 0;          // max space-delimited words
    };

    struct SqliteDictHandle {
        std::string path;
        std::string source_lang = "auto";
        std::string target_lang = "auto";
        sqlite3 *db = nullptr;
        sqlite3_stmt *stmt_ci = nullptr;
        sqlite3_stmt *stmt_cs = nullptr;
        bool has_non_ascii_keys = false;
        std::string word_table;
        std::string word_col;
        std::string trans_col;
    };

private:

    bool lookupExactZhToEn(std::string_view zh,
                           bool sentence_mode,
                           std::string &out,
                           bool *out_from_builtin = nullptr) const;
    bool lookupExactEnToZh(std::string_view en,
                           bool sentence_mode,
                           std::string &out,
                           bool *out_from_builtin = nullptr) const;
    std::optional<OfflineLookupResult> lookupMultiPairDetailed(
        std::string_view source_text,
        std::string_view src_lang,
        std::string_view tgt_lang) const;
    bool querySqliteEnToZh(std::string_view en_lower,
                           bool sentence_mode,
                           std::string &out) const;
    bool querySqliteZhToEn(std::string_view zh,
                           bool sentence_mode,
                           std::string &out) const;
    bool querySqliteForPair(std::string_view key,
                            std::string_view src_lang,
                            std::string_view tgt_lang,
                            bool sentence_mode,
                            std::string &out) const;
    void closeSqlite();
    DictSourceReport loadExternalSpec(const ExternalDictSpec &spec);

    mutable std::shared_mutex rw_mu_;
    std::string custom_path_;
    std::string last_load_signature_;
    OfflineDictMap custom_zh_to_en_;
    OfflineDictMap custom_en_to_zh_;
    std::unordered_map<std::string, PairDictBucket> pair_maps_;
    size_t custom_max_zh_cp_ = 0;
    size_t custom_max_en_words_ = 0;
    std::vector<std::string> loaded_sources_;
    std::vector<DictSourceReport> source_reports_;

    mutable std::mutex db_mu_;
    std::vector<SqliteDictHandle> sqlite_handles_;
};

} // namespace shadowbar
