#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PASSPORT_PROVIDER_BOUNDARY "passport-audio-7e32"
#define PASSPORT_PROVIDER_PCM_MAX 96000u
#define PASSPORT_PROVIDER_HEAP_RESERVE 80000u
bool passport_provider_endpoint(const char *base, const char *suffix, char *out, size_t size);
bool passport_provider_stt_prefix(const char *model, char *out, size_t size);
const char *passport_provider_stt_suffix(void);
void passport_provider_wav(uint8_t out[44], size_t pcm_bytes);
bool passport_provider_tts_body(const char *model, const char *voice, const char *text,
                                char *out, size_t size);
size_t passport_provider_record_capacity(size_t free_heap, size_t largest);
// MIME allowlist only; does not prove actual PCM encoding or sample rate.
bool passport_provider_pcm_content_type(const char *content_type);
bool passport_provider_chat_body(const char *model, const char *prompt,
    const char *previous_prompt, const char *previous_reply, char *out, size_t size);
