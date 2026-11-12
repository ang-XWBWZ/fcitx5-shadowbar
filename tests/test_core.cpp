#include "shadowbar/cache.hpp"
#include "shadowbar/config.hpp"
#include "shadowbar/sentence_splitter.hpp"
#include "shadowbar/session.hpp"
#include "shadowbar/translator.hpp"
#include "shadowbar/utf8_util.hpp"

#include <atomic>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef SHADOWBAR_HAS_SQLITE3
#include <sqlite3.h>
#endif

using namespace shadowbar;

namespace {

void testUtf8Helpers() {
    const std::string text = "你a好😀";
    assert(text.size() == 11);
    size_t p = text.size();
    p = utf8::prevCharOffset(text, p);
    assert(p == 7); // before 😀
    p = utf8::prevCharOffset(text, p);
    assert(p == 4); // before 好
    p = utf8::prevCharOffset(text, p);
    assert(p == 3); // before a
    p = utf8::prevCharOffset(text, p);
    assert(p == 0); // before 你

    size_t n = 0;
    uint32_t cp = utf8::decodeAt(text, 7, &n);
    assert(cp == 0x1F600);
    assert(n == 11);
}

void testConfigTomlAndSecrets() {
    const std::string sample = R"(# Test config
[trigger]
enable_prefix = true
prefix = ";;"
prefix_only_in_cjk_im = false
prefix_timeout_ms = 320
toggle_hotkey = "Control+Alt+T"
cycle_lang_hotkey = "Alt+Shift+L"
cycle_preset_hotkey = "Alt+Shift+P"

[behavior]
debounce_ms = 180
lock_on_punctuation = true
cache_size = 512

[translation]
source_lang = "zh"
target_lang = "en"
active_preset = "custom"

[ui]
show_source_line = true
collapse_candidates_when_idle = true
expand_actions = true

[presets.custom]
label = "技术"
provider = "openai"
endpoint = "https://example.com/v1"
api_key = "env:SHADOWBAR_TEST_KEY"
model = "gpt-4o-mini"
prompt = "Translate technical Chinese to concise English."
target_lang = "en"
)";

    ShadowConfig cfg;
    std::string err;
    assert(ConfigManager::parseToml(sample, cfg, &err));
    assert(cfg.prefix == ";;");
    assert(cfg.prefix_only_in_cjk_im == false);
    assert(cfg.prefix_timeout_ms == 320);
    assert(cfg.debounce_ms == 180);
    assert(cfg.cache_size == 512);
    assert(cfg.active_preset == "custom");
    const auto *p = cfg.findPreset("custom");
    assert(p != nullptr);
    assert(p->label == "技术");
    assert(p->provider == ProviderKind::OpenAICompatible);
    assert(p->model == "gpt-4o-mini");

    setenv("SHADOWBAR_TEST_KEY", "secret-xyz", 1);
    assert(ConfigManager::resolveSecretValue(p->api_key) == "secret-xyz");
    assert(ConfigManager::resolveSecretValue("${SHADOWBAR_TEST_KEY}") == "secret-xyz");

    // Round-trip serialization
    std::string serialized = ConfigManager::serializeToml(cfg);
    ShadowConfig cfg2;
    assert(ConfigManager::parseToml(serialized, cfg2, &err));
    assert(cfg2.prefix == ";;");
    assert(cfg2.cycle_lang_hotkey == "Alt+Shift+L");
    assert(cfg2.cycle_preset_hotkey == "Alt+Shift+P");
    assert(cfg2.expand_actions == true);
    assert(cfg2.findPreset("custom")->label == "技术");

    // Global target_lang overrides preset fallback and syncs dynamically.
    ShadowConfig remembered = ShadowConfig::makeDefault();
    remembered.target_lang = "ja";
    ShadowSession remembered_sess(remembered);
    assert(remembered_sess.viewModel().target_lang == "ja");
    remembered_sess.syncSelectionDefaults("work", "en");
    assert(remembered_sess.activePresetId() == "work");
    assert(remembered_sess.rawTargetLang() == "en");
}

void testSentenceSplitter() {
    auto s1 = splitClauses("今天天气真好。版本v1.2发布！接下来看PI=3.14吧");
    assert(s1.locked_clauses.size() == 2);
    assert(s1.locked_clauses[0] == "今天天气真好。");
    assert(s1.locked_clauses[1] == "版本v1.2发布！");
    assert(s1.active_tail == "接下来看PI=3.14吧");

    assert(endsWithSentenceTerminator("没问题。"));
    assert(endsWithSentenceTerminator("Ok!"));
    assert(!endsWithSentenceTerminator("v1.2"));
    assert(!endsWithSentenceTerminator("正在输入"));

    // Scroll chunker: strong sentence punctuation takes precedence over commas.
    auto c_short = splitTextIntoScrollChunks("短句测试。", 50);
    assert(c_short.size() == 1);
    assert(c_short[0] == "短句测试。");

    auto c_punct = splitTextIntoScrollChunks(
        "第一句在这里结束。这里逗号，然后继续写下去。", 15);
    assert(c_punct.size() == 2);
    assert(c_punct[0] == "第一句在这里结束。");
    assert(c_punct[1] == "这里逗号，然后继续写下去。");

    // Clause comma fallback when no sentence boundary precedes max_chars.
    auto c_comma = splitTextIntoScrollChunks(
        "前半句写得很长很长，后半句也写得很长很长", 14);
    assert(c_comma.size() == 2);
    assert(c_comma[0] == "前半句写得很长很长，");
    assert(c_comma[1] == "后半句也写得很长很长");

    // Hard UTF-8 codepoint truncation when no punctuation exists.
    const std::string no_punct = "你好世界测试没有标点符号的连续中文文本";
    auto c_hard = splitTextIntoScrollChunks(no_punct, 6);
    assert(c_hard.size() == 4);
    assert(c_hard[0] == "你好世界测试");
    assert(c_hard[1] == "没有标点符号");
    assert(c_hard[2] == "的连续中文文");
    assert(c_hard[3] == "本");

    // English splits by word tokens rather than character count, never mid-word.
    auto stats_mixed = countTranslationUnits("Hello, world! 你好世界 don't v1.25bit");
    assert(stats_mixed.word_count == 4);
    assert(stats_mixed.cjk_chars == 4);
    assert(stats_mixed.total_units == 8);

    auto en_words = splitTextIntoScrollChunks(
        "alpha beta gamma delta epsilon", 2);
    assert(en_words.size() == 3);
    assert(en_words[0] == "alpha beta");
    assert(en_words[1] == "gamma delta");
    assert(en_words[2] == "epsilon");

    auto en_punct = splitTextIntoScrollChunks(
        "Hello, world! Don't break v1.25bit identifiers.", 4);
    assert(en_punct.size() == 2);
    assert(en_punct[0] == "Hello, world!");
    assert(en_punct[1] == "Don't break v1.25bit identifiers.");

    auto mixed_chunks = splitTextIntoScrollChunks(
        "这里支持 AVX2 SIMD 和 Vulkan 加速", 5);
    assert(mixed_chunks.size() == 2);
    assert(mixed_chunks[0] == "这里支持 AVX2");
    assert(mixed_chunks[1] == "SIMD 和 Vulkan 加速");
}

void testLruCache() {
    LruTranslationCache cache(2);
    cache.put("p", "en", "一", "one");
    cache.put("p", "en", "二", "two");
    assert(cache.get("p", "en", "一") == "one"); // promotes 一
    cache.put("p", "en", "三", "three");         // evicts 二
    assert(!cache.get("p", "en", "二").has_value());
    assert(cache.get("p", "en", "一") == "one");
    assert(cache.get("p", "en", "三") == "three");
}

