#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Opaque handle for cross-language / Windows TSF / macOS IMKit integration.
typedef struct ShadowBarHandle ShadowBarHandle;

typedef struct ShadowBarViewSnapshot {
    int mode;               // 0 = Off, 1 = OneShot, 2 = Sticky
    int bar_visible;        // 0 or 1
    int status;             // Maps to shadowbar::TranslationStatus
    uint64_t revision;
    const char *badge;
    const char *source_text;
    size_t cursor_byte;
    const char *translation_text;
    const char *status_hint;
} ShadowBarViewSnapshot;

typedef struct ShadowBarActionSnapshot {
    int consume_key;
    int update_ui;
    const char *commit_to_app;
    const char *replay_raw_prefix;
    const char *toast_message;
} ShadowBarActionSnapshot;

typedef void (*ShadowBarUiUpdateFn)(void *user_data, const ShadowBarViewSnapshot *view);

ShadowBarHandle *shadowbar_create(const char *config_path);
void shadowbar_destroy(ShadowBarHandle *handle);

void shadowbar_set_ui_callback(ShadowBarHandle *handle,
                               ShadowBarUiUpdateFn cb,
                               void *user_data);

ShadowBarActionSnapshot shadowbar_toggle_sticky(ShadowBarHandle *handle);
ShadowBarActionSnapshot shadowbar_on_key(ShadowBarHandle *handle,
                                         int special_key,
                                         const char *utf8_char,
                                         int modifiers,
                                         int ime_composing,
                                         int in_cjk_im);
ShadowBarActionSnapshot shadowbar_on_ime_commit(ShadowBarHandle *handle,
                                                const char *utf8_text);
ShadowBarViewSnapshot shadowbar_get_view(ShadowBarHandle *handle);

#ifdef __cplusplus
}
#endif
