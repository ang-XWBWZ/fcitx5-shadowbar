#include "shadowbar/translator.hpp"
#include "shadowbar/config.hpp"
#include "shadowbar/sentence_splitter.hpp"
#include "shadowbar/utf8_util.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace shadowbar {

namespace {

std::once_flag g_curl_init_once;

void ensureCurlInit() {
    std::call_once(g_curl_init_once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct CurlWriteContext {
    std::string body;
    HttpStreamChunkCallback on_chunk;
};

size_t curlWriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    const size_t bytes = size * nmemb;
    auto *ctx = static_cast<CurlWriteContext *>(userdata);
    if (bytes > 0) {
        ctx->body.append(ptr, bytes);
        if (ctx->on_chunk) {
            ctx->on_chunk(std::string_view(ptr, bytes));
        }
    }
    return bytes;
}

int curlXferCallback(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto *cancel = static_cast<const std::atomic<bool> *>(clientp);
    if (cancel && cancel->load(std::memory_order_relaxed)) {
        return 1; // Non-zero aborts curl_easy_perform immediately.
    }
    return 0;
}

std::string urlEncode(std::string_view value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << static_cast<char>(c);
        } else {
            escaped << std::uppercase << '%' << std::setw(2)
                    << static_cast<int>(c) << std::nouppercase;
        }
    }
    return escaped.str();
}

char asciiLowerChar(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool isAsciiDigitChar(char c) {
    return c >= '0' && c <= '9';
}

std::string asciiLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(asciiLowerChar(c));
    }
    return out;
}

bool startsWithIgnoreCase(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (asciiLowerChar(s[i]) != asciiLowerChar(prefix[i])) {
            return false;
        }
    }
    return true;
}

bool containsIgnoreCase(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    return asciiLower(haystack).find(asciiLower(needle)) != std::string::npos;
}