void testSessionStateMachine() {
    ShadowConfig cfg = ShadowConfig::makeDefault();
    cfg.prefix_only_in_cjk_im = true;
    cfg.prefix_timeout_ms = 300;
    ShadowSession session(cfg);

    auto t0 = SteadyClock::now();

    // 1. In non-CJK IM, prefix does not intercept ';'.
    auto a1 = session.onKey({.utf8_char = ";"}, false, false, t0);
    assert(!a1.consume_key);
    assert(!session.isActive());

    // 2. In CJK IM, single ';' buffers without leaking, then second ';' triggers OneShot.
    auto a2 = session.onKey({.utf8_char = ";"}, false, true, t0);
    assert(a2.consume_key);
    assert(!session.isActive());
    auto a3 = session.onKey({.utf8_char = ";"}, false, true,
                            t0 + std::chrono::milliseconds(100));
    assert(a3.consume_key);
    assert(session.mode() == Mode::OneShot);
    assert(session.shouldShowBar());

    // 3. Single ';' followed by timeout replays ';' cleanly.
    session.exitToOff();
    auto b1 = session.onKey({.utf8_char = ";"}, false, true, t0);
    assert(b1.consume_key);
    auto b2 = session.flushPendingPrefixIfExpired(
        t0 + std::chrono::milliseconds(400));
    assert(b2.replay_raw_prefix == ";");
    assert(!session.isActive());

    // 4. OneShot commit flow + Shift+Enter raw fallback.
    session.enterOneShot();
    auto c1 = session.onCommitFromIme("今天", t0);
    assert(c1.trigger_translation);
    assert(session.onTranslationUpdate(
        session.revision(), {.ok = true, .is_final = true, .translated_text = "today"}));

    // Pressing Shift+Enter sends raw Chinese and exits OneShot.
    auto c2 = session.onKey({.special = SpecialKey::Enter, .shift = true},
                            false, true, t0);
    assert(c2.consume_key);
    assert(c2.commit_to_app == "今天");
    assert(session.mode() == Mode::Off);

    // 5. Sticky mode: Enter commits translation, stays Sticky, and empty Enter passes through.
    session.toggleSticky();
    assert(session.mode() == Mode::Sticky);
    assert(!session.shouldShowBar()); // Hidden while buffer is empty.
    // Esc on empty Sticky must NOT be stolen (Vim safety).
    auto esc_empty = session.onKey({.special = SpecialKey::Escape}, false, true, t0);
    assert(!esc_empty.consume_key);
    assert(session.mode() == Mode::Sticky);

    session.onCommitFromIme("你好", t0);
    assert(session.shouldShowBar());
    assert(session.onTranslationUpdate(
        session.revision(), {.ok = true, .is_final = true, .translated_text = "hello"}));

    // Tab cycles preset and requests immediate re-translation.
    auto tab_act = session.onKey({.special = SpecialKey::Tab}, false, true, t0);
    assert(tab_act.consume_key);
    assert(tab_act.immediate_translation);
    assert(session.onTranslationUpdate(
        session.revision(), {.ok = true, .is_final = true, .translated_text = "hi there"}));

    // Left + Backspace UTF-8 cursor edit: "你好" -> Left (before 好) -> BackSpace deletes 你 -> "好".
    auto left_act = session.onKey({.special = SpecialKey::Left}, false, true, t0);
    assert(left_act.consume_key);
    auto bs_act = session.onKey({.special = SpecialKey::Backspace}, false, true, t0);
    assert(bs_act.consume_key);
    assert(session.viewModel().source_text == "好");

    session.onCommitFromIme("的", t0);
    assert(session.viewModel().source_text == "的好");
    auto right_act = session.onKey({.special = SpecialKey::Right}, false, true, t0);
    assert(right_act.consume_key);
    session.onCommitFromIme("!", t0);
    assert(session.viewModel().source_text == "的好!");
    assert(session.onTranslationUpdate(
        session.revision(), {.ok = true, .is_final = true, .translated_text = "okay"}));

    auto enter_send = session.onKey({.special = SpecialKey::Enter}, false, true, t0);
    assert(enter_send.consume_key);
    assert(enter_send.commit_to_app == "okay");
    assert(session.mode() == Mode::Sticky);
    assert(!session.shouldShowBar());

    // Subsequent Enter on empty Sticky passes through to chat app.
    auto enter_pass = session.onKey({.special = SpecialKey::Enter}, false, true, t0);
    assert(!enter_pass.consume_key);

    // 6. Home / End cursor navigation & Ctrl+Backspace draft clear.
    session.onCommitFromIme("abc", t0);
    assert(session.onKey({.special = SpecialKey::Home}, false, true, t0).consume_key);
    assert(session.viewModel().cursor_byte == 0);
    session.onKey({.utf8_char = "0"}, false, false, t0);
    assert(session.onKey({.special = SpecialKey::End}, false, true, t0).consume_key);
    session.onKey({.utf8_char = "9"}, false, false, t0);
    assert(session.viewModel().source_text == "0abc9");

    auto ctrl_bs = session.onKey({.special = SpecialKey::Backspace, .ctrl = true},
                                 false, true, t0);
    assert(ctrl_bs.consume_key);
    assert(session.viewModel().source_text.empty());
    assert(session.mode() == Mode::Sticky);
    // Empty Sticky Ctrl+Backspace passes through to app.
    assert(!session.onKey({.special = SpecialKey::Backspace, .ctrl = true},
                          false, true, t0).consume_key);

    // 7. Shift+Tab reverse preset cycling & smart EN->中 auto direction.
    const std::string before_rev = session.viewModel().active_preset_id;
    session.onCommitFromIme("hello world", t0);
    assert(session.viewModel().effective_source_lang == "en");
    assert(session.viewModel().target_lang == "zh");
    assert(session.viewModel().direction_label == "EN→中");
    auto rev_tab = session.onKey({.special = SpecialKey::Tab, .shift = true},
                                 false, true, t0);
    assert(rev_tab.consume_key);
    assert(session.viewModel().active_preset_id != before_rev);

    session.clearDraft();
    session.onCommitFromIme("看看 API", t0);
    assert(session.viewModel().effective_source_lang == "zh");
    assert(session.viewModel().target_lang == "en");
    assert(session.viewModel().direction_label == "中→EN");

    // 8. Translation failure allows retry via Enter or retryTranslation().
    assert(session.onTranslationUpdate(
        session.revision(), {.ok = false, .is_final = true, .error_message = "timeout"}));
    assert(session.viewModel().status == TranslationStatus::Error);
    auto retry_enter = session.onKey({.special = SpecialKey::Enter}, false, true, t0);
    assert(retry_enter.consume_key);
    assert(retry_enter.trigger_translation);
    assert(retry_enter.immediate_translation);
    assert(retry_enter.bypass_cache_translation);
    assert(session.viewModel().status == TranslationStatus::Pending);

    auto retry_explicit = session.retryTranslation();
    assert(retry_explicit.trigger_translation);
    assert(retry_explicit.immediate_translation);
    assert(retry_explicit.bypass_cache_translation);
}

void testAsyncTranslatorStreamingAndClauseLock() {
    std::mutex call_mu;
    std::vector<std::string> requested_bodies;
    std::atomic<int> call_count{0};

    HttpTransport mock_transport =
        [&](const HttpRequest &req, const std::atomic<bool> &cancel,
            HttpStreamChunkCallback on_chunk) -> HttpResponse {
        {
            std::lock_guard<std::mutex> lock(call_mu);
            requested_bodies.push_back(req.body);
        }
        ++call_count;
        if (cancel.load()) {
            return {.cancelled = true};
        }
        if (req.body.find("今天很忙。") != std::string::npos &&
            req.body.find("明天") == std::string::npos) {
            on_chunk("data: {\"choices\":[{\"delta\":{\"content\":\"<think>ok</think>Today is \"}}]}\n");
            on_chunk("data: {\"choices\":[{\"delta\":{\"content\":\"busy.\"}}]}\n");
            on_chunk("data: [DONE]\n");
            return {.status_code = 200};
        }
        if (req.body.find("明天再聊") != std::string::npos) {
            on_chunk("data: {\"choices\":[{\"delta\":{\"content\":\"Let's talk \"}}]}\n");
            on_chunk("data: {\"choices\":[{\"delta\":{\"content\":\"tomorrow\"}}]}\n");
            on_chunk("data: [DONE]\n");
            return {.status_code = 200};
        }
        return {.status_code = 500, .error_message = "mock error"};
    };

    AsyncTranslationEngine engine(mock_transport, 100);
    Preset p;
    p.id = "mock_openai";
    p.provider = ProviderKind::OpenAICompatible;
    p.endpoint = "http://127.0.0.1:9999/v1";
    p.target_lang = "en";

    std::mutex res_mu;
    std::condition_variable res_cv;
    std::vector<TranslationResult> updates;

    auto submit_and_wait = [&](uint64_t rev, std::string text,
                               bool bypass_cache = false,
                               bool offline_fallback = true) {
        updates.clear();
        TranslationJob job{
            .revision = rev,
            .source_text = std::move(text),
            .source_lang = "zh",
            .preset = p,
            .immediate = true,
            .lock_on_punctuation = true,
            .offline_fallback = offline_fallback,
            .bypass_cache = bypass_cache,
            .debounce_ms = 0,
            .timeout_ms = 2000,
        };
        engine.submit(std::move(job), [&](uint64_t r, const TranslationResult &res) {
            if (r != rev) {
                return;
            }
            std::lock_guard<std::mutex> lock(res_mu);
            updates.push_back(res);
            if (res.is_final) {
                res_cv.notify_all();
            }
        });
        std::unique_lock<std::mutex> lock(res_mu);
        assert(res_cv.wait_for(lock, std::chrono::seconds(3), [&]() {
            return !updates.empty() && updates.back().is_final;
        }));
    };

    // 1. Complete first sentence -> cached & <think> stripped.
    submit_and_wait(1, "今天很忙。");
    assert(updates.back().ok);
    assert(updates.back().translated_text == "Today is busy.");
    assert(call_count == 1);

    // 2. Append second clause -> first clause comes from cache (only 1 new HTTP call!),
    //    and combined result has both clauses.
    submit_and_wait(2, "今天很忙。明天再聊");
    assert(updates.back().ok);
    assert(updates.back().translated_text == "Today is busy. Let's talk tomorrow");
    assert(call_count == 2);

    // 3. Offline fallback when network fails on known short phrase.
    submit_and_wait(3, "谢谢");
    assert(updates.back().ok);
    assert(updates.back().from_offline_dict);
    assert(updates.back().translated_text == "thank you");

    // 4. Cached clause stays in cache (no HTTP call), whereas bypass_cache=true forces fresh call.
    submit_and_wait(4, "今天很忙。", false, false);
    assert(updates.back().ok);
    assert(call_count == 3); // "谢谢" attempted HTTP once before offline fallback.

    submit_and_wait(5, "今天很忙。", true, false);
    assert(updates.back().ok);
    assert(updates.back().translated_text == "Today is busy.");
    assert(call_count == 4);

    // 5. Failed request is never cached, so retry succeeds once network recovers.
    submit_and_wait(6, "未知失败句", false, false);
    assert(!updates.back().ok);
    assert(call_count == 5);

    engine.shutdown();
}

