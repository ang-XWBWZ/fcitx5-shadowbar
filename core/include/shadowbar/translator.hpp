#pragma once

#include "shadowbar/cache.hpp"
#include "shadowbar/types.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace shadowbar {

struct HttpHeader {
    std::string name;
    std::string value;
};

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<HttpHeader> headers;
    std::string body;
    int timeout_ms = 4500;
};

struct HttpResponse {
    long status_code = 0;
    std::string body;
    std::string error_message;
    bool cancelled = false;
};

using HttpStreamChunkCallback = std::function<void(std::string_view chunk)>;
using HttpTransport = std::function<HttpResponse(const HttpRequest &req,
                                                 const std::atomic<bool> &cancel_flag,
                                                 HttpStreamChunkCallback on_chunk)>;

/// Production HTTP transport backed by libcurl with immediate cancellation via xferinfo.
HttpResponse curlHttpTransport(const HttpRequest &req,
                               const std::atomic<bool> &cancel_flag,
                               HttpStreamChunkCallback on_chunk);

/// Request payload dispatched to AsyncTranslationEngine.
struct TranslationJob {
    uint64_t revision = 0;
    std::string source_text;
    std::string source_lang = "zh";
    Preset preset;
    bool immediate = false;
    bool lock_on_punctuation = true;
    bool offline_fallback = true;
    bool bypass_cache = false;
    int debounce_ms = 260;
    int timeout_ms = 4500;
};

/// Translates a single text unit (sync) using the specified preset and optional read-only context.
TranslationResult translateSingleUnit(
    const Preset &preset,
    std::string_view source_lang,
    std::string_view text,
    std::string_view readonly_context,
    int timeout_ms,
    const std::atomic<bool> &cancel_flag,
    const HttpTransport &transport,
    const std::function<void(std::string_view partial_text)> &on_partial);

/// Strips `<think>...</think>` blocks emitted by reasoning models.
std::string stripReasoningTags(std::string_view raw);

/// Background translation worker with debounce, clause locking, LRU cache, and cancel-in-flight.
class AsyncTranslationEngine {
public:
    using UpdateCallback = std::function<void(uint64_t revision, TranslationResult result)>;

    explicit AsyncTranslationEngine(HttpTransport transport = curlHttpTransport,
                                    size_t cache_capacity = 2000,
                                    int worker_idle_timeout_ms = 45000);
    ~AsyncTranslationEngine();

    AsyncTranslationEngine(const AsyncTranslationEngine &) = delete;
    AsyncTranslationEngine &operator=(const AsyncTranslationEngine &) = delete;

    void setCacheCapacity(size_t capacity);
    void clearCache();
    void reloadOfflineDictionary(std::string_view custom_path = "");
    void reloadOfflineDictionaries(const std::vector<ExternalDictSpec> &specs);
    OfflineDictStats offlineDictStats() const;

    /// Submits a translation job. Calls `on_update` synchronously if resolved immediately from cache/dict,
    /// or asynchronously from the lazy worker thread after debounce.
    void submit(TranslationJob job, UpdateCallback on_update);

    /// Cancels any pending or in-flight request without shutting down the engine.
    void cancelAll();

    /// Stops worker thread cleanly (called automatically by destructor).
    void shutdown();

    /// Returns true if the background translation worker thread is currently active.
    bool isWorkerRunning() const;

private:
    struct PendingJob {
        TranslationJob job;
        UpdateCallback callback;
        SteadyTimePoint execute_at;
    };

    void workerLoop();
    TranslationResult executeJob(const TranslationJob &job,
                                 const UpdateCallback &callback,
                                 const std::shared_ptr<std::atomic<bool>> &cancel_flag);

    HttpTransport transport_;
    LruTranslationCache cache_;
    OfflineDictionary offline_dict_;
    int worker_idle_timeout_ms_ = 45000;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool has_pending_ = false;
    bool worker_running_ = false;
    PendingJob pending_;
    uint64_t latest_revision_ = 0;
    std::shared_ptr<std::atomic<bool>> current_cancel_;
    std::thread worker_;
};

/// Returns the per-user runtime path used to coordinate local HY-MT1.5 activity across processes.
std::string hyMtActivityStampPath();

/// Returns true when `model_name` identifies Tencent Hunyuan HY-MT / TY models.
bool isHyMtModelName(std::string_view model_name);

/// Builds the official single-turn user prompt expected by Hunyuan HY-MT1.5 models.
std::string buildHyMtUserPrompt(std::string_view source_lang,
                                std::string_view target_lang,
                                std::string_view text,
                                std::string_view readonly_context = {});

} // namespace shadowbar