bool endsWithPath(std::string_view path, std::string_view suffix) {
    return path.size() >= suffix.size() &&
           path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void splitUrlPathAndQuery(std::string_view url,
                          std::string &base_out,
                          std::string &suffix_out) {
    const size_t q_pos = url.find_first_of("?#");
    if (q_pos == std::string_view::npos) {
        base_out = std::string(url);
        suffix_out.clear();
    } else {
        base_out = std::string(url.substr(0, q_pos));
        suffix_out = std::string(url.substr(q_pos));
    }
    while (!base_out.empty() && base_out.back() == '/') {
        base_out.pop_back();
    }
}

bool hasExplicitScheme(std::string_view url) {
    return url.find("://") != std::string_view::npos;
}

bool isHttpOrHttpsUrl(std::string_view url) {
    return startsWithIgnoreCase(url, "http://") ||
           startsWithIgnoreCase(url, "https://");
}

bool isLocalOrPrivateEndpoint(std::string_view url) {
    std::string lower = asciiLower(url);
    return lower.find("127.0.0.1") != std::string::npos ||
           lower.find("localhost") != std::string::npos ||
           lower.find("[::1]") != std::string::npos ||
           lower.find("0.0.0.0") != std::string::npos ||
           lower.find("://192.168.") != std::string::npos ||
           lower.find("192.168.") == 0 ||
           lower.find("://10.") != std::string::npos ||
           lower.find("10.") == 0 ||
           lower.find(".local") != std::string::npos;
}

bool isLocalhostEndpoint(std::string_view url) {
    return isLocalOrPrivateEndpoint(url);
}

std::string ensureUrlScheme(std::string url, bool prefer_http_for_local) {
    url = utf8::trim(url);
    if (url.empty() || hasExplicitScheme(url)) {
        return url;
    }
    if (prefer_http_for_local && isLocalOrPrivateEndpoint(url)) {
        return "http://" + url;
    }
    if (url.find('.') != std::string::npos || url.find(':') != std::string::npos) {
        return (isLocalhostEndpoint(url) ? "http://" : "https://") + url;
    }
    return url;
}

bool isVersionPathSegment(std::string_view seg) {
    if (seg.size() < 2 || asciiLowerChar(seg[0]) != 'v' ||
        !isAsciiDigitChar(seg[1])) {
        return false;
    }
    // Accept v1, v2, v3, v4, v1beta, v2alpha, etc.
    for (size_t i = 2; i < seg.size(); ++i) {
        const char c = asciiLowerChar(seg[i]);
        if (!isAsciiDigitChar(c) && (c < 'a' || c > 'z')) {
            return false;
        }
    }
    return true;
}

std::string lastPathSegment(std::string_view url_no_query) {
    const size_t scheme_end = url_no_query.find("://");
    const size_t host_start =
        (scheme_end == std::string_view::npos) ? 0 : (scheme_end + 3);
    const size_t first_slash = url_no_query.find('/', host_start);
    if (first_slash == std::string_view::npos) {
        return {};
    }
    const size_t last_slash = url_no_query.rfind('/');
    return std::string(url_no_query.substr(last_slash + 1));
}

std::string normalizeOpenAiEndpoint(std::string endpoint) {
    endpoint = ensureUrlScheme(std::move(endpoint), true);
    if (endpoint.empty()) {
        return "https://api.openai.com/v1/chat/completions";
    }
    std::string base;
    std::string query;
    splitUrlPathAndQuery(endpoint, base, query);
    if (endsWithPath(base, "/chat/completions")) {
        return base + query;
    }
    const std::string last_seg = lastPathSegment(base);
    if (isVersionPathSegment(last_seg) ||
        asciiLower(last_seg) == "openai" ||
        base.find("/openai/deployments/") != std::string::npos) {
        return base + "/chat/completions" + query;
    }
    return base + "/v1/chat/completions" + query;
}

bool isOllamaUsingOpenAiCompat(std::string_view endpoint) {
    endpoint = utf8::trimView(endpoint);
    if (endpoint.empty()) {
        return false;
    }
    std::string base;
    std::string query;
    splitUrlPathAndQuery(endpoint, base, query);
    return endsWithPath(base, "/v1") || endsWithPath(base, "/chat/completions");
}

std::string normalizeOllamaEndpoint(std::string endpoint) {
    endpoint = ensureUrlScheme(std::move(endpoint), true);
    if (endpoint.empty()) {
        return "http://127.0.0.1:11434/api/chat";
    }
    std::string base;
    std::string query;
    splitUrlPathAndQuery(endpoint, base, query);
    if (endsWithPath(base, "/api/chat")) {
        return base + query;
    }
    if (endsWithPath(base, "/api/generate")) {
        base.resize(base.size() - 13);
        return base + "/api/chat" + query;
    }
    if (endsWithPath(base, "/v1") || endsWithPath(base, "/chat/completions")) {
        return normalizeOpenAiEndpoint(base + query);
    }
    return base + "/api/chat" + query;
}

std::string normalizeDeepLEndpoint(std::string endpoint, std::string_view api_key) {
    endpoint = ensureUrlScheme(std::move(endpoint), false);
    if (endpoint.empty()) {
        const bool is_free = endsWithPath(utf8::trimView(api_key), ":fx") ||
                             utf8::trimView(api_key).empty();
        return is_free ? "https://api-free.deepl.com/v2/translate"
                       : "https://api.deepl.com/v2/translate";
    }
    std::string base;
    std::string query;
    splitUrlPathAndQuery(endpoint, base, query);
    if (endsWithPath(base, "/v2/translate")) {
        return base + query;
    }
    if (endsWithPath(base, "/v2")) {
        return base + "/translate" + query;
    }
    return base + "/v2/translate" + query;
}

bool isAzureOpenAiEndpoint(std::string_view url) {
    return containsIgnoreCase(url, "openai.azure.com") ||
           containsIgnoreCase(url, "cognitiveservices.azure.com") ||
           containsIgnoreCase(url, "api-version=");
}

bool isKnownCloudHostRequiringKey(std::string_view url) {
    const std::string lower = asciiLower(url);
    return lower.find("api.openai.com") != std::string::npos ||
           lower.find("api.deepseek.com") != std::string::npos ||
           lower.find("openrouter.ai") != std::string::npos ||
           lower.find("api.groq.com") != std::string::npos ||
           lower.find("api.siliconflow.cn") != std::string::npos ||
           lower.find("dashscope.aliyuncs.com") != std::string::npos ||
           lower.find("ark.cn-") != std::string::npos ||
           lower.find("open.bigmodel.cn") != std::string::npos ||
           lower.find("generativelanguage.googleapis.com") != std::string::npos;
}

std::string trimIncompleteUtf8Suffix(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    size_t i = s.size();
    size_t cont = 0;
    while (i > 0 && cont < 4) {
        const unsigned char b = static_cast<unsigned char>(s[i - 1]);
        if ((b & 0x80) == 0) {
            return std::string(s);
        }
        if ((b & 0xC0) == 0x80) {
            ++cont;
            --i;
            continue;
        }
        size_t expected = 1;
        if ((b & 0xE0) == 0xC0) {
            expected = 2;
        } else if ((b & 0xF0) == 0xE0) {
            expected = 3;
        } else if ((b & 0xF8) == 0xF0) {
            expected = 4;
        }
        const size_t have = s.size() - (i - 1);
        if (have < expected) {
            return std::string(s.substr(0, i - 1));
        }
        return std::string(s);
    }
    return std::string(s);
}

std::string truncateUtf8Safe(std::string_view s, size_t max_bytes) {
    if (s.size() <= max_bytes) {
        return std::string(s);
    }
    std::string cut = trimIncompleteUtf8Suffix(s.substr(0, max_bytes));
    return cut + "...";
}

std::string extractJsonErrorField(const nlohmann::json &j) {
    if (!j.is_object()) {
        return {};
    }
    if (j.contains("error")) {
        const auto &e = j["error"];
        if (e.is_string()) {
            return utf8::trim(e.get<std::string>());
        }
        if (e.is_object()) {
            std::string msg;
            if (e.contains("message") && e["message"].is_string()) {
                msg = utf8::trim(e["message"].get<std::string>());
            } else if (e.contains("msg") && e["msg"].is_string()) {
                msg = utf8::trim(e["msg"].get<std::string>());
            } else if (e.contains("type") && e["type"].is_string()) {
                msg = utf8::trim(e["type"].get<std::string>());
            }
            std::string code;
            if (e.contains("code") && e["code"].is_string()) {
                code = utf8::trim(e["code"].get<std::string>());
            } else if (e.contains("code") && e["code"].is_number_integer()) {
                code = std::to_string(e["code"].get<long long>());
            }
            if (!code.empty() && !msg.empty() &&
                msg.find(code) == std::string::npos) {
                return "[" + code + "] " + msg;
            }
            if (!msg.empty()) {
                return msg;
            }
            if (!code.empty()) {
                return code;
            }
        }
    }
    for (const char *field : {"message", "detail", "msg"}) {
        if (j.contains(field) && j[field].is_string()) {
            std::string v = utf8::trim(j[field].get<std::string>());
            if (!v.empty()) {
                return v;
            }
        }
    }
    return {};
}

std::string extractApiErrorMessage(std::string_view provider_tag,
                                   long status_code,
                                   std::string_view body,
                                   std::string_view transport_error) {
    std::string detail;
    const std::string_view trimmed_body = utf8::trimView(body);
    if (!trimmed_body.empty()) {
        if (trimmed_body.front() == '{') {
            try {
                auto j = nlohmann::json::parse(trimmed_body);
                detail = extractJsonErrorField(j);
            } catch (...) {
            }
        }
        if (detail.empty() && !startsWithIgnoreCase(trimmed_body, "<!doctype") &&
            !startsWithIgnoreCase(trimmed_body, "<html")) {
            detail = std::string(trimmed_body);
        }
    }
    if (detail.empty()) {
        detail = std::string(utf8::trimView(transport_error));
    }
    detail = truncateUtf8Safe(detail, 220);

    std::string hint;
    if (status_code == 401 || status_code == 403) {
        hint = "鉴权失败，请检查 API Key 或权限";
    } else if (status_code == 404) {
        hint = "接口路径或模型不存在，请核对 API 地址与模型名";
    } else if (status_code == 429 || status_code == 456) {
        hint = "请求频率受限或额度已用尽";
    } else if (status_code == 408 || status_code == 504) {
        hint = "网关或上游响应超时";
    } else if (status_code >= 500) {
        hint = "服务端暂时异常";
    }

    std::ostringstream out;
    if (!provider_tag.empty()) {
        out << provider_tag << " ";
    }
    if (status_code > 0) {
        out << "HTTP " << status_code;
        if (!hint.empty()) {
            out << " (" << hint << ")";
        }
    } else if (!hint.empty()) {
        out << hint;
    }
    if (!detail.empty()) {
        if (out.tellp() > 0) {
            out << ": ";
        }
        out << detail;
    }
    return out.str();
}

std::string stripXmlBlockCaseInsensitive(std::string text,
                                         std::string_view open_tag,
                                         std::string_view close_tag,
                                         bool hide_unclosed_tail) {
    while (true) {
        const std::string lower = asciiLower(text);
        const size_t start = lower.find(open_tag);
        if (start == std::string::npos) {
            break;
        }
        const size_t close_start =
            lower.find(close_tag, start + open_tag.size());
        if (close_start == std::string::npos) {
            if (hide_unclosed_tail) {
                text.erase(start);
            }
            break;
        }
        const size_t end = close_start + close_tag.size();
        text.erase(start, end - start);
    }
    return text;
}

std::string stripKnownSpecialTokens(std::string text) {
    static constexpr std::string_view kSpecialTokens[] = {
        "<｜hy_place▁holder▁no▁2｜>",
        "<｜hy_place▁holder▁no▁1｜>",
        "<｜hy_Assistant｜>",
        "<｜hy_User｜>",
        "<｜startoftext｜>",
        "<|im_end|>",
        "<|endoftext|>",
    };
    for (std::string_view tok : kSpecialTokens) {
        size_t pos = 0;
        while ((pos = text.find(tok, pos)) != std::string::npos) {
            text.erase(pos, tok.size());
        }
    }
    return text;
}

std::string sanitizeTranslationOutput(std::string_view raw, bool is_partial) {
    std::string text(raw);
    text = stripXmlBlockCaseInsensitive(std::move(text), "<think>", "</think>", true);
    text = stripXmlBlockCaseInsensitive(std::move(text), "<thinking>", "</thinking>",
                                        true);
    text = stripXmlBlockCaseInsensitive(std::move(text), "<thought>", "</thought>",
                                        true);
    text = stripXmlBlockCaseInsensitive(std::move(text), "<reasoning>",
                                        "</reasoning>", true);
    text = stripKnownSpecialTokens(std::move(text));
    if (is_partial) {
        text = trimIncompleteUtf8Suffix(text);
    }
    text = utf8::trim(text);
    if (!is_partial && text.size() >= 6 && text.rfind("```", 0) == 0 &&
        text.compare(text.size() - 3, 3, "```") == 0) {
        const size_t first_nl = text.find('\n');
        if (first_nl != std::string::npos && text.size() > first_nl + 4) {
            text = utf8::trim(
                text.substr(first_nl + 1, text.size() - first_nl - 4));
        }
    }
    return text;
}

std::string extractTextFromContentField(const nlohmann::json &content) {
    if (content.is_string()) {
        return content.get<std::string>();
    }
    if (content.is_array()) {
        std::string merged;
        for (const auto &part : content) {
            if (part.is_string()) {
                merged += part.get<std::string>();
            } else if (part.is_object()) {
                if (part.contains("text") && part["text"].is_string()) {
                    merged += part["text"].get<std::string>();
                } else if (part.contains("content") && part["content"].is_string()) {
                    merged += part["content"].get<std::string>();
                }
            }
        }
        return merged;
    }
    return {};
}

std::string extractLlmContentFromJson(const nlohmann::json &j) {
    if (!j.is_object()) {
        return {};
    }
    if (j.contains("choices") && j["choices"].is_array() &&
        !j["choices"].empty()) {
        const auto &c0 = j["choices"][0];
        if (c0.is_object()) {
            if (c0.contains("message") && c0["message"].is_object() &&
                c0["message"].contains("content")) {
                std::string t =
                    extractTextFromContentField(c0["message"]["content"]);
                if (!t.empty()) {
                    return t;
                }
            }
            if (c0.contains("delta") && c0["delta"].is_object() &&
                c0["delta"].contains("content")) {
                std::string t =
                    extractTextFromContentField(c0["delta"]["content"]);
                if (!t.empty()) {
                    return t;
                }
            }
            if (c0.contains("text") && c0["text"].is_string()) {
                return c0["text"].get<std::string>();
            }
        }
    }
    if (j.contains("message") && j["message"].is_object() &&
        j["message"].contains("content")) {
        return extractTextFromContentField(j["message"]["content"]);
    }
    if (j.contains("response") && j["response"].is_string()) {
        return j["response"].get<std::string>();
    }
    return {};
}

bool shouldRetryStreamAsNonStream(long status_code,
                                  std::string_view err_msg,
                                  std::string_view raw_body) {
    if (status_code != 400 && status_code != 404 && status_code != 415 &&
        status_code != 422) {
        return false;
    }
    return containsIgnoreCase(err_msg, "stream") ||
           containsIgnoreCase(raw_body, "stream") ||
           containsIgnoreCase(raw_body, "sse");
}

bool isRetryableHttpOrTransportFailure(const HttpResponse &resp) {
    if (resp.cancelled) {
        return false;
    }
    if (resp.status_code == 0) {
        return true;
    }
    return resp.status_code == 408 || resp.status_code == 429 ||
           resp.status_code == 502 || resp.status_code == 503 ||
           resp.status_code == 504;
}

uint64_t fnv1a64(std::string_view s) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::string makeCacheScope(const Preset &preset, std::string_view source_lang) {
    std::string endpoint_key;
    switch (preset.provider) {
    case ProviderKind::OpenAICompatible:
        endpoint_key = normalizeOpenAiEndpoint(preset.endpoint);
        break;
    case ProviderKind::Ollama:
        endpoint_key = normalizeOllamaEndpoint(preset.endpoint);
        break;
    case ProviderKind::DeepL:
        endpoint_key = normalizeDeepLEndpoint(preset.endpoint, "");
        break;
    case ProviderKind::GoogleFree:
    case ProviderKind::Offline:
        break;
    }
    const std::string src_key =
        source_lang.empty() ? std::string("auto") : asciiLower(source_lang);
    std::string scope;
    scope.reserve(preset.id.size() + endpoint_key.size() + preset.model.size() +
                  src_key.size() + 48);
    scope.append(preset.id);
    scope.push_back('|');
    scope.append(std::to_string(static_cast<int>(preset.provider)));
    scope.push_back('|');
    scope.append(endpoint_key);
    scope.push_back('|');
    scope.append(preset.model);
    scope.push_back('|');
    scope.append(src_key);
    scope.push_back('|');
    scope.append(std::to_string(fnv1a64(preset.system_prompt)));
    return scope;
}

struct LanguageNames {
    std::string_view en;
    std::string_view zh;
};

LanguageNames lookupLanguageNames(std::string_view code) {
    const std::string lower = asciiLower(utf8::trimView(code));
    if (lower.empty() || lower == "auto") {
        return {"auto-detected language", "自动检测语言"};
    }
    if (const LanguageInfo *info = findLanguageInfo(code)) {
        return {info->en_name, info->zh_name};
    }
    return {code, code};
}

std::string languageDisplayName(std::string_view code) {
    return std::string(lookupLanguageNames(code).en);
}

std::string languageZhDisplayName(std::string_view code) {
    return std::string(lookupLanguageNames(code).zh);
}

bool isChineseLangCode(std::string_view code) {
    const std::string lower = asciiLower(utf8::trimView(code));
    return lower == "zh" || lower.rfind("zh-", 0) == 0;
}

bool containsCjkCharacters(std::string_view text) {
    const unsigned char *p = reinterpret_cast<const unsigned char *>(text.data());
    for (size_t i = 0; i + 2 < text.size(); ++i) {
        if (p[i] >= 0xE4 && p[i] <= 0xE9 && (p[i + 1] & 0xC0) == 0x80 &&
            (p[i + 2] & 0xC0) == 0x80) {
            return true;
        }
    }
    return false;
}

std::string buildLlmUserPrompt(std::string_view source_lang,
                               std::string_view target_lang,
                               std::string_view text,
                               std::string_view readonly_context) {
    std::ostringstream out;
    const std::string norm_src = asciiLower(utf8::trimView(source_lang));
    if (norm_src.empty() || norm_src == "auto") {
        out << "Detect the source language automatically and translate into "
            << languageDisplayName(target_lang) << ".\n";
    } else {
        out << "Translate from " << languageDisplayName(source_lang) << " to "
            << languageDisplayName(target_lang) << ".\n";
    }
    if (!readonly_context.empty()) {
        out << "Previous context (do NOT repeat in output): "
            << readonly_context << "\n";
    }
    out << "Text to translate:\n" << text;
    return out.str();
}

std::string cleanModelOutput(std::string_view raw) {
    std::string stripped = stripReasoningTags(raw);
    return utf8::trim(stripped);
}

TranslationResult translateGoogleFree(const Preset &preset,
                                      std::string_view source_lang,
                                      std::string_view text,
                                      int timeout_ms,
                                      const std::atomic<bool> &cancel_flag,
                                      const HttpTransport &transport) {
    std::string sl = source_lang.empty() ? "auto" : std::string(source_lang);
    std::string tl = preset.target_lang.empty() ? "en" : preset.target_lang;

    HttpRequest req;
    req.method = "GET";
    req.timeout_ms = timeout_ms;
    req.url = "https://translate.googleapis.com/translate_a/single?client=gtx&sl=" +
              urlEncode(sl) + "&tl=" + urlEncode(tl) + "&dt=t&q=" + urlEncode(text);
    req.headers = {{"User-Agent", "ShadowBar/0.1"}};

    HttpResponse resp = transport(req, cancel_flag, {});
    if (resp.cancelled) {
        return {.ok = false, .is_final = true, .error_message = "cancelled"};
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        return {
            .ok = false,
            .is_final = true,
            .error_message = extractApiErrorMessage(
                "", resp.status_code, resp.body, resp.error_message),
        };
    }

    try {
        auto json = nlohmann::json::parse(resp.body);
        std::string merged;
        if (json.is_array() && !json.empty() && json[0].is_array()) {
            for (const auto &seg : json[0]) {
                if (seg.is_array() && !seg.empty() && seg[0].is_string()) {
                    merged += seg[0].get<std::string>();
                }
            }
        }
        merged = utf8::trim(merged);
        if (merged.empty()) {
            return {.ok = false, .is_final = true, .error_message = "空译文响应"};
        }
        return {.ok = true, .is_final = true, .translated_text = std::move(merged)};
    } catch (const std::exception &e) {
        return {.ok = false, .is_final = true, .error_message = e.what()};
    }
}

TranslationResult translateDeepL(const Preset &preset,
                                 std::string_view source_lang,
                                 std::string_view text,
                                 int timeout_ms,
                                 const std::atomic<bool> &cancel_flag,
                                 const HttpTransport &transport) {
    const std::string key = ConfigManager::resolveSecretValue(preset.api_key);
    if (key.empty()) {
        return {.ok = false,
                .is_final = true,
                .error_message = "DeepL 未配置 API Key"};
    }

    std::string url = utf8::trim(preset.endpoint);
    if (url.empty()) {
        url = (key.ends_with(":fx")) ? "https://api-free.deepl.com/v2/translate"
                                     : "https://api.deepl.com/v2/translate";
    }

    std::string target = preset.target_lang.empty() ? "EN" : preset.target_lang;
    std::transform(target.begin(), target.end(), target.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    nlohmann::json payload;
    payload["text"] = nlohmann::json::array({std::string(text)});
    payload["target_lang"] = target;
    if (!source_lang.empty() && source_lang != "auto") {
        std::string src(source_lang);
        std::transform(src.begin(), src.end(), src.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        payload["source_lang"] = src;
    }

    HttpRequest req;
    req.method = "POST";
    req.url = std::move(url);
    req.timeout_ms = timeout_ms;
    req.headers = {
        {"Authorization", "DeepL-Auth-Key " + key},
        {"Content-Type", "application/json"},
    };
    req.body = payload.dump();

    HttpResponse resp = transport(req, cancel_flag, {});
    if (resp.cancelled) {
        return {.ok = false, .is_final = true, .error_message = "cancelled"};
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        return {
            .ok = false,
            .is_final = true,
            .error_message = extractApiErrorMessage(
                "DeepL", resp.status_code, resp.body, resp.error_message),
        };
    }

    try {
        auto json = nlohmann::json::parse(resp.body);
        if (json.contains("translations") && json["translations"].is_array() &&
            !json["translations"].empty()) {
            std::string out = json["translations"][0].value("text", "");
            out = utf8::trim(out);
            if (!out.empty()) {
                return {.ok = true, .is_final = true, .translated_text = std::move(out)};
            }
        }
        return {.ok = false, .is_final = true, .error_message = "DeepL 返回格式异常"};
    } catch (const std::exception &e) {
        return {.ok = false, .is_final = true, .error_message = e.what()};
    }
}

TranslationResult translateOpenAiOrOllama(
    const Preset &preset,
    std::string_view source_lang,
    std::string_view text,
    std::string_view readonly_context,
    int timeout_ms,
    const std::atomic<bool> &cancel_flag,
    const HttpTransport &transport,
    const std::function<void(std::string_view partial_text)> &on_partial) {
    const bool is_ollama = (preset.provider == ProviderKind::Ollama);
    const std::string url = is_ollama ? normalizeOllamaEndpoint(preset.endpoint)
                                      : normalizeOpenAiEndpoint(preset.endpoint);
    const std::string key = ConfigManager::resolveSecretValue(preset.api_key);

    if (!is_ollama && key.empty() && !isLocalhostEndpoint(url)) {
        return {.ok = false,
                .is_final = true,
                .error_message = "未配置 API Key（请设置环境变量或 config.toml）"};
    }

    const std::string model = preset.model.empty()
                                  ? (is_ollama ? "qwen2.5:3b" : "gpt-4o-mini")
                                  : preset.model;
    const bool is_hymt = isHyMtModelName(model);

    nlohmann::json payload;
    payload["model"] = model;
    payload["stream"] = true;
    if (is_hymt) {
        nlohmann::json messages = nlohmann::json::array();
        const std::string trimmed_sys = utf8::trim(preset.system_prompt);
        if (!trimmed_sys.empty()) {
            messages.push_back({{"role", "system"}, {"content", trimmed_sys}});
        }
        messages.push_back(
            {{"role", "user"},
             {"content", buildHyMtUserPrompt(source_lang, preset.target_lang, text,
                                             readonly_context)}});
        payload["messages"] = std::move(messages);
        if (is_ollama) {
            payload["think"] = false;
            payload["options"] = {{"temperature", 0.2},
                                  {"top_p", 0.6},
                                  {"top_k", 20},
                                  {"repeat_penalty", 1.05}};
        } else {
            payload["temperature"] = 0.2;
            payload["top_p"] = 0.6;
            payload["top_k"] = 20;
            payload["repeat_penalty"] = 1.05;
        }
    } else {
        const std::string sys_prompt =
            preset.system_prompt.empty()
                ? "Translate accurately and naturally. Output only the translation."
                : preset.system_prompt;

        std::string effective_sys = sys_prompt;
        if (!preset.target_lang.empty()) {
            effective_sys +=
                "\nImportant: Always translate into " +
                languageDisplayName(preset.target_lang) +
                " (overriding any default target language above if different). "
                "Preserve line breaks and code/placeholders, and output ONLY the "
                "translated text without quotes, labels, or explanations.";
        }

        payload["messages"] = nlohmann::json::array({
            {{"role", "system"}, {"content", effective_sys}},
            {{"role", "user"},
             {"content", buildLlmUserPrompt(source_lang, preset.target_lang, text,
                                            readonly_context)}},
        });
        if (is_ollama) {
            payload["think"] = false;
            payload["options"] = {{"temperature", 0.2}};
        } else {
            payload["temperature"] = 0.2;
        }
    }

    HttpRequest req;
    req.method = "POST";
    req.url = url;
    req.timeout_ms = timeout_ms;
    req.headers = {{"Content-Type", "application/json"}};
    if (!key.empty()) {
        req.headers.push_back({"Authorization", "Bearer " + key});
    }
    req.body = payload.dump();

    std::string line_buf;
    std::string accumulated;
    bool saw_stream_chunks = false;

    auto consume_line = [&](std::string_view raw_line) {
        std::string_view line = utf8::trimView(raw_line);
        if (line.empty()) {
            return;
        }
        if (!is_ollama) {
            if (line.rfind("data:", 0) != 0) {
                return;
            }
            line = utf8::trimView(line.substr(5));
            if (line == "[DONE]") {
                return;
            }
        }
        try {
            auto j = nlohmann::json::parse(line);
            std::string delta;
            if (is_ollama) {
                if (j.contains("message") && j["message"].is_object() &&
                    j["message"].contains("content") &&
                    j["message"]["content"].is_string()) {
                    delta = j["message"]["content"].get<std::string>();
                }
            } else if (j.contains("choices") && j["choices"].is_array() &&
                       !j["choices"].empty()) {
                const auto &ch = j["choices"][0];
                if (ch.contains("delta") && ch["delta"].is_object() &&
                    ch["delta"].contains("content") &&
                    ch["delta"]["content"].is_string()) {
                    delta = ch["delta"]["content"].get<std::string>();
                } else if (ch.contains("message") && ch["message"].is_object() &&
                           ch["message"].contains("content") &&
                           ch["message"]["content"].is_string()) {
                    delta = ch["message"]["content"].get<std::string>();
                }
            }
            if (!delta.empty()) {
                saw_stream_chunks = true;
                accumulated += delta;
                if (on_partial) {
                    std::string cleaned = cleanModelOutput(accumulated);
                    if (!cleaned.empty()) {
                        on_partial(cleaned);
                    }
                }
            }
        } catch (...) {
            // Ignore malformed keepalive or non-JSON lines; non-stream fallback handles full JSON.
        }
    };

    HttpResponse resp = transport(
        req, cancel_flag, [&](std::string_view chunk) {
            line_buf.append(chunk);
            size_t pos = 0;
            while (true) {
                size_t nl = line_buf.find('\n', pos);
                if (nl == std::string::npos) {
                    break;
                }
                consume_line(std::string_view(line_buf).substr(pos, nl - pos));
                pos = nl + 1;
            }
            if (pos > 0) {
                line_buf.erase(0, pos);
            }
        });

    if (!line_buf.empty()) {
        consume_line(line_buf);
    }

    if (resp.cancelled) {
        return {.ok = false, .is_final = true, .error_message = "cancelled"};
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        return {
            .ok = false,
            .is_final = true,
            .error_message = extractApiErrorMessage(
                "", resp.status_code, resp.body, resp.error_message),
        };
    }

    if (saw_stream_chunks) {
        std::string final_text = cleanModelOutput(accumulated);
        if (!final_text.empty()) {
            return {.ok = true, .is_final = true, .translated_text = std::move(final_text)};
        }
    }

    // Fallback: server responded with single JSON object rather than SSE/NDJSON.
    try {
        auto j = nlohmann::json::parse(resp.body);
        std::string content;
        if (is_ollama && j.contains("message") && j["message"].is_object()) {
            content = j["message"].value("content", "");
        } else if (j.contains("choices") && j["choices"].is_array() &&
                   !j["choices"].empty()) {
            const auto &ch = j["choices"][0];
            if (ch.contains("message") && ch["message"].is_object()) {
                content = ch["message"].value("content", "");
            }
        }
        content = cleanModelOutput(content);
        if (!content.empty()) {
            return {.ok = true, .is_final = true, .translated_text = std::move(content)};
        }
        return {.ok = false, .is_final = true, .error_message = "模型未返回有效译文"};
    } catch (const std::exception &e) {
        return {.ok = false, .is_final = true, .error_message = e.what()};
    }
}

} // namespace

bool isHyMtModelName(std::string_view model_name) {
    const std::string lower = asciiLower(utf8::trimView(model_name));
    if (lower.empty()) {
        return false;
    }
    return lower.find("hy-mt") != std::string::npos ||
           lower.find("hymt") != std::string::npos ||
           lower.find("hy_mt") != std::string::npos ||
           lower.find("hunyuan-mt") != std::string::npos ||
           lower.rfind("ty-", 0) == 0 ||
           lower.rfind("ty_", 0) == 0;
}

std::string buildHyMtUserPrompt(std::string_view source_lang,
                                std::string_view target_lang,
                                std::string_view text,
                                std::string_view readonly_context) {
    const std::string_view effective_target =
        target_lang.empty() ? std::string_view("en") : target_lang;
    const std::string norm_src = asciiLower(utf8::trimView(source_lang));
    const bool use_zh_prompt =
        isChineseLangCode(source_lang) ||
        isChineseLangCode(effective_target) ||
        ((norm_src.empty() || norm_src == "auto") &&
         containsCjkCharacters(text));

    std::ostringstream out;
    if (use_zh_prompt) {
        if (!readonly_context.empty()) {
            out << "参考上文（请勿输出）：" << readonly_context << "\n";
        }
        out << "将以下文本翻译为" << languageZhDisplayName(effective_target)
            << "，注意只需要输出翻译后的结果，不要额外解释：\n\n"
            << text;
    } else {
        if (!readonly_context.empty()) {
            out << "Context (do not output): " << readonly_context << "\n";
        }
        out << "Translate the following segment into "
            << languageDisplayName(effective_target)
            << ", without additional explanation.\n\n"
            << text;
    }
    return out.str();
}

std::string stripReasoningTags(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t open = raw.find("<think>", pos);
        if (open == std::string_view::npos) {
            out.append(raw.substr(pos));
            break;
        }
        out.append(raw.substr(pos, open - pos));
        size_t close = raw.find("</think>", open + 7);
        if (close == std::string_view::npos) {
            // Unclosed `<think>` during streaming: hide partial reasoning block.
            break;
        }
        pos = close + 8;
    }
    return out;
}

std::string formatCurlFailure(CURLcode rc, long timeout_ms) {
    switch (rc) {
    case CURLE_OPERATION_TIMEDOUT:
        return "请求超时 (" + std::to_string(timeout_ms) +
               "ms)，可在设置中调大超时或检查网络/代理";
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
        return "DNS 解析失败，请检查接口域名或代理设置";
    case CURLE_COULDNT_CONNECT:
        return "无法连接服务端，请检查 API 地址、端口或本地服务是否启动";
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CACERT_BADFILE:
        return std::string("TLS/SSL 安全连接失败: ") + curl_easy_strerror(rc);
    case CURLE_GOT_NOTHING:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
        return std::string("网络连接意外中断: ") + curl_easy_strerror(rc);
    default:
        return curl_easy_strerror(rc);
    }
}

bool requestWantsSseStream(const HttpRequest &req) {
    for (const auto &h : req.headers) {
        if (asciiLower(h.name) == "accept" &&
            containsIgnoreCase(h.value, "text/event-stream")) {
            return true;
        }
    }
    return req.method == "POST" &&
           req.body.find("\"stream\":true") != std::string::npos;
}

struct ThreadLocalCurlHandle {
    CURL *handle = nullptr;
    ~ThreadLocalCurlHandle() {
        if (handle) {
            curl_easy_cleanup(handle);
            handle = nullptr;
        }
    }
};

struct CurlResetGuard {
    CURL *handle = nullptr;
    struct curl_slist *headers = nullptr;
    ~CurlResetGuard() {
        if (handle) {
            curl_easy_reset(handle);
        }
        if (headers) {
            curl_slist_free_all(headers);
        }
    }
};

HttpResponse curlHttpTransport(const HttpRequest &req,
                               const std::atomic<bool> &cancel_flag,
                               HttpStreamChunkCallback on_chunk) {
    ensureCurlInit();
    HttpResponse response;

    if (cancel_flag.load(std::memory_order_relaxed)) {
        response.cancelled = true;
        response.error_message = "cancelled";
        return response;
    }

    if (req.url.empty() || !isHttpOrHttpsUrl(req.url)) {
        response.error_message = "无效的 HTTP/HTTPS 接口地址";
        return response;
    }

    // Reuse per-thread CURL handle so TLS sessions, DNS cache, and TCP keepalive
    // sockets survive across consecutive clause & scroll-segment translations.
    thread_local ThreadLocalCurlHandle tl_curl;
    if (!tl_curl.handle) {
        tl_curl.handle = curl_easy_init();
    }
    CURL *curl = tl_curl.handle;
    if (!curl) {
        response.error_message = "Failed to initialize libcurl";
        return response;
    }
    curl_easy_reset(curl);

    CurlWriteContext write_ctx;
    write_ctx.on_chunk = std::move(on_chunk);

    CurlResetGuard reset_guard;
    reset_guard.handle = curl;

    for (const auto &h : req.headers) {
        std::string line = h.name + ": " + h.value;
        reset_guard.headers = curl_slist_append(reset_guard.headers, line.c_str());
    }

    const long total_timeout_ms = static_cast<long>(std::max(500, req.timeout_ms));
    const long connect_timeout_ms = static_cast<long>(
        std::clamp(static_cast<int>(total_timeout_ms / 2),
                   std::min(static_cast<int>(total_timeout_ms), 1500), 6000));

    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, total_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 15L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 5L);
    curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 300L);
