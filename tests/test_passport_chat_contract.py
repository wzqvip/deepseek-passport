"""Regression contracts for admission/completion when ESP-IDF is unavailable.

Runtime scheduling and TLS/audio still require firmware/device validation.
"""
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

class ChatContract(unittest.TestCase):
    def test_admit_before_publish(self):
        text = (ROOT / 'main/passport_chat.c').read_text(encoding='utf-8')
        submit = text[text.index('static esp_err_t submit('):]
        self.assertLess(submit.index('s_busy = true'), submit.index('xQueueSend'))
        self.assertIn('s_busy || s_reserved_id != 0', submit)
        self.assertIn('taskENTER_CRITICAL(&s_lock)', submit)

    def test_failed_result_cannot_speak_stale_reply(self):
        text = (ROOT / 'main/passport_speech.c').read_text(encoding='utf-8')
        turn = text[text.index('static void run_turn(void)'):]
        self.assertIn('passport_chat_submit(s_last_transcript, &request_id)', turn)
        self.assertIn('passport_chat_result(request_id, reply, sizeof(reply))', turn)
        self.assertIn('passport_chat_abandon(request_id)', turn)
        self.assertNotIn('passport_chat_last_reply', turn)
        self.assertNotIn('passport_chat_busy()', turn)
        failure = turn.index('if (s_cancelled || completion != ESP_OK)')
        self.assertLess(turn.index('return;', failure), turn.index('speak_text(reply)'))

    def test_notification_speech_uses_existing_worker(self):
        text = (ROOT / 'main/passport_speech.c').read_text(encoding='utf-8')
        public = text[text.index('esp_err_t passport_speech_speak('):text.index('// Drive one whole turn.')]
        self.assertIn('return passport_speech_request_speak(text)', public)
        self.assertNotIn('speak_text_impl(', public)
        self.assertIn('s_turn_active || s_started || s_speak_pending', public)
        worker = text[text.index('static void speech_task('):]
        self.assertIn('speak_text(spoken)', worker)
        self.assertIn('s_turn_active = do_speak || do_turn', worker)

    def test_cancel_suppresses_followup_stages(self):
        text = (ROOT / 'main/passport_speech.c').read_text(encoding='utf-8')
        cancel = text[text.index('void passport_speech_talk_cancel('):text.index('bool passport_speech_listening(')]
        self.assertIn('s_cancelled = true', cancel)
        self.assertIn('s_held = false', cancel)
        self.assertIn('s_speak_pending = false', cancel)
        self.assertIn('PASSPORT_VOICE_ACT_ASK_MODEL && !s_cancelled', text)
        self.assertIn('while (!s_cancelled && completion', text)
        self.assertIn('if (s_cancelled) { err=ESP_ERR_INVALID_STATE; break; }', text)

    def test_stt_bound_and_tts_owner(self):
        text = (ROOT / 'main/passport_speech.c').read_text(encoding='utf-8')
        self.assertIn('passport_provider_record_capacity(', text)
        self.assertIn('free(pcm); // Release recording', text)
        self.assertIn('REC max %u.%u s', text)
        self.assertIn('if (s_speaking)', text)
        self.assertIn('.crt_bundle_attach = esp_crt_bundle_attach', text)

if __name__ == '__main__':
    unittest.main()