void testFloatingConfigAndDirection() {
    ShadowConfig cfg = ShadowConfig::makeDefault();
    cfg.floating_enabled = false;
    cfg.floating_hotkey = "Alt+Shift+F";
    cfg.floating_auto_read_clipboard = false;
    cfg.floating_watch_clipboard = true;
    cfg.floating_show_orb = true;
    cfg.floating_pinned = true;
    cfg.floating_result_only = true;
    cfg.floating_font_scale = 125;
    cfg.floating_segment_chars = 800;

    const std::string toml = ConfigManager::serializeToml(cfg);
    ShadowConfig roundtrip = ShadowConfig::makeDefault();
    std::string err;
    assert(ConfigManager::parseToml(toml, roundtrip, &err));
    assert(!roundtrip.floating_enabled);
    assert(roundtrip.floating_hotkey == "Alt+Shift+F");
    assert(!roundtrip.floating_auto_read_clipboard);
    assert(roundtrip.floating_watch_clipboard);
    assert(roundtrip.floating_show_orb);
    assert(roundtrip.floating_pinned);
    assert(roundtrip.floating_result_only);
    assert(roundtrip.floating_font_scale == 125);
    assert(roundtrip.floating_segment_chars == 800);

    ShadowConfig clamped = ShadowConfig::makeDefault();
    assert(ConfigManager::parseToml(
        "[floating]\nfont_scale = 999\nsegment_max_chars = 10\n",
        clamped, &err));
    assert(clamped.floating_font_scale == 160);
    assert(clamped.floating_segment_chars == 150);

    assert(ConfigManager::sanitizePresetId(" My Preset-01 ") == "my_preset-01");
    assert(detectSourceLang("Hello world") == "en");
    assert(detectSourceLang("こんにちは") == "ja");
    assert(detectSourceLang("你好世界") == "zh");

    auto d1 = resolveTranslationDirection("Hello world", "auto", "en", true);
    assert(d1.first == "en" && d1.second == "zh");
    auto d2 = resolveTranslationDirection("你好世界", "auto", "zh", true);
    assert(d2.first == "zh" && d2.second == "en");
    auto d3 = resolveTranslationDirection("你好世界", "zh", "en", true);
    assert(d3.first == "zh" && d3.second == "en");
}