#ifdef CURL_HTTP_VERSION_2TLS
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
#endif
    if (!requestWantsSseStream(req)) {
        // Auto-negotiate gzip/br on non-streaming JSON calls; keep SSE uncompressed
        // so reverse proxies flush incremental tokens immediately.
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curlXferCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancel_flag);
    if (reset_guard.headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, reset_guard.headers);
    }
    if (req.method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(req.body.size()));
    }

    CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);
    response.body = std::move(write_ctx.body);

    if (rc == CURLE_ABORTED_BY_CALLBACK ||
        cancel_flag.load(std::memory_order_relaxed)) {
        response.cancelled = true;
        response.error_message = "cancelled";
    } else if (rc != CURLE_OK) {
        response.error_message = formatCurlFailure(rc, total_timeout_ms);
    }

    return response;
}

static bool isOfflineSupportedTargetLang(std::string_view target_lang) {
    std::string lower;
    lower.reserve(target_lang.size());
    for (unsigned char c : utf8::trimView(target_lang)) {
        lower.push_back(static_cast<char>(std::tolower(c)));
    }
    return lower.empty() || lower == "auto" ||
           lower.rfind("en", 0) == 0 || lower.rfind("zh", 0) == 0 ||
           lower.find("english") != std::string::npos ||
           lower.find("chinese") != std::string::npos ||
           target_lang.find("中文") != std::string_view::npos;
}

