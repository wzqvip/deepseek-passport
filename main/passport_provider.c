// Wire format from SiliconFlow official audio/transcriptions and audio/speech API.
// Pure helpers: no network access, credentials, or ESP-IDF dependency.
#include "passport_provider.h"
#include <stdio.h>
#include <string.h>

bool passport_provider_pcm_content_type(const char *type)
{
    if (!type) return false;
    while (*type == ' ' || *type == '\t') ++type;
    char mime[48]; size_t n = 0;
    while (*type && *type != ';') {
        if (n + 1 >= sizeof(mime)) return false;
        unsigned char c = (unsigned char)*type++;
        mime[n++] = (char)((c >= 'A' && c <= 'Z') ? c + ('a'-'A') : c);
    }
    while (n && (mime[n-1] == ' ' || mime[n-1] == '\t')) --n;
    mime[n] = 0;
    return !strcmp(mime, "audio/pcm") || !strcmp(mime, "audio/l16") ||
           !strcmp(mime, "audio/x-pcm") || !strcmp(mime, "application/octet-stream");
}

bool passport_provider_endpoint(const char *base, const char *suffix, char *out, size_t size)
{
    if (!base || !suffix || !out || !size || strncmp(base, "https://", 8) != 0) return false;
    size_t n = strlen(base), m = strlen(suffix);
    while (n && base[n-1] == '/') --n;
    int written = (n >= m && memcmp(base+n-m, suffix, m) == 0)
        ? snprintf(out, size, "%.*s", (int)n, base)
        : snprintf(out, size, "%.*s%s", (int)n, base, suffix);
    return written > 0 && (size_t)written < size;
}

bool passport_provider_stt_prefix(const char *model, char *out, size_t size)
{
    if (!model || !*model || strchr(model, '\r') || strchr(model, '\n')) return false;
    int n = snprintf(out, size, "--" PASSPORT_PROVIDER_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n%s\r\n"
        "--" PASSPORT_PROVIDER_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n", model);
    return n > 0 && (size_t)n < size;
}
const char *passport_provider_stt_suffix(void) { return "\r\n--" PASSPORT_PROVIDER_BOUNDARY "--\r\n"; }
static void le32(uint8_t *p, uint32_t v) { for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i)); }
void passport_provider_wav(uint8_t out[44], size_t bytes)
{
    memset(out, 0, 44);
    memcpy(out, "RIFF", 4); le32(out+4, (uint32_t)bytes+36);
    memcpy(out+8, "WAVEfmt ", 8); le32(out+16, 16);
    out[20]=1; out[22]=1; le32(out+24, 16000); le32(out+28, 32000);
    out[32]=2; out[34]=16; memcpy(out+36, "data", 4); le32(out+40, (uint32_t)bytes);
}
static bool escape(const char *in, char *out, size_t size)
{
    size_t n=0;
    if (!in) return false;
    for (; *in; ++in) {
        unsigned char c=(unsigned char)*in;
        if (c == '\n' || c == '\r' || c == '\t') {
            if (n+2 >= size) return false;
            out[n++]='\\'; out[n++]=c=='\n' ? 'n' : (c=='\r' ? 'r' : 't');
            continue;
        }
        if (c < 32) return false;
        if (c=='"' || c=='\\') { if(n+1>=size) return false; out[n++]='\\'; }
        if(n+1>=size) return false;
        out[n++]=(char)c;
    }
    out[n]=0; return true;
}
bool passport_provider_tts_body(const char *model, const char *voice, const char *text, char *out, size_t size)
{
    char m[192], v[192], t[384];
    if (!model || !*model || !voice || !*voice || !text || !*text ||
        !escape(model,m,sizeof(m)) || !escape(voice,v,sizeof(v)) || !escape(text,t,sizeof(t))) return false;
    int n=snprintf(out,size,"{\"model\":\"%s\",\"voice\":\"%s\",\"input\":\"%s\","
        "\"response_format\":\"pcm\",\"sample_rate\":16000,\"stream\":true}",m,v,t);
    return n>0 && (size_t)n<size;
}
bool passport_provider_chat_body(const char *model, const char *prompt,
    const char *previous_prompt, const char *previous_reply, char *out, size_t size)
{
    char m[192], p[400], hp[400], hr[512];
    if (!model || !*model || !prompt || !*prompt || !out || !size ||
        !escape(model,m,sizeof(m)) || !escape(prompt,p,sizeof(p)) ||
        !escape(previous_prompt,hp,sizeof(hp)) || !escape(previous_reply,hr,sizeof(hr))) return false;
    const char *persona = "You are a gentle, friendly blue whale companion. "
        "Reply warmly and briefly in the user's language. Be honest about uncertainty. "
        "Never pretend to have performed real-world actions.";
    int n;
    if (hp[0] && hr[0]) {
        n=snprintf(out,size,"{\"model\":\"%s\",\"messages\":["
            "{\"role\":\"system\",\"content\":\"%s\"},"
            "{\"role\":\"user\",\"content\":\"%s\"},"
            "{\"role\":\"assistant\",\"content\":\"%s\"},"
            "{\"role\":\"user\",\"content\":\"%s\"}],\"max_tokens\":256,\"stream\":false}",m,persona,hp,hr,p);
    } else {
        n=snprintf(out,size,"{\"model\":\"%s\",\"messages\":["
            "{\"role\":\"system\",\"content\":\"%s\"},"
            "{\"role\":\"user\",\"content\":\"%s\"}],\"max_tokens\":256,\"stream\":false}",m,persona,p);
    }
    if (n<=0 || (size_t)n>=size) { out[0]=0; return false; }
    return true;
}

size_t passport_provider_record_capacity(size_t free_heap, size_t largest)
{
    if (free_heap <= PASSPORT_PROVIDER_HEAP_RESERVE) return 0;
    size_t n=free_heap-PASSPORT_PROVIDER_HEAP_RESERVE;
    if (n>largest) n=largest;
    if (n>PASSPORT_PROVIDER_PCM_MAX) n=PASSPORT_PROVIDER_PCM_MAX;
    n=(n/640)*640;
    return n>=16000 ? n : 0; // Refuse less than half a second, rather than fail mid-turn.
}