void testAiApiOptimizations() {
    std::atomic<bool> cancel{false};

    // 1. Endpoint normalization + Azure/Volcengine/Zhipu/Gemini/Ollama/DeepL + headers + prompts.
    {
        std::vector<HttpRequest> seen;
        HttpTransport rec_ok = [&](const HttpRequest &req, const std::atomic<bool> &,
                                   HttpStreamChunkCallback) -> HttpResponse {
            seen.push_back(req);
            return {.status_code = 200,
                    .body = R"({"choices":[{"message":{"content":"ok"}}]})"};
        };

        // Bare OpenAI host
        Preset p_openai{
            .id = "oa",
            .provider = ProviderKind::OpenAICompatible,
            .endpoint = "https://api.openai.com/",
            .api_key = "sk-test",
            .model = "gpt-4o-mini",
            .system_prompt = "Translate naturally into English.",
            .target_lang = "zh",
        };
        auto r = translateSingleUnit(p_openai, "auto", "Hello world", "", 4000,
                                     cancel, rec_ok, {});
        assert(r.ok && r.translated_text == "ok");
        assert(seen.back().url == "https://api.openai.com/v1/chat/completions");
        assert(seen.back().body.find("from auto") == std::string::npos);
        assert(seen.back().body.find("Detect the source language automatically") !=
               std::string::npos);
        assert(seen.back().body.find("Simplified Chinese") != std::string::npos);

        // Volcengine /api/v3
        Preset p_ark = p_openai;
        p_ark.endpoint = "https://ark.cn-beijing.volces.com/api/v3";
        r = translateSingleUnit(p_ark, "fr", "Bonjour", "", 4000, cancel, rec_ok, {});
        assert(r.ok);
        assert(seen.back().url ==
               "https://ark.cn-beijing.volces.com/api/v3/chat/completions");
        assert(seen.back().body.find("French") != std::string::npos);

        // Zhipu /api/paas/v4
        Preset p_zhipu = p_openai;
        p_zhipu.endpoint = "https://open.bigmodel.cn/api/paas/v4/";
        r = translateSingleUnit(p_zhipu, "de", "Hallo", "", 4000, cancel, rec_ok, {});
        assert(r.ok);
        assert(seen.back().url ==
               "https://open.bigmodel.cn/api/paas/v4/chat/completions");
        assert(seen.back().body.find("German") != std::string::npos);

        // Gemini OpenAI compat /v1beta/openai
        Preset p_gemini = p_openai;
        p_gemini.endpoint = "https://generativelanguage.googleapis.com/v1beta/openai";
        r = translateSingleUnit(p_gemini, "en", "Hi", "", 4000, cancel, rec_ok, {});
        assert(r.ok);
        assert(seen.back().url ==
               "https://generativelanguage.googleapis.com/v1beta/openai/chat/completions");

        // Azure OpenAI query param preservation & api-key header
        Preset p_azure = p_openai;
        p_azure.endpoint =
            "https://myres.openai.azure.com/openai/deployments/gpt4o?api-version=2024-08-01-preview";
        r = translateSingleUnit(p_azure, "en", "Hi", "", 4000, cancel, rec_ok, {});
        assert(r.ok);
        assert(seen.back().url ==
               "https://myres.openai.azure.com/openai/deployments/gpt4o/chat/completions?api-version=2024-08-01-preview");
        bool has_api_key_hdr = false;
        for (const auto &h : seen.back().headers) {
            if (h.name == "api-key" && h.value == "sk-test") {
                has_api_key_hdr = true;
            }
        }
        assert(has_api_key_hdr);

        // Ollama /api/generate rewrite -> /api/chat (native response)
        HttpTransport ollama_ok = [&](const HttpRequest &req, const std::atomic<bool> &,
                                      HttpStreamChunkCallback) -> HttpResponse {
            seen.push_back(req);
            return {.status_code = 200,
                    .body = R"({"message":{"content":"你好"}})"};
        };
        Preset p_ollama{
            .id = "ol",
            .provider = ProviderKind::Ollama,
            .endpoint = "http://127.0.0.1:11434/api/generate",
            .model = "qwen2.5:7b",
            .target_lang = "zh-TW",
        };
        r = translateSingleUnit(p_ollama, "en", "Hello", "", 4000, cancel, ollama_ok, {});
        assert(r.ok && r.translated_text == "你好");
        assert(seen.back().url == "http://127.0.0.1:11434/api/chat");
        assert(seen.back().body.find("Traditional Chinese") != std::string::npos);

        // Ollama /v1 -> switches cleanly to /v1/chat/completions without Ollama-only options
        p_ollama.endpoint = "127.0.0.1:11434/v1";
        r = translateSingleUnit(p_ollama, "en", "Hello", "", 4000, cancel, rec_ok, {});
        assert(r.ok);
        assert(seen.back().url == "http://127.0.0.1:11434/v1/chat/completions");
        assert(seen.back().body.find("\"options\"") == std::string::npos);

        // DeepL free vs pro endpoint & ZH-HANT normalization
        HttpTransport deepl_ok = [&](const HttpRequest &req, const std::atomic<bool> &,
                                     HttpStreamChunkCallback) -> HttpResponse {
            seen.push_back(req);
            return {.status_code = 200,
                    .body = R"({"translations":[{"text":"哈囉"}]})"};
        };
        Preset p_deepl{
            .id = "dl",
            .provider = ProviderKind::DeepL,
            .endpoint = "",
            .api_key = "abc-free-key:fx",
            .target_lang = "zh-TW",
        };
        r = translateSingleUnit(p_deepl, "en", "Hello", "", 4000, cancel, deepl_ok, {});
        assert(r.ok && r.translated_text == "哈囉");
        assert(seen.back().url == "https://api-free.deepl.com/v2/translate");
        assert(seen.back().body.find("\"ZH-HANT\"") != std::string::npos);

        p_deepl.api_key = "pro-key-without-fx";
        r = translateSingleUnit(p_deepl, "en", "Hello", "", 4000, cancel, deepl_ok, {});
        assert(r.ok);
        assert(seen.back().url == "https://api.deepl.com/v2/translate");
    }

    // 2. Preflight failures make zero HTTP calls.
    {
        int calls = 0;
        HttpTransport never_called = [&](const HttpRequest &, const std::atomic<bool> &,
                                         HttpStreamChunkCallback) -> HttpResponse {
            ++calls;
            return {.status_code = 200};
        };
        unsetenv("SHADOWBAR_UNSET_ENV_KEY_TEST");
        Preset p_env{
            .id = "bad_env",
            .provider = ProviderKind::OpenAICompatible,
            .endpoint = "https://api.openai.com/v1",
            .api_key = "env:SHADOWBAR_UNSET_ENV_KEY_TEST",
            .model = "gpt-4o-mini",
            .target_lang = "en",
        };
        auto r1 = translateSingleUnit(p_env, "zh", "你好", "", 2000, cancel, never_called, {});
        assert(!r1.ok && calls == 0);
        assert(r1.error_message.find("SHADOWBAR_UNSET_ENV_KEY_TEST") != std::string::npos);

        Preset p_scheme = p_env;
        p_scheme.api_key = "sk-test";
        p_scheme.endpoint = "ftp://invalid.host/v1";
        auto r2 =
            translateSingleUnit(p_scheme, "zh", "你好", "", 2000, cancel, never_called, {});
        assert(!r2.ok && calls == 0);

        Preset p_cloud_nokey = p_env;
        p_cloud_nokey.api_key.clear();
        p_cloud_nokey.endpoint = "https://api.deepseek.com";
        auto r3 = translateSingleUnit(p_cloud_nokey, "zh", "你好", "", 2000, cancel,
                                      never_called, {});
        assert(!r3.ok && calls == 0);
    }

    // 3. Detailed JSON error parsing & inline HTTP-200 SSE error capture.
    {
        HttpTransport err401 = [&](const HttpRequest &, const std::atomic<bool> &,
                                   HttpStreamChunkCallback) -> HttpResponse {
            return {
                .status_code = 401,
                .body = R"({"error":{"message":"Incorrect API key provided","code":"invalid_api_key"}})",
            };
        };
        Preset p{
            .id = "err",
            .provider = ProviderKind::OpenAICompatible,
            .endpoint = "http://127.0.0.1:9000/v1",
            .model = "gpt-4o-mini",
            .target_lang = "en",
        };
        auto r401 = translateSingleUnit(p, "zh", "你好", "", 2000, cancel, err401, {});
        assert(!r401.ok);
        assert(r401.error_message.find("401") != std::string::npos);
        assert(r401.error_message.find("Incorrect API key provided") != std::string::npos);

        HttpTransport sse_err = [&](const HttpRequest &, const std::atomic<bool> &,
                                    HttpStreamChunkCallback on_chunk) -> HttpResponse {
            on_chunk("data: {\"error\":{\"message\":\"Quota exhausted\",\"code\":\"quota\"}}\n");
            on_chunk("data: [DONE]\n");
            return {.status_code = 200};
        };
        auto rsse = translateSingleUnit(p, "zh", "你好", "", 2000, cancel, sse_err, {});
        assert(!rsse.ok);
        assert(rsse.error_message.find("Quota exhausted") != std::string::npos);
    }

    // 4. Content-part array, unclosed <thinking> suppression in partials, code-fence strip, and stream->non-stream fallback.
    {
        std::vector<std::string> partials;
        HttpTransport array_stream = [&](const HttpRequest &, const std::atomic<bool> &,
                                         HttpStreamChunkCallback on_chunk) -> HttpResponse {
            on_chunk("data: {\"choices\":[{\"delta\":{\"content\":[{\"type\":\"text\",\"text\":\"<thinking>step 1\"}]}}]}\n");
            on_chunk("data: {\"choices\":[{\"delta\":{\"content\":[{\"type\":\"text\",\"text\":\"</thinking>```text\\nHello\"}]}}]}\n");
            on_chunk("data: {\"choices\":[{\"delta\":{\"content\":\" world\\n```\"}}]}\n");
            on_chunk("data: [DONE]\n");
            return {.status_code = 200};
        };
        Preset p{
            .id = "stream",
            .provider = ProviderKind::OpenAICompatible,
            .endpoint = "http://127.0.0.1:9000/v1",
            .model = "gpt-4o-mini",
            .target_lang = "en",
        };
        auto r = translateSingleUnit(
            p, "zh", "你好世界", "", 2000, cancel, array_stream,
            [&](std::string_view part) { partials.emplace_back(part); });
        assert(r.ok && r.translated_text == "Hello world");
        for (const auto &part : partials) {
            assert(part.find("step 1") == std::string::npos);
            assert(part.find("<thinking>") == std::string::npos);
        }

        // Fallback from rejected `stream: true` -> `stream: false`
        std::vector<std::string> fb_bodies;
        HttpTransport fallback_t = [&](const HttpRequest &req, const std::atomic<bool> &,
                                       HttpStreamChunkCallback) -> HttpResponse {
            fb_bodies.push_back(req.body);
            if (req.body.find("\"stream\":true") != std::string::npos) {
                return {.status_code = 400,
                        .body = R"({"error":{"message":"stream is not supported by this model"}})"};
            }
            return {.status_code = 200,
                    .body = R"({"choices":[{"message":{"content":"Fallback OK"}}]})"};
        };
        auto rfb = translateSingleUnit(p, "zh", "你好", "", 3000, cancel, fallback_t, {});
        assert(rfb.ok && rfb.translated_text == "Fallback OK");
        assert(fb_bodies.size() == 2);
        assert(fb_bodies[0].find("\"stream\":true") != std::string::npos);
        assert(fb_bodies[1].find("\"stream\":false") != std::string::npos);
    }

    // 5. Transient retry on 503/429 vs no retry after real streamed content delta.
    {
        int tries = 0;
        HttpTransport flaky = [&](const HttpRequest &, const std::atomic<bool> &,
                                  HttpStreamChunkCallback on_chunk) -> HttpResponse {
            ++tries;
            if (tries == 1) {
                on_chunk(R"({"error":"upstream overloaded"})");
                return {.status_code = 503, .body = R"({"error":"upstream overloaded"})"};
            }
            return {.status_code = 200,
                    .body = R"({"choices":[{"message":{"content":"Recovered"}}]})"};
        };
        Preset p{
            .id = "retry",
            .provider = ProviderKind::OpenAICompatible,
            .endpoint = "http://127.0.0.1:9000/v1",
            .model = "gpt-4o-mini",
            .target_lang = "en",
        };
        auto rr = translateSingleUnit(p, "zh", "你好", "", 4000, cancel, flaky, {});
        assert(rr.ok && rr.translated_text == "Recovered");
        assert(tries == 2);

        int mid_tries = 0;
        HttpTransport mid_drop = [&](const HttpRequest &, const std::atomic<bool> &,
                                     HttpStreamChunkCallback on_chunk) -> HttpResponse {
            ++mid_tries;
            on_chunk("data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n");
            return {.status_code = 0, .error_message = "connection reset"};
        };
        auto rdrop = translateSingleUnit(p, "zh", "你好", "", 4000, cancel, mid_drop, {});
        assert(!rdrop.ok);
        assert(mid_tries == 1);
    }

    // 6. Cache namespace isolation on model / source_lang changes.
    {
        std::atomic<int> count{0};
        HttpTransport cache_t = [&](const HttpRequest &, const std::atomic<bool> &,
                                    HttpStreamChunkCallback) -> HttpResponse {
            int n = ++count;
            return {.status_code = 200,
                    .body = "{\"choices\":[{\"message\":{\"content\":\"v" +
                            std::to_string(n) + "\"}}]}"};
        };
        AsyncTranslationEngine engine(cache_t, 50);
        Preset p{
            .id = "shared",
            .provider = ProviderKind::OpenAICompatible,
            .endpoint = "http://127.0.0.1:9000/v1",
            .api_key = "sk-cache-test",
            .model = "model-a",
            .target_lang = "zh",
        };
        auto run_job = [&](uint64_t rev, std::string src_lang, Preset pr) {
            std::mutex m;
            std::condition_variable cv;
            TranslationResult fin;
            bool done = false;
            engine.submit(
                {.revision = rev,
                 .source_text = "hello",
                 .source_lang = std::move(src_lang),
                 .preset = std::move(pr),
                 .immediate = true,
                 .lock_on_punctuation = false,
                 .offline_fallback = false,
                 .debounce_ms = 0,
                 .timeout_ms = 2000},
                [&](uint64_t r, const TranslationResult &res) {
                    if (r != rev || !res.is_final) {
                        return;
                    }
                    std::lock_guard<std::mutex> lk(m);
                    fin = res;
                    done = true;
                    cv.notify_all();
                });
            std::unique_lock<std::mutex> lk(m);
            assert(cv.wait_for(lk, std::chrono::seconds(2), [&]() { return done; }));
            return fin;
        };

        auto a1 = run_job(1, "en", p);
        auto a2 = run_job(2, "en", p); // cache hit
        assert(a1.translated_text == "v1" && a2.translated_text == "v1" && count == 1);

        p.model = "model-b";
        auto b1 = run_job(3, "en", p); // model changed -> miss
        assert(b1.translated_text == "v2" && count == 2);

        auto c1 = run_job(4, "auto", p); // source_lang changed -> miss
        assert(c1.translated_text == "v3" && count == 3);

        p.system_prompt = "Different prompt style.";
        auto d1 = run_job(5, "auto", p); // system_prompt changed -> miss
        assert(d1.translated_text == "v4" && count == 4);

        p.endpoint = "https://other-gateway.example.com/v1";
        auto e1 = run_job(6, "auto", p); // endpoint changed -> miss
        assert(e1.translated_text == "v5" && count == 5);
        engine.shutdown();
    }

    // 7. Direct curlHttpTransport pre-cancel & invalid scheme validation + cancel during retry backoff.
    {
        std::atomic<bool> pre_cancel{true};
        auto c_resp = curlHttpTransport(
            {.url = "https://127.0.0.1:9/v1/chat/completions"}, pre_cancel, {});
        assert(c_resp.cancelled);

        std::atomic<bool> no_cancel{false};
        auto bad_scheme =
            curlHttpTransport({.url = "ftp://127.0.0.1/bad"}, no_cancel, {});
        assert(!bad_scheme.cancelled && !bad_scheme.error_message.empty());

        std::atomic<int> retry_calls{0};
        std::atomic<bool> cancel_during_backoff{false};
        std::thread cancel_trigger;
        HttpTransport rate_limited = [&](const HttpRequest &,
                                         const std::atomic<bool> &,
                                         HttpStreamChunkCallback) -> HttpResponse {
            ++retry_calls;
            // Flip cancel asynchronously once 429 triggers backoff sleep.
            cancel_trigger = std::thread([&cancel_during_backoff]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                cancel_during_backoff.store(true, std::memory_order_relaxed);
            });
            return {.status_code = 429, .body = R"({"error":"slow down"})"};
        };
        Preset p{
            .id = "cancel_retry",
            .provider = ProviderKind::OpenAICompatible,
            .endpoint = "http://127.0.0.1:9000/v1",
            .model = "gpt-4o-mini",
            .target_lang = "en",
        };
        auto rc = translateSingleUnit(p, "zh", "你好", "", 4000,
                                      cancel_during_backoff, rate_limited, {});
        if (cancel_trigger.joinable()) {
            cancel_trigger.join();
        }
        assert(!rc.ok);
        assert(retry_calls.load() == 1);
    }
}