static TranslationResult runOfflineLookup(const OfflineDictionary &dict,
                                          std::string_view text,
                                          std::string_view source_lang,
                                          std::string_view target_lang) {
    if (auto hit = dict.lookupDetailed(text, target_lang, source_lang);
        hit && hit->matched_units > 0) {
        return {.ok = true,
                .is_final = true,
                .from_offline_dict = true,
                .translated_text = std::move(hit->text)};
    }
    bool supported = isOfflineSupportedTargetLang(target_lang);
    if (!supported) {
        const std::string norm_tgt = normalizeLanguageCode(target_lang);
        const OfflineDictStats st = dict.stats();
        const std::string suffix = "→" + norm_tgt;
        for (const auto &pair : st.supported_pairs) {
            if (pair.size() > suffix.size() &&
                pair.compare(pair.size() - suffix.size(), suffix.size(), suffix) == 0) {
                supported = true;
                break;
            }
        }
    }
    if (!supported) {
        return {.ok = false,
                .is_final = true,
                .error_message =
                    "离线词典目前仅支持 中 ↔ 英 双向及已导入的多语言词库（请切换目标语言为 英文/中文，或在「⚙ 设置 → 通用 → 离线词典」导入对应语种词库）"};
    }
    return {.ok = false,
            .is_final = true,
            .error_message =
                "离线词典未命中（内置词库主要覆盖日常与开发常用词；可在「⚙ 设置 → 通用 → 离线词典」导入 .tsv / .csv / .json / .jsonl / StarDict .ifo / CC-CEDICT .u8 / SQLite 词库）"};
}

