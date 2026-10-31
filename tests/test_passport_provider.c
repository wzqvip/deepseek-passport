#include "passport_provider.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned u32(const uint8_t *p) { return p[0]|(unsigned)p[1]<<8|(unsigned)p[2]<<16|(unsigned)p[3]<<24; }
int main(void)
{
    char out[1024]; uint8_t wav[44];
    assert(passport_provider_endpoint("https://example.test/v1/","/audio/speech",out,sizeof(out)));
    assert(!strcmp(out,"https://example.test/v1/audio/speech"));
    assert(passport_provider_endpoint("https://example.test/v1/audio/speech/","/audio/speech",out,sizeof(out)));
    assert(!strcmp(out,"https://example.test/v1/audio/speech"));
    assert(!passport_provider_endpoint("http://example.test","/audio/speech",out,sizeof(out)));
    assert(!passport_provider_endpoint("https://example.test","/audio/speech",out,8));
    assert(passport_provider_stt_prefix("FunAudioLLM/SenseVoiceSmall",out,sizeof(out)));
    assert(strstr(out,"name=\"model\"\r\n\r\nFunAudioLLM/SenseVoiceSmall\r\n"));
    assert(strstr(out,"filename=\"speech.wav\""));
    assert(!passport_provider_stt_prefix("bad\r\nmodel",out,sizeof(out)));
    assert(!passport_provider_stt_prefix("model",out,10));
    assert(!strcmp(passport_provider_stt_suffix(),"\r\n--" PASSPORT_PROVIDER_BOUNDARY "--\r\n"));
    passport_provider_wav(wav,96000);
    assert(!memcmp(wav,"RIFF",4) && !memcmp(wav+8,"WAVEfmt ",8));
    assert(u32(wav+4)==96036 && u32(wav+40)==96000 && u32(wav+24)==16000);
    assert(u32(wav+28)==32000 && wav[22]==1 && wav[34]==16);
    assert(passport_provider_tts_body("FunAudioLLM/CosyVoice2-0.5B","FunAudioLLM/CosyVoice2-0.5B:anna","hi \"you\"",out,sizeof(out)));
    assert(strstr(out,"\"response_format\":\"pcm\",\"sample_rate\":16000,\"stream\":true"));
    assert(strstr(out,"hi \\\"you\\\""));
    assert(passport_provider_tts_body("m","v","line\ntext",out,sizeof(out)));
    assert(strstr(out,"line\\ntext"));
    assert(!passport_provider_tts_body("m","v","bad\x01text",out,sizeof(out)));
    assert(!passport_provider_tts_body("m","","text",out,sizeof(out)));
    assert(!passport_provider_tts_body("m","v","text",out,10));
    assert(passport_provider_record_capacity(300000,200000)==96000);
    assert(passport_provider_record_capacity(80000,200000)==0);
    assert(passport_provider_record_capacity(90000,200000)==0);
    assert(passport_provider_record_capacity(120000,20000)==19840);
    char chat[2048];
    assert(passport_provider_chat_body("m\"x", "hello\nworld", "old", "answer", chat, sizeof(chat)));
    assert(strstr(chat,"gentle, friendly blue whale"));
    assert(strstr(chat,"\"role\":\"assistant\""));
    assert(strstr(chat,"m\\\"x"));
    assert(!passport_provider_chat_body("m", "hello", "", "", chat, 20));
    assert(chat[0] == 0);
    puts("provider protocol tests: PASS");
    return 0;
}