void testOfflineDictionaryExperience() {
    namespace fs = std::filesystem;

    // 1. Config TOML round-trip for offline_dict_path
    {
        ShadowConfig cfg = ShadowConfig::makeDefault();
        cfg.offline_dict_path = "~/.local/share/shadowbar/custom_terms.tsv";
        const std::string toml = ConfigManager::serializeToml(cfg);
        ShadowConfig parsed = ShadowConfig::makeDefault();
        assert(ConfigManager::parseToml(toml, parsed));
        assert(parsed.offline_dict_path == cfg.offline_dict_path);
    }

    // 2. Built-in bidirectional dictionary, punctuation preservation, FMM segmentation,
    //    English inflection lemmatization, mixed tokens/URLs/numbers, and single-char guard.
    OfflineDictionary dict;
    const auto st = dict.stats();
    assert(st.zh_to_en_entries >= 200);
    assert(st.en_to_zh_entries >= 200);

    // Exact & punctuated ZH -> EN
    assert(dict.lookup("谢谢", "en") == "thank you");
    assert(dict.lookup("你好", "en") == "hello");
    assert(dict.lookup("你好！", "en") == "hello!");
    auto zh_sent = dict.lookupDetailed("请帮我看下配置文件", "en");
    assert(zh_sent.has_value());
    assert(zh_sent->coverage() >= 0.8);
    assert(zh_sent->text.find("please") != std::string::npos);
    assert(zh_sent->text.find("please please") == std::string::npos);
    assert(zh_sent->text.find("configuration") != std::string::npos);

    // Technical mixed ZH + EN + URL + version preservation
    auto zh_tech = dict.lookupDetailed("接口报错，请看 https://example.com 和 v2.1.0", "en");
    assert(zh_tech.has_value());
    assert(zh_tech->text.find("https://example.com") != std::string::npos);
    assert(zh_tech->text.find("v2.1.0") != std::string::npos);

    // Exact & inflected EN -> ZH
    assert(dict.lookup("Hello", "zh") == "你好");
    assert(dict.lookup("Hello!", "zh-CN") == "你好！");
    assert(dict.lookup("Permission denied", "zh-CN") == "权限不足");
    assert(dict.lookup("configuration", "zh") == "配置");

    // Morphology: running -> run, dependencies -> dependency, configured -> configure
    assert(dict.lookup("running", "zh").has_value());
    assert(dict.lookup("dependencies", "zh").has_value());
    assert(dict.lookup("configured", "zh").has_value());

    // Full English sentence segmentation with article elision + punctuation
    auto en_sent = dict.lookupDetailed(
        "Please check the configuration and error logs.", "zh-CN");
    assert(en_sent.has_value());
    assert(en_sent->coverage() >= 0.85);
    assert(en_sent->text.find("请") != std::string::npos);
    assert(en_sent->text.find("配置") != std::string::npos);
    assert(en_sent->text.find("错误") != std::string::npos);

    // Grammar-aware ZH -> EN clause rewriting regressions
    {
        auto mod_file = dict.lookupDetailed("该文件已经被修改，请重新加载配置。", "en");
        assert(mod_file.has_value());
        assert(mod_file->text.find("this file has already been modified") != std::string::npos);
        assert(mod_file->text.find("reload") != std::string::npos);

        auto restart = dict.lookupDetailed("并重启应用", "en");
        assert(restart.has_value());
        assert(restart->text.find("and restart") != std::string::npos);

        auto cmp_de = dict.lookupDetailed("这里面的内容比我想象中更复杂", "en");
        assert(cmp_de.has_value());
        assert(cmp_de->text.find("content in here") != std::string::npos);
        assert(cmp_de->text.find("content more complex") == std::string::npos);
        assert(cmp_de->text.find("more complex than") != std::string::npos);

        auto more_people = dict.lookupDetailed("越来越多的人开始", "en");
        assert(more_people.has_value());
        assert(more_people->text.find("more and more") != std::string::npos);
        assert(more_people->text.find(" of more and more") == std::string::npos);
        assert(more_people->text.find("start") != std::string::npos);

        auto ba_send = dict.lookupDetailed("请把错误日志发给我", "en");
        assert(ba_send.has_value());
        const size_t send_pos = ba_send->text.find("send");
        const size_t log_pos = ba_send->text.find("error log");
        assert(send_pos != std::string::npos && log_pos != std::string::npos);
        assert(send_pos < log_pos);

        auto when_faster = dict.lookupDetailed(
            "在悬浮窗中翻译中文段落时，用离线词典速度真的更快吗？", "en");
        assert(when_faster.has_value());
        assert(when_faster->text.rfind("when ", 0) == 0);
        assert(when_faster->text.find("in floating window") != std::string::npos);
        assert(when_faster->text.find("faster") != std::string::npos);
        assert(when_faster->text.find("more fast") == std::string::npos);
    }

    // Grammar-aware EN -> ZH clause rewriting regressions
    {
        auto upd = dict.lookupDetailed(
            "We updated the offline dictionary and fixed two parsing bugs.", "zh-CN");
        assert(upd.has_value());
        assert(upd->text.find("我们更新了") != std::string::npos);
        assert(upd->text.find("并修复了") != std::string::npos);
        assert(upd->text.find("两个") != std::string::npos);

        auto how_reset = dict.lookupDetailed(
            "How do I reset the floating window position and size?", "zh-CN");
        assert(how_reset.has_value());
        assert(how_reset->text.rfind("我如何重置", 0) == 0);

        auto faster_apis = dict.lookupDetailed(
            "The local dictionary responds much faster than cloud APIs.", "zh-CN");
        assert(faster_apis.has_value());
        assert(faster_apis->text.find("快得多") != std::string::npos);
        assert(faster_apis->text.find("比") != std::string::npos);
        assert(faster_apis->text.find("接口") != std::string::npos);

        auto hyphen = dict.lookupDetailed("high-speed", "zh-CN");
        assert(hyphen.has_value());
        assert(hyphen->text.find("速度") != std::string::npos);
    }

    // Multiline preservation
    auto ml = dict.lookupDetailed("你好\n谢谢", "en");
    assert(ml.has_value());
    assert(ml->text == "hello\nthank you");

    // Guard: lone ASCII typing character and mismatched language return nullopt
    assert(!dict.lookup("h", "en").has_value());
    assert(!dict.lookup("h", "zh").has_value());
    assert(!dict.lookup("你好", "ja").has_value());
    assert(!dict.lookup("hello", "en").has_value());
    assert(dict.lookup("apple", "zh") == "苹果");
    assert(dict.lookup("悬浮窗", "en") == "floating window");

    // 3. External TSV, ECDICT CSV, and CC-CEDICT .u8 loading & reload
    const auto tmp_dir =
        fs::temp_directory_path() / "shadowbar_offline_dict_test";
    fs::remove_all(tmp_dir);
    fs::create_directories(tmp_dir);

    const auto tsv_path = tmp_dir / "terms.tsv";
    {
        std::ofstream out(tsv_path);
        out << "# custom glossary\n";
        out << "ShadowBar\t影栏输入法\n";
        out << "检索增强\tRAG pipeline\n";
        // User TSV overrides built-in cleanly
        out << "测试\tsmoke test\n";
    }
    dict.reload(tsv_path.string());
    assert(dict.lookup("ShadowBar", "zh") == "影栏输入法");
    assert(dict.lookup("检索增强", "en") == "RAG pipeline");
    assert(dict.lookup("测试", "en") == "smoke test");

    const auto csv_path = tmp_dir / "ecdict.csv";
    {
        std::ofstream out(csv_path);
        out << "word,phonetic,definition,translation\n";
        out << "ephemeral,ɪ'femərəl,\"lasting short\",\"adj. 短暂的, 瞬息的\\nn. 朝生暮死\"\n";
        out << "idempotent,,\"same result\",\"adj. 幂等的\"\n";
    }
    dict.reload(csv_path.string());
    auto csv_standalone = dict.lookup("ephemeral", "zh-CN");
    assert(csv_standalone.has_value());
    assert(csv_standalone->find("短暂") != std::string::npos);
    auto csv_sent = dict.lookup("An ephemeral token", "zh-CN");
    assert(csv_sent.has_value());
    assert(csv_sent->find("短暂") != std::string::npos);
    assert(csv_sent->find("adj.") == std::string::npos);

    const auto cedict_path = tmp_dir / "cedict_ts.u8";
    {
        std::ofstream out(cedict_path);
        out << "# CC-CEDICT test subset\n";
        out << "計算機 计算机 [ji4 suan4 ji1] /computer/calculator/\n";
    }
    dict.reload(cedict_path.string());
    assert(dict.lookup("计算机", "en") == "computer");
    assert(dict.lookup("計算機", "en") == "computer");

#ifdef SHADOWBAR_HAS_SQLITE3
    // 4. Read-only SQLite ECDICT-schema lookup test
    const auto db_path = tmp_dir / "ecdict.db";
    {
        sqlite3 *db = nullptr;
        assert(sqlite3_open(db_path.c_str(), &db) == SQLITE_OK);
        const char *ddl =
            "CREATE TABLE stardict (id INTEGER PRIMARY KEY, word TEXT, translation TEXT);"
            "INSERT INTO stardict (word, translation) VALUES "
            "('ubiquitous', 'adj. 无处不在的, 普遍存在的'),"
            "('telemetry', 'n. 遥测技术\\n[计] 远程监控'),"
            "('apis', 'n. 蜂属'),"
            "('connect', 'vi. 连接…\\nvt. 连接; 接通');";
        char *err_msg = nullptr;
        assert(sqlite3_exec(db, ddl, nullptr, nullptr, &err_msg) == SQLITE_OK);
        sqlite3_close(db);
    }
    dict.reload(db_path.string());
    assert(dict.stats().sqlite_connected);
    auto sql_hit = dict.lookup("Telemetry", "zh-CN");
    assert(sql_hit.has_value());
    assert(sql_hit->find("遥测") != std::string::npos);
    auto apis_sent = dict.lookupDetailed("Call cloud APIs quickly", "zh-CN");
    assert(apis_sent.has_value());
    assert(apis_sent->text.find("接口") != std::string::npos);
    assert(apis_sent->text.find("蜂属") == std::string::npos);
    auto connect_sent = dict.lookupDetailed("Connect server", "zh-CN");
    assert(connect_sent.has_value());
    assert(connect_sent->text.find("连接") != std::string::npos);
    assert(connect_sent->text.find("…") == std::string::npos);
#endif

    // 5. AsyncTranslationEngine direct Offline preset + coverage-gated fallback + translateSingleUnit
    {
        Preset offline_preset{
            .id = "offline",
            .label = "离线",
            .provider = ProviderKind::Offline,
            .target_lang = "en",
        };
        std::atomic<bool> cancel{false};
        auto direct_res = translateSingleUnit(
            offline_preset, "zh", "你好，世界！", "", 1000, cancel, curlHttpTransport, {});
        assert(direct_res.ok && direct_res.from_offline_dict);
        assert(direct_res.translated_text == "hello, world!");

        std::atomic<int> http_calls{0};
        HttpTransport failing_transport = [&](const HttpRequest &,
                                              const std::atomic<bool> &,
                                              HttpStreamChunkCallback) -> HttpResponse {
            ++http_calls;
            return {.error_message = "network unreachable"};
        };
        AsyncTranslationEngine engine(failing_transport, 100);
        engine.reloadOfflineDictionary(tsv_path.string());

        auto run_sync = [&](const TranslationJob &job) -> TranslationResult {
            std::mutex m;
            std::condition_variable cv;
            TranslationResult last{};
            bool done = false;
            engine.submit(job, [&](uint64_t, const TranslationResult &r) {
                if (!r.is_final) {
                    return;
                }
                std::lock_guard<std::mutex> lk(m);
                last = r;
                done = true;
                cv.notify_all();
            });
            std::unique_lock<std::mutex> lk(m);
            assert(cv.wait_for(lk, std::chrono::seconds(2), [&]() { return done; }));
            return last;
        };

        // Explicit Offline mode hits custom TSV without network
        auto off_custom = run_sync(TranslationJob{
            .revision = 1,
            .source_text = "检索增强",
            .source_lang = "zh",
            .preset = offline_preset,
            .immediate = true,
            .bypass_cache = true,
        });
        assert(off_custom.ok && off_custom.from_offline_dict);
        assert(off_custom.translated_text == "RAG pipeline");
        assert(http_calls.load() == 0);

        Preset off_ja = offline_preset;
        off_ja.target_lang = "ja";
        auto off_unsupported = run_sync(TranslationJob{
            .revision = 10,
            .source_text = "你好",
            .source_lang = "zh",
            .preset = off_ja,
            .immediate = true,
            .bypass_cache = true,
        });
        assert(!off_unsupported.ok);
        assert(off_unsupported.error_message.find("中 ↔ 英") != std::string::npos);

        // Online fallback succeeds when offline dictionary has high coverage
        Preset online_preset{
            .id = "quick",
            .provider = ProviderKind::GoogleFree,
            .target_lang = "en",
        };
        auto high_cov = run_sync(TranslationJob{
            .revision = 2,
            .source_text = "提交代码并运行测试",
            .source_lang = "zh",
            .preset = online_preset,
            .immediate = true,
            .lock_on_punctuation = false,
            .offline_fallback = true,
            .bypass_cache = true,
        });
        assert(high_cov.ok && high_cov.from_offline_dict);

        // Online fallback refuses low-coverage long untranslatable text so real API error surfaces
        auto low_cov_cjk = run_sync(TranslationJob{
            .revision = 3,
            .source_text = "测试 饕餮魑魅魍魉耄耋龃龉",
            .source_lang = "zh",
            .preset = online_preset,
            .immediate = true,
            .lock_on_punctuation = false,
            .offline_fallback = true,
            .bypass_cache = true,
        });
        assert(!low_cov_cjk.ok);
        assert(low_cov_cjk.error_message.find("network unreachable") != std::string::npos);

        Preset online_en_zh{
            .id = "quick_zh",
            .provider = ProviderKind::GoogleFree,
            .target_lang = "zh-CN",
        };
        auto low_cov_en = run_sync(TranslationJob{
            .revision = 4,
            .source_text = "test foo bar baz qux quux corge grault garply",
            .source_lang = "en",
            .preset = online_en_zh,
            .immediate = true,
            .lock_on_punctuation = false,
            .offline_fallback = true,
            .bypass_cache = true,
        });
        assert(!low_cov_en.ok);
        assert(low_cov_en.error_message.find("network unreachable") != std::string::npos);

        engine.shutdown();
    }

    fs::remove_all(tmp_dir);
}

} // namespace