TranslationResult translateSingleUnit(
    const Preset &preset,
    std::string_view source_lang,
    std::string_view text,
    std::string_view readonly_context,
    int timeout_ms,
    const std::atomic<bool> &cancel_flag,
    const HttpTransport &transport,
    const std::function<void(std::string_view partial_text)> &on_partial) {
    if (utf8::trimView(text).empty()) {
        return {.ok = true, .is_final = true, .translated_text = ""};
    }
    if (cancel_flag.load(std::memory_order_relaxed)) {
        return {.ok = false, .is_final = true, .error_message = "cancelled"};
    }
    if (preset.provider == ProviderKind::Offline) {
        static const OfflineDictionary kStandaloneOfflineDict;
        return runOfflineLookup(kStandaloneOfflineDict, text, source_lang,
                                preset.target_lang);
    }

    Preset effective = preset;
    const std::string resolved_key =
        ConfigManager::resolveSecretValue(effective.api_key);
    const std::string_view raw_key = utf8::trimView(effective.api_key);
    const bool uses_env_ref =
        raw_key.rfind("env:", 0) == 0 ||
        (raw_key.size() >= 4 && raw_key.rfind("${", 0) == 0 &&
         raw_key.back() == '}');

    if (uses_env_ref && resolved_key.empty() &&
        effective.provider != ProviderKind::GoogleFree) {
        return {.ok = false,
                .is_final = true,
                .error_message = "未找到环境变量密钥: " + std::string(raw_key) +
                                 "（请先设置环境变量或直接填写 API Key）"};
    }

    switch (effective.provider) {
    case ProviderKind::OpenAICompatible:
        effective.endpoint = normalizeOpenAiEndpoint(effective.endpoint);
        if (!isHttpOrHttpsUrl(effective.endpoint)) {
            return {.ok = false,
                    .is_final = true,
                    .error_message = "API 地址格式无效，需以 http:// 或 https:// 开头"};
        }
        if (resolved_key.empty() &&
            isKnownCloudHostRequiringKey(effective.endpoint)) {
            return {.ok = false,
                    .is_final = true,
                    .error_message = "请填写 API Key（支持 env:VAR_NAME）"};
        }
        break;
    case ProviderKind::Ollama:
        if (isOllamaUsingOpenAiCompat(effective.endpoint)) {
            effective.endpoint = normalizeOpenAiEndpoint(effective.endpoint);
            if (!isHttpOrHttpsUrl(effective.endpoint)) {
                return {.ok = false,
                        .is_final = true,
                        .error_message = "Ollama 接口地址格式无效"};
            }
            effective.provider = ProviderKind::OpenAICompatible;
        } else {
            effective.endpoint = normalizeOllamaEndpoint(effective.endpoint);
            if (!isHttpOrHttpsUrl(effective.endpoint)) {
                return {.ok = false,
                        .is_final = true,
                        .error_message = "Ollama 接口地址格式无效"};
            }
        }
        break;
    case ProviderKind::DeepL: {
        if (resolved_key.empty()) {
            return {.ok = false,
                    .is_final = true,
                    .error_message = "DeepL 需要填写 API Key（支持 env:DEEPL_API_KEY）"};
        }
        effective.endpoint = normalizeDeepLEndpoint(effective.endpoint, resolved_key);
        if (!isHttpOrHttpsUrl(effective.endpoint)) {
            return {.ok = false,
                    .is_final = true,
                    .error_message = "DeepL 接口地址格式无效"};
        }
        const std::string lower_tgt = asciiLower(effective.target_lang);
        if (lower_tgt == "zh-tw" || lower_tgt == "zh-hant" ||
            lower_tgt == "zh-hk") {
            effective.target_lang = "ZH-HANT";
        } else if (lower_tgt == "zh-cn" || lower_tgt == "zh-hans") {
            effective.target_lang = "ZH";
        }
        break;
    }
    case ProviderKind::GoogleFree:
    case ProviderKind::Offline:
        break;
    }

    const int budget_ms = std::max(500, timeout_ms);
    const auto overall_deadline =
        SteadyClock::now() + std::chrono::milliseconds(budget_ms);

    struct InterceptState {
        HttpRequest last_request;
        HttpResponse last_response;
        std::string captured_raw;
        std::string inline_error;
        bool saw_sse_data = false;
        bool saw_content_delta = false;
        bool emitted_partial_tokens = false;
    } intercept;

    auto enrich_headers = [](HttpRequest &req) {
        bool has_accept = false;
        bool has_ua = false;
        bool has_api_key = false;
        std::string bearer;
        for (const auto &h : req.headers) {
            const std::string lower_name = asciiLower(h.name);
            if (lower_name == "accept") {
                has_accept = true;
            } else if (lower_name == "user-agent") {
                has_ua = true;
            } else if (lower_name == "api-key") {
                has_api_key = true;
            } else if (lower_name == "authorization" &&
                       startsWithIgnoreCase(h.value, "Bearer ")) {
                bearer = utf8::trim(std::string_view(h.value).substr(7));
            }
        }
        const bool is_sse =
            (req.method == "POST" &&
             req.body.find("\"stream\":true") != std::string::npos);
        if (!has_accept) {
            req.headers.push_back(
                {"Accept", is_sse ? "text/event-stream, application/json"
                                  : "application/json"});
        }
        if (!has_ua) {
            req.headers.push_back({"User-Agent", "ShadowBar/0.1"});
        }
        if (!bearer.empty() && !has_api_key && isAzureOpenAiEndpoint(req.url)) {
            req.headers.push_back({"api-key", bearer});
        }
    };

    auto inspect_json_line = [&](std::string_view payload,
                                 std::string &normalized_out) -> bool {
        const std::string_view trimmed = utf8::trimView(payload);
        if (trimmed.empty() || trimmed == "[DONE]" || trimmed.front() != '{') {
            return false;
        }
        try {
            auto j = nlohmann::json::parse(trimmed);
            if (intercept.inline_error.empty()) {
                std::string err = extractJsonErrorField(j);
                if (!err.empty()) {
                    intercept.inline_error = std::move(err);
                }
            }
            if (!intercept.saw_content_delta &&
                !extractLlmContentFromJson(j).empty()) {
                intercept.saw_content_delta = true;
            }
            // Flatten OpenAI content-part arrays delta.content=[{type:"text",text:"..."}]
            // so downstream SSE parsers handle them identically to string deltas.
            if (j.is_object() && j.contains("choices") &&
                j["choices"].is_array() && !j["choices"].empty() &&
                j["choices"][0].is_object()) {
                auto &c0 = j["choices"][0];
                if (c0.contains("delta") && c0["delta"].is_object() &&
                    c0["delta"].contains("content") &&
                    c0["delta"]["content"].is_array()) {
                    c0["delta"]["content"] =
                        extractTextFromContentField(c0["delta"]["content"]);
                    normalized_out = j.dump();
                    return true;
                }
            }
        } catch (...) {
        }
        return false;
    };

    HttpTransport retried_transport =
        [&](const HttpRequest &raw_req, const std::atomic<bool> &cancel,
            HttpStreamChunkCallback inner_on_chunk) -> HttpResponse {
        HttpRequest req = raw_req;
        enrich_headers(req);
        intercept.last_request = req;

        constexpr int kMaxAttempts = 2;
        HttpResponse final_resp;
        for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
            if (cancel.load(std::memory_order_relaxed)) {
                final_resp.cancelled = true;
                final_resp.error_message = "cancelled";
                break;
            }
            const auto rem_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    overall_deadline - SteadyClock::now())
                    .count();
            if (attempt > 0 && rem_ms < 500) {
                break;
            }
            HttpRequest attempt_req = req;
            if (rem_ms > 0) {
                attempt_req.timeout_ms =
                    std::min(req.timeout_ms, static_cast<int>(rem_ms));
            }

            intercept.captured_raw.clear();
            intercept.inline_error.clear();
            intercept.saw_sse_data = false;
            intercept.saw_content_delta = false;
            std::string line_buf;

            HttpStreamChunkCallback spy_chunk;
            if (inner_on_chunk) {
                spy_chunk = [&](std::string_view chunk) {
                    if (intercept.captured_raw.size() < 65536) {
                        intercept.captured_raw.append(chunk);
                    }
                    line_buf.append(chunk);
                    size_t nl = std::string::npos;
                    while ((nl = line_buf.find('\n')) != std::string::npos) {
                        std::string line = line_buf.substr(0, nl);
                        line_buf.erase(0, nl + 1);
                        if (!line.empty() && line.back() == '\r') {
                            line.pop_back();
                        }
                        if (line.rfind("data:", 0) == 0) {
                            intercept.saw_sse_data = true;
                            std::string normalized_json;
                            if (inspect_json_line(
                                    std::string_view(line).substr(5),
                                    normalized_json)) {
                                std::string rewritten =
                                    "data: " + normalized_json + "\n";
                                inner_on_chunk(rewritten);
                                continue;
                            }
                        } else {
                            std::string unused;
                            inspect_json_line(line, unused);
                        }
                        inner_on_chunk(line + "\n");
                    }
                    if (!line_buf.empty() && !intercept.saw_sse_data &&
                        line_buf.find("data:") == std::string::npos) {
                        std::string unused;
                        inspect_json_line(line_buf, unused);
                    }
                };
            }

            final_resp = transport(attempt_req, cancel, spy_chunk);

            const bool can_retry =
                (attempt + 1 < kMaxAttempts) &&
                !intercept.emitted_partial_tokens &&
                !intercept.saw_content_delta &&
                !cancel.load(std::memory_order_relaxed) &&
                isRetryableHttpOrTransportFailure(final_resp);
            if (can_retry) {
                const int delay_ms = (final_resp.status_code == 429) ? 320 : 200;
                const auto after_sleep_rem =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        overall_deadline - SteadyClock::now())
                        .count();
                if (after_sleep_rem >= delay_ms + 450) {
                    int slept = 0;
                    while (slept < delay_ms &&
                           !cancel.load(std::memory_order_relaxed)) {
                        const int step = std::min(25, delay_ms - slept);
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(step));
                        slept += step;
                    }
                    if (!cancel.load(std::memory_order_relaxed)) {
                        // Discard unterminated error/partial line before retry.
                        line_buf.clear();
                        continue;
                    }
                }
            }

            if (!line_buf.empty() && inner_on_chunk) {
                if (line_buf.rfind("data:", 0) == 0) {
                    intercept.saw_sse_data = true;
                    std::string normalized_json;
                    if (inspect_json_line(std::string_view(line_buf).substr(5),
                                          normalized_json)) {
                        inner_on_chunk("data: " + normalized_json + "\n");
                    } else {
                        inner_on_chunk(line_buf);
                    }
                } else {
                    inner_on_chunk(line_buf);
                }
                line_buf.clear();
            }
            break;
        }
        intercept.last_response = final_resp;
        return final_resp;
    };

    auto sanitized_on_partial = [&](std::string_view partial) {
        if (!on_partial || cancel_flag.load(std::memory_order_relaxed)) {
            return;
        }
        std::string cleaned = sanitizeTranslationOutput(partial, true);
        if (!cleaned.empty()) {
            intercept.emitted_partial_tokens = true;
            on_partial(cleaned);
        }
    };

    TranslationResult result;
    switch (effective.provider) {
    case ProviderKind::GoogleFree:
        result = translateGoogleFree(effective, source_lang, text, budget_ms,
                                     cancel_flag, retried_transport);
        break;
    case ProviderKind::OpenAICompatible:
    case ProviderKind::Ollama:
        result = translateOpenAiOrOllama(
            effective, source_lang, text, readonly_context, budget_ms,
            cancel_flag, retried_transport,
            on_partial ? std::function<void(std::string_view)>(sanitized_on_partial)
                       : std::function<void(std::string_view)>{});
        break;
    case ProviderKind::DeepL:
        result = translateDeepL(effective, source_lang, text, budget_ms,
                                cancel_flag, retried_transport);
        break;
    case ProviderKind::Offline:
        return {.ok = false,
                .is_final = true,
                .error_message = "离线词典未命中"};
    }

    const bool is_llm = (effective.provider == ProviderKind::OpenAICompatible ||
                         effective.provider == ProviderKind::Ollama);
    if (!result.ok && is_llm && !cancel_flag.load(std::memory_order_relaxed) &&
        !intercept.last_response.cancelled) {
        // 1. Prefer inline SSE error over generic empty-response messages.
        if (!intercept.inline_error.empty()) {
            result.error_message = extractApiErrorMessage(
                "", intercept.last_response.status_code, "",
                intercept.inline_error);
            return result;
        }

        // 2. If server ignored `stream: true` and returned plain HTTP 200 JSON,
        // extract choices[0].message.content / message.content / response / error.
        const std::string_view raw_json_candidate =
            !utf8::trimView(intercept.last_response.body).empty()
                ? utf8::trimView(intercept.last_response.body)
                : utf8::trimView(intercept.captured_raw);
        if (intercept.last_response.status_code >= 200 &&
            intercept.last_response.status_code < 300 &&
            !raw_json_candidate.empty() && raw_json_candidate.front() == '{') {
            try {
                auto j = nlohmann::json::parse(raw_json_candidate);
                std::string err = extractJsonErrorField(j);
                if (!err.empty()) {
                    result.error_message = err;
                    return result;
                }
                std::string content = extractLlmContentFromJson(j);
                std::string cleaned = sanitizeTranslationOutput(content, false);
                if (!cleaned.empty()) {
                    return {.ok = true,
                            .is_final = true,
                            .translated_text = std::move(cleaned)};
                }
            } catch (...) {
            }
        }

        // 3. Automatic non-stream fallback if provider rejected `stream: true`.
        if (shouldRetryStreamAsNonStream(intercept.last_response.status_code,
                                         result.error_message,
                                         intercept.last_response.body) &&
            !intercept.last_request.body.empty()) {
            try {
                auto payload = nlohmann::json::parse(intercept.last_request.body);
                if (payload.is_object() && payload.value("stream", false)) {
                    payload["stream"] = false;
                    HttpRequest non_stream_req = intercept.last_request;
                    non_stream_req.body = payload.dump();
                    for (auto &h : non_stream_req.headers) {
                        if (asciiLower(h.name) == "accept") {
                            h.value = "application/json";
                        }
                    }
                    const auto rem_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            overall_deadline - SteadyClock::now())
                            .count();
                    non_stream_req.timeout_ms = static_cast<int>(
                        std::max<long long>(500, std::min<long long>(rem_ms, budget_ms)));
                    HttpResponse fb_resp = transport(non_stream_req, cancel_flag, {});
                    if (fb_resp.cancelled ||
                        cancel_flag.load(std::memory_order_relaxed)) {
                        return {.ok = false,
                                .is_final = true,
                                .error_message = "cancelled"};
                    }
                    if (fb_resp.status_code < 200 || fb_resp.status_code >= 300) {
                        return {.ok = false,
                                .is_final = true,
                                .error_message = extractApiErrorMessage(
                                    "", fb_resp.status_code, fb_resp.body,
                                    fb_resp.error_message)};
                    }
                    auto fb_json = nlohmann::json::parse(fb_resp.body);
                    std::string err = extractJsonErrorField(fb_json);
                    if (!err.empty()) {
                        return {.ok = false,
                                .is_final = true,
                                .error_message = std::move(err)};
                    }
                    std::string cleaned = sanitizeTranslationOutput(
                        extractLlmContentFromJson(fb_json), false);
                    if (!cleaned.empty()) {
                        return {.ok = true,
                                .is_final = true,
                                .translated_text = std::move(cleaned)};
                    }
                }
            } catch (...) {
            }
        }
    }

    if (result.ok) {
        result.translated_text =
            sanitizeTranslationOutput(result.translated_text, false);
        if (result.translated_text.empty()) {
            return {.ok = false,
                    .is_final = true,
                    .error_message = "API 返回内容为空"};
        }
    }
    return result;
}

