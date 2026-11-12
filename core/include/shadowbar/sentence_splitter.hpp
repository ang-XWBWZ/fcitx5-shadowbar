#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace shadowbar {

struct ClauseSplitResult {
    /// Completed sentences/clauses ending in sentence-terminating punctuation.
    std::vector<std::string> locked_clauses;
    /// Trailing unfinalized text currently being composed by the user.
    std::string active_tail;
};

/// Splits text at sentence boundaries so completed clauses stay locked & cached
/// while only `active_tail` is re-translated during typing.
ClauseSplitResult splitClauses(std::string_view text);

/// Returns true if `text` ends with a sentence-terminating delimiter.
bool endsWithSentenceTerminator(std::string_view text);

struct TranslationUnitStats {
    size_t total_units = 0; // cjk_chars + word_count
    size_t cjk_chars = 0;   // CJK characters (counted per character)
    size_t word_count = 0;  // Non-CJK words/tokens (counted per word)
};

/// Counts segment budget units using character counts for CJK and word counts for English/Latin.
TranslationUnitStats countTranslationUnits(std::string_view text);

/// Splits long text into segments of at most `max_units` (CJK per character, English/Latin per word).
/// Prefers breaking at punctuation before the limit (sentence/newline boundaries first,
/// then clause punctuation), falling back to clean word/CJK boundaries without cutting words mid-token.
std::vector<std::string> splitTextIntoScrollChunks(std::string_view text,
                                                   size_t max_units);

} // namespace shadowbar
