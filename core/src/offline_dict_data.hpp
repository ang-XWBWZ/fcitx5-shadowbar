#pragma once

#include "shadowbar/cache.hpp"

#include <cstddef>

namespace shadowbar::offline_detail {

struct BuiltinDictIndex {
    OfflineDictMap zh_to_en;
    OfflineDictMap en_to_zh;
    size_t max_zh_cp = 1;
    size_t max_en_words = 1;
};

const BuiltinDictIndex &builtinDictIndex();

} // namespace shadowbar::offline_detail