AsyncTranslationEngine::AsyncTranslationEngine(HttpTransport transport,
                                               size_t cache_capacity)
    : transport_(std::move(transport)), cache_(cache_capacity) {
    worker_ = std::thread([this]() { workerLoop(); });
}

AsyncTranslationEngine::~AsyncTranslationEngine() {
    shutdown();
}

void AsyncTranslationEngine::setCacheCapacity(size_t capacity) {
    cache_.setCapacity(capacity);
}

void AsyncTranslationEngine::clearCache() {
    cache_.clear();
}

void AsyncTranslationEngine::reloadOfflineDictionary(std::string_view custom_path) {
    offline_dict_.reload(custom_path);
}

void AsyncTranslationEngine::reloadOfflineDictionaries(
    const std::vector<ExternalDictSpec> &specs) {
    offline_dict_.reload(specs);
}

OfflineDictStats AsyncTranslationEngine::offlineDictStats() const {
    return offline_dict_.stats();
}

void AsyncTranslationEngine::cancelAll() {
    std::lock_guard<std::mutex> lock(mu_);
    ++latest_revision_;
    has_pending_ = false;
    if (current_cancel_) {
        current_cancel_->store(true, std::memory_order_relaxed);
    }
    cv_.notify_all();
}

void AsyncTranslationEngine::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_) {
            return;
        }
        stop_ = true;
        has_pending_ = false;
        ++latest_revision_;
        if (current_cancel_) {
            current_cancel_->store(true, std::memory_order_relaxed);
        }
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void AsyncTranslationEngine::submit(TranslationJob job, UpdateCallback on_update) {
    // Synchronous fast path: single phrase hits cache or offline dictionary without scheduling network.
    const std::string trimmed = utf8::trim(job.source_text);
    if (trimmed.empty()) {
        cancelAll();
        return;
    }

    const std::string cache_scope = makeCacheScope(job.preset, job.source_lang);
    if (!job.bypass_cache) {
        if (auto cached =
                cache_.get(cache_scope, job.preset.target_lang, trimmed)) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                latest_revision_ = job.revision;
                has_pending_ = false;
                if (current_cancel_) {
                    current_cancel_->store(true, std::memory_order_relaxed);
                }
            }
            cv_.notify_all();
            if (on_update) {
                on_update(job.revision,
                          {.ok = true, .is_final = true, .translated_text = *cached});
            }
            return;
        }
    }

    if (job.preset.provider == ProviderKind::Offline) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            latest_revision_ = job.revision;
            has_pending_ = false;
            if (current_cancel_) {
                current_cancel_->store(true, std::memory_order_relaxed);
            }
        }
        cv_.notify_all();
        if (on_update) {
            on_update(job.revision,
                      runOfflineLookup(offline_dict_, trimmed,
                                       job.source_lang,
                                       job.preset.target_lang));
        }
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        latest_revision_ = job.revision;
        if (current_cancel_) {
            current_cancel_->store(true, std::memory_order_relaxed);
        }
        auto delay = job.immediate ? std::chrono::milliseconds(0)
                                   : std::chrono::milliseconds(job.debounce_ms);
        pending_ = PendingJob{
            .job = std::move(job),
            .callback = std::move(on_update),
            .execute_at = SteadyClock::now() + delay,
        };
        has_pending_ = true;
    }
    cv_.notify_all();
}