void testHyMtPromptAndConfig() {
    // 1. Model detection & HY-MT1.5 prompt formatting
    assert(isHyMtModelName("hy-mt1.5-1.8b-1.25bit"));
    assert(isHyMtModelName("Hy-MT1.5-1.8B-2bit.gguf"));
    assert(isHyMtModelName("ty_2bit"));
    assert(!isHyMtModelName("gpt-4o-mini"));

    const std::string zh_to_en = buildHyMtUserPrompt("zh", "en", "你好世界");
    assert(zh_to_en ==
           "将以下文本翻译为英语，注意只需要输出翻译后的结果，不要额外解释：\n\n你好世界");

    const std::string en_to_zh = buildHyMtUserPrompt("en", "zh", "Hello world");
    assert(en_to_zh ==
           "将以下文本翻译为中文，注意只需要输出翻译后的结果，不要额外解释：\n\nHello world");

    const std::string auto_zh = buildHyMtUserPrompt("auto", "en", "地铁站在哪里");
    assert(auto_zh.find("将以下文本翻译为英语") == 0);

    const std::string ja_to_en = buildHyMtUserPrompt("ja", "en", "こんにちは");
    assert(ja_to_en ==
           "Translate the following segment into English, without additional explanation.\n\nこんにちは");

    const std::string with_ctx =
        buildHyMtUserPrompt("zh", "en", "二等座也可以", "我想买高铁票");
    assert(with_ctx.find("参考上文（请勿输出）：我想买高铁票\n将以下文本翻译为英语") == 0);

    // 2. [hymt] config roundtrip & validation
    ShadowConfig cfg;
    cfg.hymt_variant = "1.25bit";
    cfg.hymt_backend = "vulkan";
    cfg.hymt_port = 18999;
    cfg.hymt_threads = 8;
    cfg.hymt_mirror = "hf-mirror";
    cfg.hymt_server_path = "/tmp/llama-server";
    cfg.hymt_model_125bit_path = "/tmp/Hy-MT1.5-1.8B-1.25bit.gguf";
    cfg.hymt_model_2bit_path = "/tmp/Hy-MT1.5-1.8B-2bit.gguf";
    cfg.hymt_custom_model_path = "/tmp/custom.gguf";
    cfg.hymt_auto_start = false;

    const std::string dumped = ConfigManager::serializeToml(cfg);
    ShadowConfig parsed;
    std::string err;
    assert(ConfigManager::parseToml(dumped, parsed, &err));
    assert(parsed.hymt_variant == "1.25bit");
    assert(parsed.hymt_backend == "vulkan");
    assert(parsed.hymt_port == 18999);
    assert(parsed.hymt_threads == 8);
    assert(parsed.hymt_mirror == "hf-mirror");
    assert(parsed.hymt_server_path == "/tmp/llama-server");
    assert(parsed.hymt_model_125bit_path == "/tmp/Hy-MT1.5-1.8B-1.25bit.gguf");
    assert(parsed.hymt_model_2bit_path == "/tmp/Hy-MT1.5-1.8B-2bit.gguf");
    assert(parsed.hymt_custom_model_path == "/tmp/custom.gguf");
    assert(parsed.hymt_auto_start == false);

    // 3. Single-turn OpenAI-compatible payload & sampling + special-token stripping
    std::vector<HttpRequest> seen;
    HttpTransport hymt_mock = [&](const HttpRequest &req, const std::atomic<bool> &,
                                  HttpStreamChunkCallback) -> HttpResponse {
        seen.push_back(req);
        return {
            .status_code = 200,
            .body =
                R"({"choices":[{"message":{"content":"Subway station<｜hy_place▁holder▁no▁2｜>"}}]})"};
    };
    Preset ty_preset{
        .id = "ty_2bit",
        .label = "TY 2bit",
        .provider = ProviderKind::OpenAICompatible,
        .endpoint = "http://127.0.0.1:18081/v1/chat/completions",
        .api_key = "",
        .model = "hy-mt1.5-1.8b-2bit",
        .system_prompt = "",
        .target_lang = "en",
    };
    std::atomic<bool> cancel{false};
    auto r = translateSingleUnit(ty_preset, "zh", "地铁站", "", 4000, cancel,
                                 hymt_mock, {});
    assert(r.ok);
    assert(r.translated_text == "Subway station");
    assert(seen.size() == 1);
    const std::string &body = seen[0].body;
    assert(body.find("将以下文本翻译为英语") != std::string::npos);
    assert(body.find("\"role\":\"system\"") == std::string::npos);
    assert(body.find("\"top_p\":0.6") != std::string::npos);
    assert(body.find("\"top_k\":20") != std::string::npos);
    assert(body.find("\"repeat_penalty\":1.05") != std::string::npos);
}