void AsyncTranslationEngine::workerLoop() {
    std::unique_lock<std::mutex> lock(mu_);
    while (!stop_) {
        if (!has_pending_) {
            cv_.wait(lock, [this]() { return stop_ || has_pending_; });
            continue;
        }

        const auto deadline = pending_.execute_at;
        const uint64_t scheduled_rev = pending_.job.revision;
        if (SteadyClock::now() < deadline) {
            cv_.wait_until(lock, deadline, [this, scheduled_rev]() {
                return stop_ || !has_pending_ || latest_revision_ != scheduled_rev;
            });
            continue;
        }

        PendingJob active = std::move(pending_);
        has_pending_ = false;
        auto cancel_flag = std::make_shared<std::atomic<bool>>(false);
        current_cancel_ = cancel_flag;
        lock.unlock();

        TranslationResult res = executeJob(active.job, active.callback, cancel_flag);

        lock.lock();
        if (current_cancel_ == cancel_flag) {
            current_cancel_.reset();
        }
        const bool still_latest = (!stop_ && active.job.revision == latest_revision_);
        lock.unlock();

        if (still_latest && !cancel_flag->load(std::memory_order_relaxed) &&
            active.callback) {
            active.callback(active.job.revision, res);
        }

        lock.lock();
    }
}

TranslationResult AsyncTranslationEngine::executeJob(
    const TranslationJob &job,
    const UpdateCallback &callback,
    const std::shared_ptr<std::atomic<bool>> &cancel_flag) {
    auto join_parts = [](const std::vector<std::string> &parts,
                         std::string_view tail) -> std::string {
        std::string out;
        for (const auto &p : parts) {
            if (!p.empty()) {
                if (!out.empty()) {
                    out.push_back(' ');
                }
                out.append(p);
            }
        }
        std::string_view t = utf8::trimView(tail);
        if (!t.empty()) {
            if (!out.empty()) {
                out.push_back(' ');
            }
            out.append(t);
        }
        return out;
    };

    auto lookup_fallback = [&](std::string_view text) -> std::optional<std::string> {
        auto hit = offline_dict_.lookupDetailed(text, job.preset.target_lang,
                                                job.source_lang);
        if (!hit || hit->matched_units <= 0) {
            return std::nullopt;
        }
        if (hit->exact_match || hit->coverage() >= 0.65) {
            return std::move(hit->text);
        }
        return std::nullopt;
    };

    const std::string cache_scope = makeCacheScope(job.preset, job.source_lang);

    if (!job.lock_on_punctuation) {
        auto on_partial = [&](std::string_view partial) {
            if (cancel_flag->load(std::memory_order_relaxed) || !callback) {
                return;
            }
            callback(job.revision, {.ok = true,
                                    .is_final = false,
                                    .translated_text = std::string(partial)});
        };
        TranslationResult res = translateSingleUnit(
            job.preset, job.source_lang, job.source_text, "", job.timeout_ms,
            *cancel_flag, transport_, on_partial);
        if (res.ok && res.is_final && !res.translated_text.empty()) {
            cache_.put(cache_scope, job.preset.target_lang,
                       utf8::trim(job.source_text), res.translated_text);
        } else if (!res.ok && !cancel_flag->load(std::memory_order_relaxed) &&
                   job.offline_fallback) {
            if (auto hit = lookup_fallback(job.source_text)) {
                return {.ok = true,
                        .is_final = true,
                        .from_offline_dict = true,
                        .translated_text = std::move(*hit)};
            }
        }
        return res;
    }

    ClauseSplitResult split = splitClauses(job.source_text);
    std::vector<std::string> locked_translations;
    locked_translations.reserve(split.locked_clauses.size());

    std::string last_locked_source;
    for (const auto &clause : split.locked_clauses) {
        if (cancel_flag->load(std::memory_order_relaxed)) {
            return {.ok = false, .is_final = true, .error_message = "cancelled"};
        }
        auto cached =
            job.bypass_cache
                ? std::nullopt
                : cache_.get(cache_scope, job.preset.target_lang, clause);
        if (cached) {
            locked_translations.push_back(*cached);
        } else {
            TranslationResult c_res = translateSingleUnit(
                job.preset, job.source_lang, clause, last_locked_source,
                job.timeout_ms, *cancel_flag, transport_, {});
            if (!c_res.ok) {
                if (!cancel_flag->load(std::memory_order_relaxed) &&
                    job.offline_fallback) {
                    if (auto hit = lookup_fallback(clause)) {
                        locked_translations.push_back(std::move(*hit));
                        last_locked_source = clause;
                        continue;
                    }
                }
                return c_res;
            }
            cache_.put(cache_scope, job.preset.target_lang, clause,
                       c_res.translated_text);
            locked_translations.push_back(std::move(c_res.translated_text));
        }
        last_locked_source = clause;
    }

    std::string tail_trimmed = utf8::trim(split.active_tail);
    if (tail_trimmed.empty()) {
        std::string combined = join_parts(locked_translations, "");
        return {.ok = true, .is_final = true, .translated_text = std::move(combined)};
    }

    // Show locked prefix immediately if we already have previous clauses while tail streams.
    if (!locked_translations.empty() && callback &&
        !cancel_flag->load(std::memory_order_relaxed)) {
        callback(job.revision,
                 {.ok = true,
                  .is_final = false,
                  .translated_text = join_parts(locked_translations, "")});
    }

    if (!job.bypass_cache) {
        if (auto tail_cached =
                cache_.get(cache_scope, job.preset.target_lang, tail_trimmed)) {
            return {.ok = true,
                    .is_final = true,
                    .translated_text = join_parts(locked_translations, *tail_cached)};
        }
    }

    auto on_tail_partial = [&](std::string_view partial) {
        if (cancel_flag->load(std::memory_order_relaxed) || !callback) {
            return;
        }
        callback(job.revision,
                 {.ok = true,
                  .is_final = false,
                  .translated_text = join_parts(locked_translations, partial)});
    };

    TranslationResult tail_res = translateSingleUnit(
        job.preset, job.source_lang, tail_trimmed, last_locked_source,
        job.timeout_ms, *cancel_flag, transport_, on_tail_partial);
    if (tail_res.ok) {
        cache_.put(cache_scope, job.preset.target_lang, tail_trimmed,
                   tail_res.translated_text);
        return {.ok = true,
                .is_final = true,
                .translated_text =
                    join_parts(locked_translations, tail_res.translated_text)};
    }

    if (!cancel_flag->load(std::memory_order_relaxed) && job.offline_fallback) {
        if (auto hit = lookup_fallback(tail_trimmed)) {
            return {.ok = true,
                    .is_final = true,
                    .from_offline_dict = true,
                    .translated_text = join_parts(locked_translations, std::move(*hit))};
        }
    }

    // Preserve already translated locked prefix in partial display on tail error.
    if (!locked_translations.empty()) {
        tail_res.translated_text = join_parts(locked_translations, "");
    }
    return tail_res;
}

} // namespace shadowbar