void testMultilangAndDictFormats() {
    namespace fs = std::filesystem;

    // 1. Multi-script detectSourceLang & normalization
    assert(detectSourceLang("안녕하세요") == "ko");
    assert(detectSourceLang("привет мир") == "ru");
    assert(detectSourceLang("مرحبا") == "ar");
    assert(detectSourceLang("สวัสดี") == "th");
    assert(detectSourceLang("नमस्ते") == "hi");
    assert(detectSourceLang("Hello—world") == "en");
    assert(detectSourceLang("C'est très bien") == "auto");
    assert(normalizeLanguageCode("zh-CN") == "zh");
    assert(normalizeLanguageCode("zh-hant") == "zh-TW");
    assert(normalizeLanguageCode("jp") == "ja");
    assert(findLanguageInfo("ko")->zh_name == "韩语");

    // 2. Multi-lang config, Preset follow-global ("") & [dicts.*] TOML round-trip
    ShadowConfig cfg = ShadowConfig::makeDefault();
    cfg.floating_source_lang = "ja";
    cfg.cycle_languages = {"en", "zh", "fr"};
    cfg.presets[0].target_lang = ""; // 跟随全局
    cfg.offline_dicts.push_back(ExternalDictSpec{
        .id = "ja_zh",
        .path = "/tmp/fake_ja_zh.tsv",
        .source_lang = "ja",
        .target_lang = "zh",
        .enabled = true,
        .label = "日汉术语",
    });
    const std::string toml = ConfigManager::serializeToml(cfg);
    ShadowConfig parsed = ShadowConfig::makeDefault();
    std::string parse_err;
    assert(ConfigManager::parseToml(toml, parsed, &parse_err));
    assert(parsed.floating_source_lang == "ja");
    assert((parsed.cycle_languages == std::vector<std::string>{"en", "zh", "fr"}));
    assert(parsed.presets[0].target_lang.empty());
    assert(parsed.offline_dicts.size() == 1);
    assert(parsed.offline_dicts[0].source_lang == "ja");
    assert(parsed.offline_dicts[0].target_lang == "zh");
    assert(parsed.offline_dicts[0].label == "日汉术语");

    // 3. Configurable cycleTargetLang()
    ShadowSession custom_session(parsed);
    custom_session.syncSelectionDefaults("", "en");
    auto c_act1 = custom_session.cycleTargetLang();
    assert(c_act1.update_ui);
    assert(custom_session.viewModel().target_lang == "zh");
    auto c_act2 = custom_session.cycleTargetLang();
    assert(custom_session.viewModel().target_lang == "fr");
    auto c_act3 = custom_session.cycleTargetLang();
    assert(custom_session.viewModel().target_lang == "en");

    // 4. Multi-format / multi-language OfflineDictionary tests in temp dir
    const fs::path tmp_dir =
        fs::temp_directory_path() / "shadowbar_multilang_dict_test";
    fs::remove_all(tmp_dir);
    fs::create_directories(tmp_dir);

    // 4a. TSV with # pair header + concise reverse
    const fs::path tsv_path = tmp_dir / "ja_custom.tsv";
    {
        std::ofstream f(tsv_path);
        f << "# pair: ja->zh\n";
        f << "りんご\t苹果\n";
        f << "こんにちは\t你好\n";
        f << "今日\t今天\n";
    }
    auto tsv_info = OfflineDictionary::inspectFile(tsv_path.string());
    assert(tsv_info.loaded);
    assert(tsv_info.format == "tsv");
    assert(tsv_info.source_lang == "ja");
    assert(tsv_info.target_lang == "zh");
    assert(tsv_info.entry_count == 3);

    // 4b. CSV with ISO language header
    const fs::path csv_path = tmp_dir / "vocab.csv";
    {
        std::ofstream f(csv_path);
        f << "ko,zh\n";
        f << "안녕하세요,你好\n";
        f << "감사합니다,谢谢\n";
    }

    // 4c. JSONL with HTML tags
    const fs::path jsonl_path = tmp_dir / "fr-zh.jsonl";
    {
        std::ofstream f(jsonl_path);
        f << "{\"word\":\"bonjour\",\"translation\":\"<b>你好</b><br><i>问候</i>\"}\n";
        f << "{\"word\":\"merci\",\"translation\":\"谢谢\"}\n";
    }

    // 4d. Binary StarDict (.ifo + .idx + .dict) for de->zh
    const fs::path sd_stem = tmp_dir / "de-zh";
    {
        std::string dict_data = "谢谢";
        std::string idx_data = "danke";
        idx_data.push_back('\0');
        uint32_t off = 0;
        uint32_t sz = static_cast<uint32_t>(dict_data.size());
        auto pushBe32 = [&](uint32_t v) {
            idx_data.push_back(static_cast<char>((v >> 24) & 0xFF));
            idx_data.push_back(static_cast<char>((v >> 16) & 0xFF));
            idx_data.push_back(static_cast<char>((v >> 8) & 0xFF));
            idx_data.push_back(static_cast<char>(v & 0xFF));
        };
        pushBe32(off);
        pushBe32(sz);

        {
            std::ofstream f(fs::path(sd_stem.string() + ".dict"), std::ios::binary);
            f.write(dict_data.data(), static_cast<std::streamsize>(dict_data.size()));
        }
        {
            std::ofstream f(fs::path(sd_stem.string() + ".idx"), std::ios::binary);
            f.write(idx_data.data(), static_cast<std::streamsize>(idx_data.size()));
        }
        {
            std::ofstream f(fs::path(sd_stem.string() + ".ifo"));
            f << "StarDict's dict ifo file\nversion=2.4.2\n"
              << "wordcount=1\nidxfilesize=" << idx_data.size()
              << "\nsametypesequence=h\nbookname=de-zh\n";
        }
    }
    const std::string sd_ifo_path = sd_stem.string() + ".ifo";
    auto sd_info = OfflineDictionary::inspectFile(sd_ifo_path);
    assert(sd_info.loaded);
    assert(sd_info.format == "stardict");
    assert(sd_info.source_lang == "de");
    assert(sd_info.target_lang == "zh");
    assert(sd_info.entry_count == 1);

    std::vector<ExternalDictSpec> specs = {
        {.id = "ja", .path = tsv_path.string(), .source_lang = "auto", .target_lang = "auto", .enabled = true, .label = "ja"},
        {.id = "ko", .path = csv_path.string(), .source_lang = "auto", .target_lang = "auto", .enabled = true, .label = "ko"},
        {.id = "fr", .path = jsonl_path.string(), .source_lang = "fr", .target_lang = "zh", .enabled = true, .label = "fr"},
        {.id = "de", .path = sd_ifo_path, .source_lang = "auto", .target_lang = "auto", .enabled = true, .label = "de"},
    };

#ifdef SHADOWBAR_HAS_SQLITE3
    const fs::path db_ru = tmp_dir / "ru-zh.sqlite";
    sqlite3 *raw_db = nullptr;
    assert(sqlite3_open(db_ru.c_str(), &raw_db) == SQLITE_OK);
    assert(sqlite3_exec(raw_db,
                        "CREATE TABLE dict(word TEXT, translation TEXT);"
                        "INSERT INTO dict VALUES('привет', '你好');",
                        nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw_db);
    specs.push_back({.id = "ru", .path = db_ru.string(), .source_lang = "ru", .target_lang = "zh", .enabled = true, .label = "ru"});
#endif

    OfflineDictionary dict;
    dict.reload(specs);
    auto st = dict.stats();
    assert(st.multi_lang_entries >= 7);

    // Check forward & concise reverse lookup across all languages
    auto hit_ja = dict.lookup("りんご", "zh");
    assert(hit_ja && *hit_ja == "苹果");
    auto hit_zh_ja = dict.lookup("苹果", "ja", "zh");
    assert(hit_zh_ja && *hit_zh_ja == "りんご");
    auto hit_ko = dict.lookup("감사합니다", "zh");
    assert(hit_ko && *hit_ko == "谢谢");
    auto hit_fr_auto = dict.lookup("bonjour", "zh", "auto");
    assert(hit_fr_auto && hit_fr_auto->find("你好") != std::string::npos);
    auto hit_de = dict.lookup("danke", "zh", "de");
    assert(hit_de && *hit_de == "谢谢");
#ifdef SHADOWBAR_HAS_SQLITE3
    auto hit_ru = dict.lookup("привет", "zh", "ru");
    assert(hit_ru && *hit_ru == "你好");
#endif

    // Check AsyncTranslationEngine::reloadOfflineDictionaries
    AsyncTranslationEngine engine;
    engine.reloadOfflineDictionaries(specs);
    assert(engine.offlineDictStats().multi_lang_entries >= 7);

    fs::remove_all(tmp_dir);
}

int main() {
    testUtf8Helpers();
    testConfigTomlAndSecrets();
    testSentenceSplitter();
    testLruCache();
    testSessionStateMachine();
    testAsyncTranslatorStreamingAndClauseLock();
    testFloatingConfigAndDirection();
    testAiApiOptimizations();
    testOfflineDictionaryExperience();
    testHyMtPromptAndConfig();
    testMultilangAndDictFormats();
    std::cout << "ALL shadowbar_core tests PASSED.\n";
    return 0;
}
