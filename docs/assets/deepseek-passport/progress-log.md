<p align="right">
  <a href="progress-log.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# DeepSeek Passport Progress Log

A dated record of what was done, what was decided, what evidence supports it, and
what is still open. New entries are appended at the top of the history section.

Rules for this log:

- Record decisions, not activity. A commit message already records that code
  changed.
- Every factual claim is either cited to a file or a reference, or marked as
  reported and unverified.
- Never record a credential, a key, a token, or a QR secret.
- A milestone is only complete when the corresponding exit criteria in
  [roadmap.md](roadmap.md) are met and their evidence is recorded here.

---

## 2026-10-15 — Session 42: "holding OK does nothing" had three causes, and only one was the button

The owner reported that holding OK had stopped responding. It had three causes at once, and the button was
the only innocent one.

### It was on the work screen

```text
screen work  240x320  children=13
```

On the work screen OK answers the selected option; it does not start a conversation. The device had been
put there by the bridge's status document — `{"task": "waiting for a task", "state": "idle"}` — which the
work feed treated as a notification worth taking the screen over for. So the owner held the talk button on
a screen that has no talk button, and the device was working exactly as written.

Two fixes, one at each end, because either alone leaves the trap:

- **A document that says nothing is happening is not news.** The screen is taken over only for a state
  that is running, waiting, done or failed — never for idle — so an idle report cannot move the interface
  away from where talking works.
- **The bridge no longer invents a status.** Its `/work-status` answers 404 when there is nothing to
  report, instead of a cheerful idle document whose only effect was to take over a pet's screen.

The work screen already said how to leave (`UP/DOWN 选择  OK 确认  长按OK 返回`), which is what a person
who lands there needs — and the owner, reasonably, had not read it as an instruction about the button they
were holding.

### The device had also lost its network, and refused turns silently

```text
wifi: Association refused temporarily time 1000, comeback time 1100 (TUs)
voice: last="not associated"
```

The turn needs the network; it refused and said **nothing on screen**, so the console's record was the only
place the reason existed. The refusal now says so in words the owner can act on, and that is the general
rule this session added: every path that declines to do something says so where it can be seen.

Recovering it taught a lesson of its own. The association was refused temporarily — a router rate limit —
and repeated `wifi connect` attempts kept it refused and then left the station in a failed state. A reboot
re-associated immediately. Hammering a link that has just refused you is not a diagnostic.

### And the wake swallow, which was a genuine defect

A press on a dark screen was consumed as "wake up" and did nothing else, so holding OK on a sleeping device
required pressing it twice. That is a defensible rule for a notification button and a wrong one for a
push-to-talk button: the talk press now lights the screen **and** starts listening, and the **release** is
honoured too — a release that arrives after the screen has dimmed again used to be swallowed, which left
the microphone open until the thirty-second ceiling and never sent the turn. The silence gate is what makes
the first change safe: a press in a pocket opens the microphone, hears a room, and uploads nothing.

Verified on the device: an idle document leaves the home screen alone, a running one still takes it over, a
long press leaves the work screen, and a press on a dark screen reaches `state=listening mic=open` with the
panel showing the listening line.

---

## 2026-10-15 — Session 41: the device now waits visibly, and a length where the reply should be

The owner's report was precise and correct: **"the bridge was still transcribing after six attempts"** —
the device's own give-up message — with the instruction to add a thinking state and wait for the result.

### Three timeouts that did not agree

The wait was six attempts with a 400 ms gap: about three seconds. A recognition request is a network round
trip through a provider, so several seconds is normal, and the device was giving up on bridges that were
still working. Making the wait longer, however, exposed the second half of the problem: the **per-request
timeout was eight seconds**, so a bridge that held the request open — as a well-behaved one should, rather
than answering "not ready" and making the device poll — was aborted by the device's own client just before
it answered. Measured on hardware, with the bridge deliberately slowed by eight seconds:

```text
bridge: transcription delayed by 8.0 s for testing
bridge: GET /voice/transcript -> 'which branch should I deploy'      (answered, correctly)
device: last="transcript fetch returned HTTP -1"                     (timed out waiting for it)
```

Three numbers now agree, and they are written down where they can be checked against each other:

| | value | why |
| --- | --- | --- |
| per request | 15 s | the longest a bridge may hold one request |
| between attempts | 0.5 s | the gap after a bridge says "not ready" |
| in total | 30 s | after which the turn fails with the number of seconds it waited |

And the bridge holds its answer for 12 s, deliberately shorter than the device's per-request timeout, so a
provider that is merely slow is answered inside the request the device already has open. Verified with the
same eight-second provider: the turn completes, the transcript arrives, the model answers.

The wait is also **visible** now, which is what the owner asked for: the screen shows a thinking line and
the pet's `THINKING` mood from the moment the audio is delivered until the transcript arrives. A wait
nobody can see is indistinguishable from a hang, and the previous code showed nothing at all — the audio
was delivered, the log said so, and the panel sat on whatever it had said before.

### A length where the reply should be

Testing that fix turned up a separate, pre-existing defect, and it is worth writing down exactly:

```text
panel:  the model's answer, correct, a short Chinese greeting with a romanisation
bridge: GET /speak raw='text=%14'        a single byte, value twenty
```

Twenty is the byte length of that answer. The speaker was handed a **number** where the string should have
been, and it built a request out of it, so the device asked a provider to synthesize a control character.
It is **intermittent**: the same command in the next run sent the whole sentence correctly, percent-encoded
and all. An intermittent wrong value in a fixed buffer is the signature of something writing past the end
of a neighbouring allocation; the reply buffer is one of two allocated together, and a first step is to
stop handing out whatever `malloc` left there:

- both chat buffers are cleared at allocation, so an unwritten reply is empty rather than garbage;
- the speaker refuses a reply whose first byte is a control character, logs the byte and the length, and
  says so instead of sending nonsense to a provider.

The root cause is not yet identified and is recorded as open. It was found by checking the wire bytes
rather than trusting either end, which is the same discipline that turned the previous session's log
encoding into a non-event.

---

## 2026-10-15 — Session 40: the bridge with a real key, and both directions verified

The owner supplied a SiliconFlow key, so the stand-in answers could be replaced with real ones. The
service's own documentation also corrected two of my assumptions, which is why it is worth reading before
guessing at an API:

- **The voice name must carry the model**: `FunAudioLLM/CosyVoice2-0.5B:claire`. A bare `anna` is
  rejected with `{"code":20047,"message":"Invalid voice."}` — a clear error, and the reason the first
  attempt failed. The eight preset voices are alex, benjamin, charles, david, anna, bella, claire and
  diana.
- **`sample_rate` is a request parameter**, so `response_format: pcm` with `sample_rate: 16000` returns
  exactly what this device plays. Nothing in the path needs decoding or resampling, which was the whole
  reason for choosing PCM in the first place.

### Both directions, verified with real models

The bridge now has `--selftest`, which round-trips the two directions with no device and no person:
speech synthesis is asked for a sentence, and the audio that comes back is fed straight into recognition
as if the device had recorded it.

```text
selftest: asking the provider to speak a Chinese test sentence
  synthesis returned 107438 bytes of 16 kHz PCM (3.4 s)
selftest: feeding that audio back as if the device had spoken it
  recognition returned that sentence: 17 of the 17 distinct characters, in order
```

A round trip through two models that returns the sentence **character for character** is more than the
test was written to expect. Both credentials, both request shapes and the rate handling are therefore
exercised — and this is the only way to test the microphone half without a person talking.

### The device, speaking real audio

```text
voice speak hello there        -> bridge received it, device played 14678 samples
voice speak (the last answer)  -> DeepSeek's Chinese reply, 37079 samples played (2.3 s at 16 kHz)
```

The second is the important one: the Chinese answer came from the model, went to the bridge as a
URL-encoded query, was synthesized by SiliconFlow and was played by the device. The wire bytes were
checked rather than assumed, because the log made them look wrong:

```text
raw = text=%E4%BD%A0%E5%A5%BD%EF%BC%8C%E5%BE%88%E9%AB%98%E5%85%B4%E4%B8%BA%E4%BD%A0%E6%9C%8D%E5%8A%A1%E3%80%82
      = the reply, correctly percent-encoded UTF-8
```

**The garbage was in the log file, not on the wire.** The bridge was started with a redirect through a
GBK console, so correct UTF-8 in memory came out of the log as replacement characters and the device was
blamed for a moment. The bridge now reconfigures its own output streams to UTF-8 whatever the console
code page is, because a log its own author cannot read back is a log that produces exactly this kind of
false conclusion.

### One real limitation found, and it is not in the voice path

`voice speak <Chinese text>` typed into the **serial console** loses its argument: the device echoes the
characters back correctly, then speaks the model's last answer instead, as though the argument were
absent. Typing Chinese into this console does not work, which also means a Chinese Wi-Fi SSID cannot be
typed there — the web console handles it, which is how the device is configured, but the serial console
should not quietly do the wrong thing. Recorded as a defect for the CLI rather than fixed here, because it
is not on the voice path: a transcript arrives in an HTTP response body and an answer arrives in JSON, and
both were proven above.

---

## 2026-10-15 — Session 39: the bridge exists, and its endpoints were verified one at a time

The owner asked which speech services to use and where to find them, and to go and look. Both, and then
the half of the system that was still a document: `tools/passport_bridge.py`.

### The recommendation

**SiliconFlow** for both directions, because one key and one OpenAI-compatible shape cover speech-to-text
and text-to-speech, `/v1/audio/speech` can return raw PCM, and that means no audio decoder anywhere in the
path — which is the same reason the device sends PCM in the first place. Alibaba Model Studio is the
second choice for Mandarin quality, at the cost of two different API shapes, which the bridge handles as a
provider mode. The reasoning and the links live in
[`voice-backends.md`](voice-backends.md#which-providers-to-use-right-now) rather than here.

Prices and model identifiers were deliberately not copied into the repository: they change, and a number
in a document that nobody re-checks is worse than a link.

### The bridge, and what was verified

[`tools/passport_bridge.py`](../../../tools/passport_bridge.py) implements the protocol in
[`bridge-protocol.md`](bridge-protocol.md) with no dependencies beyond the standard library, and a `--fake`
provider that answers without a key so the device side can be checked without spending anything. Verified
on the device, endpoint by endpoint:

```text
POST /voice      <- 92160 bytes, 2.9 s of audio, streamed while recording
GET  /speak      <- "hello from the bridge", and the device played what came back:
                    last="spoke 31 KB (16000 samples)" — one second at 16 kHz, exactly the tone sent
GET  /work-status <- the work screen's document, from the host and from the device's polling
```

The transcription itself returned the fake provider's canned text, and the `/voice/transcript` polling is
the one leg not yet seen against this bridge: a real turn reached the silence gate before it, which is the
next point.

### What a real turn still needs

A person. The device behaved exactly as designed when I drove a push-to-talk turn without speaking into it:

```text
last="90 KB, peak 369 below gate 942: discarded"
```

It streamed the recording while capturing it — which is why the bridge received 2.9 seconds of room tone —
and then, having measured a peak well below twice the room's own floor, threw the turn away instead of
sending a question it knew was empty. That is the correct behaviour and it is also the limit of what can be
verified without a human: the speech level of a person at this microphone is not something a test can
produce.

---

## 2026-10-15 — Session 38: the work screen stopped dragging the owner back, and the page can be checked from the device

Two reports from the owner. One was my mess; the other was a real bug, and the first report turned out to
be my mess as well.

### "Why does it keep saying index repo?"

Because I had left a stand-in bridge running on this machine and pointed the device at it: its canned
status document says `indexing repository`, 42%, for ever, and the device polled it every five seconds.
Stopped, and the device's `bridge_url` cleared.

But the report was worth more than the cleanup. **The work screen took over on every document, not only
on a changed one**, so leaving it did not stick: pressing OK to go home lasted until the next poll, five
seconds later, for ever. A bridge that reports the same task all afternoon would have made the device
unusable as anything else, which is exactly what the owner saw. It now takes the screen over only when the
document says something new — the task, the state, the question or the number of options — while still
refreshing what is on the screen, so looking at it always shows the latest.

Verified on the device: a first document takes the screen over, going home stays home through a repeat of
the same document, and a different document takes it over again.

### The page can now be checked from the device

`prov start` was reporting the access point up, but "is the page actually being served?" still needed
another machine on the same network to answer. It does not any more:

```text
prov fetch /            -> HTTP/1.1 200 OK, body begins with the page's doctype
prov fetch /api/status  -> HTTP/1.1 200 OK
```

`prov fetch` opens a socket to the device's own console — the access point's address first, loopback
second — sends a GET and prints the status line and the beginning of the body. A failure is now the
device's own, and a success means any client that can reach that address will get the page. This is the
kind of self-test the owner asked for two sessions ago, applied to the one question that was still
dependant on someone else's network.

### What the owner's browser still needs to know

The page exists **while the setup portal is running**, and on a device that is already configured the
portal starts when provisioning mode is entered — which the information screen on a long press of UP now
says. An always-on console was considered and rejected on measured grounds: with the HTTP server up there
is not enough contiguous memory left for the TLS session the voice path needs, and that trade was recorded
when the conflict was first measured.

The second thing is the browser rather than the device: this serves plain HTTP on port 80, and browsers
increasingly try `https://` first, so the address has to be typed with `http://` in front of it.

---

## 2026-10-15 — Session 37: the page was never being served, and the corner now says so

The owner asked two things: what address the device is on, and for a long press of UP on a connected
device to show the current network, its address and the web address, with entering provisioning mode as a
choice rather than a consequence.

Answering the first found the third real cause of "I cannot connect".

### The setup page was never served

The portal's HTTP server was failing to start, and nothing said so anywhere the owner could see:

```text
the portal's start trace, step 5 of 6, the HTTP server: ESP_ERR_HTTPD_TASK
and the error line beside it: the HTTP server failed to start
```

`ESP_ERR_HTTPD_TASK` means the server's task could not be created — no contiguous memory for its stack.
The stack size was 8,192 bytes, which was not a decision: it was the value copied from the example. By the
time it runs, the join code's pixels are already reserved (they have to be, or they will not fit
afterwards), so the device was holding tens of kilobytes while asking for eight more, and failing.

The consequence is worse than the error: the access point rises, the screen shows a join code and a
password, and **the page those credentials lead to is not being served**. Every symptom the owner
reported follows from that. The device has been walking into this since the pixel buffer was added.

Fixed by making the two fit: the server's stack is 5,120 (these handlers render a page into a heap buffer
and apply a form; they do not recurse), and the join code's quiet zone is two modules rather than the
specification's four, which is another 5 KB of pixels. The quiet zone is reported to the console by
`prov pixels`, so the check of the drawn symbol knows what to expect — and it still matches the encoder,
cell for cell, at 99 pixels.

A trace was added through the start sequence, which is how this was found in one run rather than by
guesswork:

```text
portal start: steps 1 to 4, ending with the hotspot configuration accepted (ESP_OK)
portal start: step 5, the HTTP server: ESP_ERR_HTTPD_TASK
```

`prov status` now reports `access point: up` and `wifi status` reports the page address with whether it is
being served right now, because "the page is not there" and "the network is not there" are different
problems and the device should say which one it is in.

### The long press now shows the information

```text
bubble:  the network name, then its address, then the web address, on three lines
hint:    OK enters provisioning mode, DOWN cancels
```

Someone who long-presses this button on a working device is usually after the address, not after a
re-provision, so making them enter the setup screen to read it — and raise a hotspot to do it — was the
wrong way round. Verified on the device: the three lines above appear, OK opens the setup screen, DOWN
returns the normal hint and stays home, and the question expires after twenty seconds.

### A note on this environment

For most of this session this host could reach the device at 192.168.3.75 and fetch its setup page. By the
end it could not: `ping` to the device's own gateway is answered by the host itself with "destination host
unreachable", so the *host* has lost its route to 192.168.3.0/24. That is a change on this machine's
network — it carries several virtual adapters, including VPN clients — and not something the firmware can
affect. It is recorded because the LAN page was verified working earlier and the reason it could not be
re-verified at the end is environmental rather than a regression.

---

## 2026-10-15 — Session 36: the corner shows the network, and re-provisioning asks first

Two changes the owner asked for, both about answering a question before it is asked.

### The top-right corner: Wi-Fi and battery

The corner now answers the two questions a glance at a pocket device should answer. `wifi` is a word and
`wifi_bar0` to `wifi_bar2` are three bars lit from the associated signal — three above −67 dBm, two above
−80, one below that, none when there is no association at all — next to `battery`, which was already there.
`passport_wifi_rssi()` was added for it and reads the driver on demand, because a stale signal figure in the
corner is worse than none.

The bars are objects rather than a glyph because this interface has no icon font: three rectangles whose
colour changes are cheaper than a font, and they are named so the console can see them like everything else.

Verified on the device by taking the link away and giving it back:

```text
wifi disconnect -> ui get wifi  text: --        battery 98%
wifi connect    -> ui get wifi  text: WiFi      battery 98%
```

The indicator refreshes about once a second, on its own cadence: association and signal do not change on
the battery's five-second schedule, and a corner that says "connecting" for five seconds after the link
came up is a corner that lies.

### Re-provisioning asks first

A long press of UP used to open the setup screen unconditionally. On a device that is already on a
network that is almost certainly a mistake — it raises a hotspot and puts a join code on the screen — so
now it asks:

```text
ui key up long -> the hint line asks whether to re-provision (OK to confirm, DOWN to cancel)
                  and the bubble names the network the device is on
ui key ok click -> screen setup        (confirmed)
ui key down click -> the normal hint, still on the home screen   (cancelled)
13 seconds of silence -> the normal hint, still on the home screen  (expired)
```

A device with no network configured skips the question, because for it the setup screen is the only way
forward and a confirmation would be noise. The question also sits above push-to-talk in the button
handling, so a press of OK answers it rather than starting a recording, and it expires after twelve
seconds so an unanswered question cannot leave the interface in a state that is neither normal nor setup.

### The console tool

`tools/passport-console.ps1` gained a `sleep <ms>` script directive, because a confirmation that expires
is behaviour that cannot be checked by a test that cannot wait. It also now decodes the port as UTF-8:
its first version appended one `char` per byte and turned every Chinese log line into mojibake, which is a
poor quality for the tool whose job is reading what the device says.

---

## 2026-10-15 — Session 35: the hotspot was being knocked down by the station, and provisioning now works over the LAN

**The owner's report:** still cannot connect. And a request worth acting on: *write the firmware you need and
give yourself a way to test networking, rather than depending on me.*

### A second real cause, found by looking for one

`passport_wifi_init()` forced `WIFI_MODE_STA` on every call. `passport_provision_start()` calls it, so the
hotspot survived that particular ordering — but **any station reconnect calls it too, and switching to
station-only stops a running access point.** The device auto-reconnects when its station drops, so the
sequence "open the setup screen, phone starts associating, router hiccups, station reconnects" took the
hotspot off the air in the middle of the phone's handshake. The phone reports that as a wrong password,
because it has nothing better to say.

The Wi-Fi layer now asks the driver for the current mode and keeps `WIFI_MODE_APSTA` when an access point
is running, instead of forcing station-only. Verified by doing exactly that:

```text
wifi disconnect   -> access point : up, mode : station + access point
wifi connect      -> access point : up, mode : station + access point, agreement : the radio matches the screen
prov events       -> nothing after the initial setup: no access point stop
```

### The diagnostics the owner asked for

- **`prov status`** now prints the driver's mode and reads the access point's configuration **back from the
  driver**, reporting whether the radio agrees with what the screen shows. "The password is wrong" is a
  claim about the radio, so the answer has to come from the radio.
- **`prov events`** — the last eight access-point events with uptimes, and **every client departure carries
  the Wi-Fi reason code, spelled out**: "4-way handshake timed out: the password did not match" is a
  different problem from "the client left on its own: often no internet on this network", and from "no
  attempt at all". Kept in a ring so an attempt made while nobody was watching can be read afterwards.
- **`prov clients`** — who is associated right now, with signal.
- **`prov selftest`** — and a correction. The first version scanned for its own hotspot and announced "NOT
  on the air" when it did not appear. **A radio cannot receive its own transmission**, so a self-scan can
  never see the access point it is hosting: that conclusion was invalid, and the command now says what it
  can and cannot prove. What does prove the interface is up is the driver's own `WIFI_EVENT_AP_START`,
  which the event log now records — a handler registered after `esp_wifi_set_config()` had been missing it.

### Provisioning over the LAN, which needed no new firmware at all

The evidence that mattered most came from asking the device a different question: the web console answers
on the **station** interface, not only on the hotspot. From this host, on the same network as the device:

```text
http://192.168.3.75/            HTTP 200, 3380 bytes   the full setup page
http://192.168.3.75/api/status  HTTP 200  {"wifi":"connected","rssi":-43,...,"largest_block":10752}
```

So the device can be configured from any phone or computer already on the network, without the hotspot, the
QR code or a typed password. That is now the documented first answer to "I cannot connect to it".

### What this session could not do, stated plainly

**Be the Wi-Fi client.** This machine has exactly one wireless radio, its radio state is `Software Off`, and
enabling it needs an administrator privilege this session does not have — the attempt was refused. So the
last mile, an actual phone associating, still needs a person. What is verified from the device side is that
the interface is raised (the driver's own event), that the mode is station-plus-access-point, that the
address is 192.168.4.1 on channel 6, that the authentication is WPA2-PSK, and that the radio's configuration
matches the screen character for character.

---

## 2026-10-15 — Session 34: "the password is wrong" was a crash, not a password

**The report from the owner:** the phone said the Wi-Fi password was wrong and would not connect.

It was not the password. On the device, `prov start` aborted:

```text
E esp_netif_lwip: esp_netif_new_api: Failed to configure netif (config or if_key is NULL or duplicate key)
assert failed: esp_netif_create_default_wifi_ap wifi_default.c:408 (netif)
```

**What was wrong.** `passport_provision_start()` created the access point's network interface on every
call, and `passport_provision_stop()` never destroyed it. So the **second** time the setup screen was
opened, `esp_netif_create_default_wifi_ap()` was called on an interface that already existed — and that
function does not return an error, it asserts and reboots the device. The setup screen had already drawn
the hotspot name, the password and the join code, so from the owner's side the device was advertising a
network, the code looked right, and the phone would not associate. A phone reports that as "incorrect
password", because that is the only thing a user can act on.

This is the same class of defect as the one found in Session 31, in the same file: state that is created
per call and released per call, where one of the two sides was not written. The first was a stack, this
one an interface.

**The fix.** The page buffer and the network interface are created once and kept for the lifetime of the
run; the interface is no longer recreated on start nor expected to vanish on stop. Verified by the
sequence that used to abort it:

```text
prov start -> access point : up
prov stop
prov start -> access point : up
prov stop
prov start -> access point : up
```

No assert, no register dump, and the hotspot is up after each cycle.

**The console now answers this question directly.** `prov status` reads the configuration back **from
the Wi-Fi driver** and compares it with what the screen shows, because "the password is wrong" is a claim
about the radio rather than about this application's strings:

```text
access point : up
ssid         : Passport-91E1
password     : <ten characters, shown on the device's screen>
radio        : ssid "Passport-91E1" password "<the same ten characters>" auth WPA2-PSK channel 6
agreement    : the radio matches the screen
```

**And the password can no longer be misread.** It used to be eight hexadecimal digits, which contain
`1` — the character most often mistyped as `l` or `I` — twice, typed by a person from a 240-pixel screen.
It is now ten characters from an alphabet with `i`, `l`, `o`, `0` and `1` removed, which is both easier to
type and stronger (31^10 against 16^8). The derivation moved into `passport_identity.c`, which is pure
and host-tested: the test asserts the length, the absence of every confusable character, that the same
board always produces the same credentials, and that neighbouring MAC addresses do not.

**Not verified, and it is the one thing that matters to the owner:** that a client can associate. This
machine's wireless radio is software-off and enabling it needs a privilege this session does not have, so
the host could not be the test client. The radio's configuration, the authentication mode, the channel and
the agreement with the screen are all confirmed on the device; the association itself needs a phone.

---

## 2026-10-14 — Session 33: the complete gate, and the deep-sleep omission it found

**Milestone:** the repository's whole validation gate runs, and it found something that had been missing
since the application replaced the demo.

### The gate is larger than the tests I had been running

`tools/validate.sh --static` builds and runs **17 C tests and 6 Python contract tests**, not the ten
passport tests I had been reporting. Every earlier entry in this log that says "10 of 10 host tests
PASS" is a true statement about the subset I ran and a misleading one about the repository: the
upstream tests for the BSP (`test_bsp_button`, `test_bsp_lvgl_init`, `test_bsp_audio_recovery`,
`test_bsp_display_rounding`, `test_bsp_es8311_sleep_check`), key handling, and the Python contracts were
never part of it. They are now, and they pass. From here the count is the whole suite.

### Two failures, and one of them was a real omission

Four demo runtime tests failed to compile and one Python test errored, all for the same reason: they
read `main/demo_audio.c`, `demo_ble.c`, `demo_low_power.c` and `demo_wifi.c`, which this project
deleted when it replaced the demo interface.

The first four are tests of a removed feature and were removed with it, along with the demo stub
harness they were the only users of. But `test_deep_sleep_contract.py` was not a test of the demo — it
was a **contract about the order in which this board must be shut down before sleeping**, and it was
reading the demo's `sleep_task` only because that was the last code to do it properly. Retargeted at
the replacement instead of deleted, it failed immediately:

```text
AssertionError: function not found: sleep_task
```

because **the replacement never shuts the board down at all.** `passport_power_sleep()` checked the key,
killed the buttons, armed the wake source and slept — leaving the battery gauge awake, the codec
powered, the I2S and I2C pins driven, and the panel's charge pumps running. On a board where the
standby current was always going to be an external measurement, that would have been the difference
between deep sleep and pretending.

The sequence the contract asks for is now in `passport_power_sleep()`, in the reference order, with the
panel teardown under the interface's lock: battery, codec, I2S pins, I2C pins, panel, then the sleep.
The contract names `passport_power.c` now and passes.

The lesson is worth stating plainly: **when a feature is replaced, its contracts have to be moved, not
just its tests.** The tests that only exercised the old code were noise; the one that encoded a rule
about the hardware was the whole reason the old code was correct.

### The demo is now fully replaced

`demo_navigation.c` and `demo_radio.h` were unreferenced and `demo_navigation.c` was still being
compiled into the image, so it was dead flash. Both are gone, with their test, their stubs, and their
lines in the gate. Nothing under `main/` or `tests/` matches `demo` any more.

### The firmware half of the gate

```text
idf.py -B <own dir> build                PASS
idf.py merge-bin -o <absolute path>      merged image, 2,349,776 bytes
tools/verify_firmware.py                 Firmware layout: PASS (app 2,284,240 / 8,323,072 in 'factory')
                                         Merged firmware: PASS (2,349,776 bytes, flash at 0x0)
tools/archive_firmware.py create         archive keyed by content hash: bins, elf, map, manifest
```

One Windows trap worth recording for anyone mirroring the bash gate by hand: `merge-bin` runs esptool
with the **build directory as its working directory**, so a relative `-o build/fwgate/...` becomes
`build/fwgate/build/fwgate/...` and fails with a `FileNotFoundError` that names the file it could not
write. The gate's own script passes an absolute path, which is why it never noticed.

### Verified on the device after the change

The gate-verified build was flashed and the new shutdown path exercised: `power sleep 15` put the device
to sleep with the console vanishing immediately, and after waking, `audio status` reports the path free,
`notify test question` plays 4096 samples through a codec that had been put to sleep, and `ui audit`
reports no font problems on a panel that had been shut down and held. The shutdown is not one-way.

---

## 2026-10-14 — Session 32: the join code verified at both levels

**Milestone:** the code the device draws has been checked against an independent implementation, and the
pixels it draws have been checked against its own encoder. What is left is a camera.

### The symbol

`tools/check_device_qr.py` reads the matrix `prov qr` prints, regenerates the same payload with the
`qrcode` package for all eight masks, and compares cell for cell:

```text
device: version 3, 29 modules, states mask 2, matrix 29x29
MATCH: the matrix is the reference symbol for fixture 'v3_wifi' under mask 2,
       which is the mask the device states
```

The mask is the interesting part, and it is worth being precise about what this does and does not
settle. It does **not** say which mask should win — that has never had an independent oracle, since the
`segno` package disagreed with the specification about a padding byte when it was tried. What it says is
that the matrix the device drew is a valid symbol for that payload **under the mask the device claims**,
which is a property the reference implementation can confirm. The claims and the drawing agree.

The payload comes from `make_qr_fixture.py`, which is the one place it is recorded, so nothing new was
added and the captured matrix — which carries the hotspot password — is never committed.

### The drawn pixels

A correct symbol can still be unscannable: inverting the two colours leaves a matrix that is perfect
cell for cell and unreadable by any camera. Only the pixels can tell you that, so `prov pixels` samples
the centre of every module in the buffer that was actually painted and compares it with the encoder's
matrix:

```text
prov pixels: 111 px, scale 3, 37 modules, quiet zone 4
prov pixels: 419 of 1369 module centres are dark (31%), 0 disagree with the symbol
prov pixels: MATCH - the drawn pixels are the encoded symbol
```

It also prints the top-left seven by seven as drawn and as encoded, which is what made the one failure
below obvious in a single look. The dark fraction reads as 31% of the canvas, which looks wrong for a
symbol that should be about half dark — and it is right: the canvas includes the four-module quiet
ring, which is 528 of the 1369 centres and entirely light. The symbol area itself is **49.8% dark**,
which is exactly what a QR symbol should be.

### The checker was wrong first

The first run reported 428 of 1369 centres disagreeing. The corner dump showed the drawn finder pattern
shifted by exactly two modules, and the reason was in my checker rather than in the firmware: it
hard-coded the quiet zone as two modules, and the application's is four. The application now **reports**
its quiet zone to the console instead of the console keeping a copy.

This is the third time in this project that a checker carrying its own copy of a constant produced a
false failure — the other two were in Session 30, and one of those was a wrong conclusion about ESP-IDF
that reached a protocol document. The pattern is consistent enough to name: **a checker that duplicates
a constant is checking its copy, not the firmware.** The remedy each time was to ask the component that
owns the value.

### Not verified, and it is now a narrow question

Whether a phone can read the code off this panel — the contrast, the size on a 240-pixel-wide screen,
and whatever the glass does to the light. Everything up to the glass is now checked twice, by an
independent implementation and against the device's own renderer.

---

## 2026-10-14 — Session 31: an acceptance walkthrough, which found a crash and three other defects

**Milestone:** the interface was walked the way a person would walk it, and it does not survive that
walk yet. Four defects, one of them a crash, all now fixed and verified.

### 1. A stack protection fault on a long press of UP

The gesture that opens the setup screen **rebooted the device**, every time:

```text
Guru Meditation Error: Core 0 panic'ed (Stack protection fault).
Detected in task "passport_input"
```

It is my regression. In Session 27 I trimmed the button task's stack from 12,288 to 6,144 on the
reasoning that it only queues events now that the QR encoder lives on the app worker. That reasoning
was wrong: the long-press-of-UP branch still called `passport_app_show_setup()`, which builds the
screen, which encodes the join code, which needs about six kilobytes. The trim removed the room and
left the call.

Everything heavy about that screen now happens on the worker — the button task sets a flag and the
worker reserves the pixels, starts the access point and builds the screen. **The same mistake was made
twice inside this one fix**: the first attempt moved `qr_reserve()` into the button branch, which put
the encoder back on the same small stack and crashed identically. That is worth recording as a rule
rather than an anecdote: *the button task's stack is a constraint on every call on that path, and it
has to be checked when a call is added, not when it crashes.*

### 2. The join code stopped rendering

With the setup screen finally reachable, it showed the failure label instead of a code. The pixels are
24,642 bytes and the access point, started first, takes tens of kilobytes and fragments what remains.

Fixed by reserving the pixels at the moment the screen is requested and **before** the access point
starts, then releasing them when the screen closes. That is the original reservation, restored with
its reasoning intact — the code cannot be retried if the allocation fails, so it goes first — and with
only its duration changed. Held for ever, those bytes left no room for a TLS session, which is how the
cost was found in Session 27.

```text
passport_app: the join code's buffer is ready, 24642 bytes (111 pixels)
passport_app: the join code was generated, version 3, 29 modules, scale 3, 111 pixels, 40-byte payload
```

### 3. Chinese text drawn with a font that has no Chinese glyphs

Four labels — the home hint and three on the setup and work screens — were created with Montserrat, the
Latin font, while carrying Chinese text. On the glass they are empty boxes. The repository's own guide
warns about exactly this and says a successful build proves nothing; what was missing was a way to
check it on the device.

There is one now. `ui audit` walks every screen, and for every label whose text contains a non-ASCII
character it asks the font that label is actually drawn with whether that character exists. It is a
runtime check rather than a source review because the two facts live in different places.

**It was run before the fix, and it failed** — which is the only reason to trust it:

```text
MISSING: passport/hint can not draw U+63D0, so the hint line has holes
ui audit: 5 labels checked, 1 with characters their font cannot draw
```

After the fix: 0 findings across all three screens, 15 labels on the home and work screens and 11 on
the setup screen.

### 4. The on-screen hint described controls that no longer exist

It still told the owner that OK asks a question and that holding it stands by — from before OK became
push-to-talk and before sleep moved to a double press of UP. A hint that lies is worse than no hint,
because it teaches the wrong gesture. It now names the four real gestures — hold OK to talk, hold DOWN
for work mode, hold UP for setup, double-press UP for standby — in the font that can draw them.

### Also observed, and correct

An injected gesture was silently swallowed early in the walkthrough. The screen had gone to standby
after the idle timeout, so the press was consumed as a wake-up and did nothing else — the arbitration
from Session 25 doing its job. It cost me a confusing minute; it is recorded because it is the same
confusion a person will have.

### Not verified

Whether a phone can read the code on the glass. With the portal up, the largest free block is 7,680
bytes, so a snapshot of it cannot be allocated either. The matrix the device generates is printed by
`prov qr`, so the symbol itself can be checked against an independent implementation of the same
payload — no decoder is installed in this environment, and installing one is not this session's
business. That is the next thing.

---

## 2026-10-14 — Session 30: the silence threshold now measures the room, and two wrong defect reports retracted

**Milestone:** the device measures its own room at start-up and sets its silence threshold from that
measurement. And two defects I reported last session turn out not to exist, which is the more important
half of this entry.

### The retractions

**"A press does not start recording for about 1.4 seconds."** It does not. Measured with instrumentation
added for the purpose:

```text
the speech task logs (in Chinese, as it logs): press->wake 0 ms,
  wake->screen 0 ms, screen->connection ready 26 ms
```

Twenty-six milliseconds from the press to the first sample. A second check with the user-facing case —
a 300 ms press — captured 11,520 bytes, which is 0.36 seconds of 16 kHz audio. The recording tracks the
press. The 1.4 seconds was in my test harness: it wrote a command, waited out a fixed read window, then
sent the release, and I read the resulting empty recording as a firmware fault.

**"The silence gate's default sits inside the ambient range and would discard real speech."** Also
wrong, and wrong in the other direction. Ambient peaks in this session ranged from 216 to 886 across
runs, and speech was never actually measured, because every automated test was typed at a console rather
than spoken. A fixed threshold of 400 therefore did not discard speech — it was simply a guess that
filtered quiet moments and let noisy ones through.

### What was actually wrong, and what was done about it

A fixed threshold is a guess about a room nobody measured. The boot warm-up — half a second of capture
that already existed to settle the codec — now **measures the room as well**, skipping its own settling
transient, and the gate is twice the measured peak, clamped between 300 and 4000. The console can pin a
value, and 0 disables the check. `voice status` reports the number and where it came from.

Verified on the device:

```text
voice gate -> state=idle mic=closed turns=0 gate=562 (twice the measured room)
a quiet press -> 48 KB, peak 252 below gate 562: discarded
```

The room measured 281 peak at start-up, the gate came out at 562, and a press with no speech in it was
discarded — the threshold did its job on the first try, in the room it was measured in. The audio had
already been streamed when the decision was made, so the saving is the provider call rather than the
bytes; that is stated in the code.

**What is still not verified, and cannot be by me:** whether the gate is set correctly for a person
speaking into this microphone. The margin is described as a heuristic in the code, not as a tuned value,
because the speech level is something only a person can measure. The override exists for exactly that.

### The pattern, named

This is the second session in a row in which I published a firmware defect that was really an artifact of
my own measurement. Both had the same shape: a claim inferred from a test script's behaviour rather than
from a number the device reported. The remedy being adopted rather than promised: **instrument first,
then report.** Where a latency or a level is in question, the device logs the measurement before any
conclusion is drawn, and the instrumentation is kept afterwards — the press-to-sample breakdown stays in
the firmware precisely because it is the number that settled the question.

---

## 2026-10-14 — Session 29: the answer is spoken, and a wrong diagnosis corrected

**Milestone:** the voice loop is complete. Hold OK, speak, release: the audio streams to the bridge,
the transcript comes back, the model answers, and **the answer is played through the speaker** — with
the microphone provably shut for the whole utterance.

### What was built

`GET <bridge>/speak?text=…` returns the spoken form as raw PCM, the same format the device uploads, so
one shape covers both directions. The device streams it into the speaker as it arrives, and the audio
path is taken **once for the whole utterance** by a new `passport_audio_play_begin/_chunk/_end`. That
detail is the point: playing chunk by chunk with the one-shot call would hand the arbiter back hundreds
of times inside a single answer, leaving room for a capture in between, which is exactly the hole the
invariant exists to close.

A long answer is spoken as a bounded prefix — about 190 bytes, cut on a character boundary. A kilobyte
of Chinese is half a minute of audio, and percent-encoding triples whatever it carries; the full text is
on the screen and the log says when it was shortened. Replies are Chinese, so the query string needed a
percent-encoder, which is now a pure function with host tests of its own — including that a truncated
encoding is refused rather than sent as a bare `%` or `%E`.

### Verified on the device

```text
hold, speak seven seconds, release
  released after 361 chunks, 231040 bytes
  bridge log: POST /voice -> 231040 bytes of PCM (7.2 s at 16 kHz)
              GET  /voice/transcript -> ready=1
  transcript: which branch should I deploy
  bridge log: GET /speak -> text='I can help you figure out which branch to deploy,
              but I need a bit more context. Could you share: ...'
  audio status: owner = playback, microphone = closed (playback holds the path)
  afterwards:   state=idle, owner=free, microphone=allowed, 46248 B free / 32768 largest
```

The middle two lines are the whole feature: the model's own answer was asked for as speech, and while it
played, the arbiter held the path and the microphone was shut. The last line is the turn closing
cleanly with nothing left behind.

### The correction I owe this project

Session 27 concluded that this HTTP client "cannot read the answer after a chunked upload", and Session
28 built a protocol around it. **That was wrong.** `esp_http_client_fetch_headers` returns a *length* —
a positive content length, 0 for a chunked body, -1 on error — and the code compared it with `ESP_OK`,
so every response that had a body looked like a transport failure. The client is fine.

What makes this worth recording is the shape of the error rather than the bug: a plausible-sounding
limitation in someone else's code was accepted and written into a contract, when the only evidence for
it was a log line produced by my own type confusion. The code comment, the protocol document and the
progress log all now say what actually happened. The two-request design is kept — a bridge that spends
seconds transcribing should not have to hold the upload's socket — but it is kept on its own merits
rather than on a defect that never existed.

The second ESP-IDF observation from Session 27 stands and is verified: the client declares a chunked
request body and writes the caller's bytes unframed.

---

## 2026-10-14 — Session 28: the voice turn works, end to end

**Milestone:** hold OK, speak, release — and the answer comes back. The audio streams to the bridge,
the transcript returns, the model is asked, and the reply is on the screen. The reply is not spoken
yet, and that is the one piece of the voice milestone still missing.

### How the blocking defect was cleared

Session 27 ended with a specific failure: the upload arrived byte-exact and the client could not read
the answer. The fix is not to fight the client — it is **two requests**. The upload is
fire-and-forget, and the transcript is fetched the way this project fetches everything else: a plain
`GET`, which the work client had already proven on this device. Both ends are mine, so the protocol
was shaped around the transport's weakness rather than the other way round. It is written into
[the bridge protocol](bridge-protocol.md) with the reason, so a future reader does not "simplify" it
back into one request.

The first attempt then failed in a way worth recording. The bridge answered `{"ready": true, ...}`
and the device polled six times and gave up. The device's reader is an **integer** reader — the same
one the work document uses — so a JSON boolean is not a value it can see. A missing `ready` now counts
as ready, which is tolerant in the right direction, and the protocol specifies the field as `1` or
`0`. The lesson is about the seam, not the parser: two components can agree on a protocol while
disagreeing about a type, and only an end-to-end run finds that.

### Verified on the device

```text
voice hold -> speak for seven seconds -> voice release
  the speech task logs (in Chinese): released after 352 chunks, 225280 bytes, rms=366 peak=886
  the speech task logs: audio delivered (220 KB), fetching the transcript
  the speech task logs: transcript: which branch should I deploy
  voice status: state=idle, transcript="which branch should I deploy",
                last="turn finished: said ..."
  bridge log:   POST /voice -> 225280 bytes of PCM (7.0 s at 16 kHz)
                GET  /voice/transcript -> ready=1
  the model's reply: 1023 bytes in 2796 ms
```

A second turn was then run to prove the first one really finished: 261 chunks, 167,040 bytes, and
`turns=2` with the state back to `idle`. Memory afterwards read 48,612 bytes free with a 34,816 largest
block, so a turn leaves nothing behind.

### The defect this run exposed, which is real and not fixed

**A press does not start recording for about 1.4 seconds.** The turn opens the microphone, updates the
screen, and connects the socket before the first sample is read, and that is longer than it sounds: a
short press captures nothing at all, which is exactly what several runs in this session showed before
the cause was understood. The fix is to start capturing immediately and hold the first fraction of a
second in a small pre-buffer while the connection is made, then flush it — the memory for that is
available now, and the design is a few lines. It is not done, so a person pressing and speaking
immediately loses the beginning of their sentence.

### Also worth stating

The silence gate's default of 400 is **inside** the range this room produced: ambient peaks measured
366 to 671 across these runs, and speech 886 to 1,821. A gate in the middle of the ambient range would
discard real speech. It was bypassed with `voice gate 0` for every test here, so the default is
untested against real use and should be re-derived from a proper measurement rather than guessed
again.

---

## 2026-10-14 — Session 27: push-to-talk streams audio, and a memory regression found on the way

**Milestone, partial:** the microphone opens on the press, the speech streams to the bridge while it
is still being recorded, and the bridge confirms the exact number of bytes. Reading the transcript
back does not work yet, and that is the one thing left.

### What was built

`passport_speech` owns the audio and the socket; the turn rules stay in `passport_voice`, which was
already pure and host-tested. A press opens the microphone, a release ends the turn, and OK is now
push-to-talk rather than a text turn — one button, one meaning. Deep sleep moved to a double press of
UP, because holding OK to speak and holding it to sleep cannot both be true.

The recording is **streamed while it is captured**: 20 ms chunks, each written to the request body as
it is read from the codec, so no buffer holds the whole utterance. That is not an optimisation. A few
seconds of speech is tens of kilobytes and the device does not have them; the alternative designs
were all rejected in the header of `passport_speech.h`, including the one that looks simplest — a
bounded recording posted as a WAV — because a WAV needs its length in the first 44 bytes, which is
exactly what an unknown-length recording does not have.

The room's own level is measured during capture, and a recording below a threshold is treated as an
empty room: the transcript is discarded rather than requested, which saves a provider call. The
threshold is adjustable from the console, and it needed to be — see below.

### Verified on the device

```text
voice hold / voice release, hold three seconds
  the speech task logs (in Chinese, as it logs): recording started, held=1
  the speech task logs: first chunk captured, capture=ESP_OK, held=1
  the speech task logs: released after 213 chunks, 136320 bytes, rms=401 peak=1005
  bridge log:      POST /voice -> 136320 bytes of PCM (4.3 s at 16 kHz)
```

The byte count is the check that matters: 213 chunks of 640 bytes is 136,320, and 136,320 bytes at
16 kHz mono is 4.3 seconds, which is how long the button was held. **The audio really did leave the
device while it was being recorded.**

### The memory regression, which mattered more than the feature

While testing this, `llm ask` began failing with `ESP_ERR_HTTP_CONNECT`. It was not the network. The
device was nearly out of memory:

```text
[mem] free=4972 B  min=40 B  largest=1664 B      <- before
[mem] free=50428 B min=43544 B largest=38912 B   <- after the fix
```

Two causes, both mine. **Thirty-two kilobytes of QR pixels were reserved at boot and held for the
life of the device**, on the reasoning that reserving them made the join code's rendering certain.
That certainty cost more than it was worth: with the buffer held, the largest free block at rest was
7,680 bytes and a TLS session could no longer be allocated at all. The pixels are now allocated when
the setup screen opens and released when it closes, which is sound precisely because provisioning and
provider calls are never needed at the same moment. The second cause was four task stacks I had
inflated defensively while chasing earlier faults; they are back to what their work actually needs.

The fix is verified by the thing it broke: a full model round trip returns, with the certificate
validated, a 492-byte answer, and 1,759 ms on the clock.

### Two ESP-IDF behaviours found the hard way

- **`esp_http_client` declares `Transfer-Encoding: chunked` when opened with a negative write length
  and then writes the caller's bytes unframed.** The first bytes of the body are therefore read as a
  chunk size by the server. This was found because this project's stand-in bridge logs what it cannot
  parse, and what it logged was the first samples of the recording. The chunks are now framed by the
  caller, with the reason recorded beside the code.
- **After a chunked upload, the client cannot read the answer**: `fetch_headers` reports a transport
  failure, and reading the data directly returns nothing, even though the server answered. The bridge
  is known good — a hand-made chunked request to it returns `200` with the transcript. This is the
  remaining defect.

### Still not working, and exactly how

The transcript never arrives, so the model is never asked and the voice turn is not complete. What is
verified is the half that was in doubt — capture, streaming, byte-exact delivery — and the failure is
now a specific, reproducible one on one side of a boundary that has been tested from both directions.

---

## 2026-10-14 — Session 26: the web console covers every setting, and does not leak them

**Milestone:** the device can be configured completely from a phone. Every setting the console can
change, the setup page can change, and the page shows live status while it does it.

### What was built

The setup page is now **generated from the same table that parses the submission**. One table lists
each form field, the configuration field it writes, its label, and its group; the page iterates it
and the parser applies it. A setting added to the schema and forgotten here is now caught by the
host test rather than by a user who cannot find it.

Three checks live in that test, and they are the reason the table is worth having: every entry names
a field the schema really has; no two entries write the same setting or share a form key; and **the
table covers every settable field in the schema**, counted rather than assumed.

Whether a field is secret is asked of the schema instead of being repeated in the table, so a key
cannot become visible because someone forgot to mark it twice. Secrets render as an empty input
labelled with whether one is stored; blank still means "keep".

The page is grouped — network, chat model, voice, work mode, and preferences, each heading written
in the interface's language — and shows live state: Wi-Fi, hotspot,
address, battery, free heap, with links to `/api/status` and `/api/config`.

### Verified on the device

```text
prov page        3,380 bytes, complete HTML, 19 inputs, 5 group headings
                 the page contains neither the stored Wi-Fi password nor the API key
                 (checked against the actual values, not eyeballed)
prov form pet_name=Whale2&notify_mode=2&idle_off_seconds=30
                 applied and saved; config get confirms all three
                 the two secrets still read "(set)": a blank field did not clear them
prov page status {"wifi":"connected","rssi":-41,...,"free_heap":21664,"largest_block":11264}
```

### Two mistakes of mine on the way

- **The console's preview was cut short while the page was fine.** `prov page` printed into a
  2,048-byte buffer while the HTTP path used 4,096, so the preview ended mid-tag and the page looked
  broken. The console now uses the same size; the lesson is that a debugging path with a different
  limit from the real one invents bugs.
- **My check for a complete page was wrong, not the page.** I asked whether the captured text ends
  with `</html>`; it ended with the console's `passport>` prompt, so the answer was no. The renderer
  had reported 3,380 bytes and the footer was in the capture all along. Worth writing down because
  the wrong check nearly sent me into the renderer.

I also wrote, and then removed, a genuinely bad piece of code: a battery figure inserted by
searching the rendered page for a placeholder and `memmove`-ing the rest of the string. Formatting
the string before the `snprintf` is what it should have been from the start.

### A measurement worth putting in the budget

With the setup portal running and the code on screen, free heap is **21,664 bytes** and the largest
block is **11,264**. Both the page buffer and the QR pixels are held at once, on top of the access
point and the HTTP server. The consequence is concrete: **direct-mode TLS needs about 24 KB
contiguous, so it cannot run while the portal is up.** Provisioning and provider calls must not
overlap, which is already true of the design — a device being set up has nothing to say to a model
yet — but it is now a measured constraint rather than an assumption.

---

## 2026-10-14 — Session 25: standby, deep sleep, and the wake-level bug it found

**Milestone:** the device turns its screen off when nobody is there, lights it again on a key
without also doing what that key means, and can sleep deeply on request.

### What was built

`passport_power` owns three things:

- **Idle screen-off.** After the configured `idle_off_seconds` the backlight goes off; the
  interface keeps running, so the screen returns instantly rather than being redrawn from nothing.
  The timeout is read live, so changing it takes effect without a reboot, and **0 means never**,
  which is the shipping default: the owner asked for an always-connected device first.
- **The wake-versus-act arbitration.** Standby and the application share the same three buttons, so
  the first press after the screen goes dark only lights it and is otherwise swallowed. Verified by
  watching the pet's mood: it stayed `idle` through a press that lit the screen.
- **Deep sleep**, on a long press of OK. The BSP hands the button pin back as a plain digital input
  first — with the ADC still driving it the pin reads low, which is the level the wake condition
  tests, so the device would wake at the instant of sleeping.

Two refusals are deliberate rather than defensive padding. If a key is **already held** when sleep
is requested, the request is refused with the reading that caused it, because the wake condition
would already be satisfied. And if arming the wake source fails, the device does **not** sleep:
better a device that stays awake than one nobody can wake.

### The bug the device found

The first attempt woke immediately and looked as though it had never slept. The cause was mine: the
BSP reports the level the pin settles at **with nothing pressed** (1 here, because the keypad node is
pulled up), and I armed the wake on that level instead of on the level a press produces. The wake
condition was therefore true at the instant of sleeping.

This is the hazard the BSP's own comment warns about, in the opposite direction — it warns that
leaving the ADC in charge makes the pin read low and wakes the device instantly; it does not warn
that the returned level is the *idle* one. The mapping is now inverted with the reasoning written
beside it.

### Verified on the device

```text
boot                power: screen=on idle=8s off_after=60s
config set idle_off_seconds 10        (live: no reboot, no save)
13 s later          power: screen=off idle=24s off_after=10s
ui key ok click     the power module logs that the screen lit up (in Chinese, as it logs)
                    power: screen=on idle=1s
                    the pet's mood stayed "idle": the press was a wake-up, not an action
power sleep 20      the console disappeared within about a second and the device stopped
                    answering, which is what deep sleep looks like from here
afterwards          the device answers again, so it woke
```

**Which source woke it cannot be seen from this console**, and that is worth stating rather than
guessing: opening the port toggles DTR and RTS, which resets the chip — observed as
`rst:0x15 (USB_UART_CHIP_RESET)` — so the wake cause is erased before it can be read. The evidence
that it was the timer is a deduction: during deep sleep the USB peripheral is powered down and
cannot reach the chip, and no key was pressed, so the armed timer was the only possible source.

### What is not verified

- **Waking by key**, which needs a person to press one: long-press OK to sleep, then press any key.
  That is the shipping path, and it is the check this milestone hands over.
- **Standby current.** It needs an external USB power meter, as recorded when the measurement plan
  was written; no number is invented here.

---

## 2026-10-14 — Session 24: work mode makes a sound, and only when it should

**Milestone:** the device chimes when the desktop needs its owner, decides that from the document
alone, and stays quiet the rest of the time.

### What was built

`passport_tone` generates a sine in integers — no floating point, no table of unexplained numbers,
no library. Bhaskara's approximation on the half wave, mirrored, with the angle kept in units of
1/1024 degree and a linear rise and fall so the note does not click.

`passport_notify` sequences two or three notes and plays them through the existing audio path, so
the chime is subject to the same arbitration as everything else: **the microphone is closed for the
duration**. A notification that recorded itself would be a defect on a board with no echo
cancellation, and the arbiter already forbids it.

The decision of *whether* to make a sound is pure and lives in `passport_work`, because it is a
rule about work states rather than about audio: a new question and a finished or failed task are
worth interrupting for; running and idle are not; and `notify_mode` 0 means silence whatever
happened. Mode 2 asks for speech, which is not wired to work mode yet, so it is reported as its own
answer and the device chimes while saying in the log that speech is not connected — a device
configured for speech should still be audible rather than silent about it.

Two judgements worth naming: **a poll every five seconds must not chatter**, so only a change is
announced; and **the first document after boot is not announced**, because a question may have been
waiting for hours and chiming at power-on teaches the owner to ignore the sound.

### The tone test found a real bug, in the generator

The test checks the waveform against the C library's sine, so it measures the **total** error rather
than the approximation's own. That caught a mistake the formula's reputation would have hidden: the
angle was quantised to whole degrees, and near the ends of the half wave that alone put the sample
up to 1.75% away from a true sine — an order of magnitude worse than Bhaskara's 0.16%. Keeping the
angle in 1/1024 degree fixed it, and the test now asserts the worst error over a whole turn is under
0.5%.

The test also asserts the pitch by counting zero crossings (1000 Hz over a quarter second is about
250 of them), that the ends of a note are silent, that filling in chunks gives byte-identical output
to filling at once, and the degenerate cases: a fade longer than the note, a zero-length note, an
amplitude above full scale, and a zero sample rate.

A mistake of mine in the test itself is worth recording: it first asked the generator for 16,000
samples into a 4,096-sample array and crashed. The generator did exactly what it was asked; sizing
the caller's buffer is the caller's job.

### Verified on the device

```text
notify status   -> buffer=ready, notify_mode: 1
notify test question / done / failed
                -> ESP_OK each time, 4096 samples rendered
audio status    -> owner free, microphone allowed again afterwards
```

Then the live behaviour, against the stand-in bridge: with the bridge serving a `running` task the
device polled for ten seconds and **logged no chime at all**; when the bridge switched to `waiting`,
the device logged the question chime and the screen read `Which branch?`. So it announces a change
and nothing else.

### Not verified

**Whether the chimes sound right.** The generator is measured, the playback reports success, and the
arbitration is checked — but nobody in this session has heard them. That is a person's check, and it
is the same class of check as whether the QR code scans.

---

## 2026-10-14 — Session 23: work mode against a real server, end to end

**Milestone:** the device fetches a status document from a server on the network, shows it, and
sends an answer back. The whole loop was exercised against a real HTTP server, not a feed from the
console.

### What was built

`passport_work_client` polls `<bridge_url>/work-status` on its own task and feeds the parsed view
to the screen, and posts the chosen option to `<bridge_url>/answer`. Plain HTTP rather than TLS,
for a reason recorded with the decision: the bridge is a companion on the same network, the
document carries no credentials, and skipping the handshake keeps the largest free heap block at
112 KiB instead of 68 — which this board needs for the display and the audio path. The consequence
is written down too: anything on the network can serve a document or read an answer, so the bridge
must not leave a trusted LAN before the reserved `bridge_token` becomes authentication.

The protocol is now a document rather than an understanding:
[the bridge protocol](bridge-protocol.md), with the field table, the rules the device applies, and
what it deliberately does not do. `tools/mock_bridge.py` is a stand-in server for testing without
the companion, and it is what the verification below used.

### Verified end to end

The device was pointed at a real HTTP server on this machine over the local network:

```text
config set bridge_url http://192.168.2.183:8080 ; config save
work client   -> polls=2 failures=0 last="fetched, state=waiting"
ui get work_task     -> text: deploy
ui get work_state    -> text: waiting
ui get work_question -> text: Which branch?
ui get work_option0  -> text: > main
```

Nothing was fed over the console: **the screen updated itself from the network.** Then the answer
path, from a button press to the server's log:

```text
work key down  -> the marker moved to develop
work key ok    -> answer -> develop
work answer    -> ESP_OK, last="answer accepted"
the server recorded: [{"answer": "develop"}]
```

And the device follows a change rather than only fetching once. The server was restarted with a
different scenario and, without any command, the device reported `last="fetched, state=done"` with
the screen reading `indexing repository` / `done` / `100%`.

### Notes

- The device's `bridge_url` was cleared after the test, so it is not polling a host that has
  stopped serving. Setting it again is the two commands above.
- The mock bridge is a testing aid, not part of the product. It is committed because the protocol
  is easier to trust with a reference implementation beside it, and because the next person needs
  something to point the device at before the companion exists.

---

## 2026-10-14 — Session 22: work mode on the screen, verified field by field

**Milestone:** work mode is visible and button-driven. A task, its progress, and a question with
options now appear on the panel, and the buttons move and confirm the selection.

### What was built

A third screen, `work`, whose objects the console can read: `work_task`, `work_state`,
`work_bar`, `work_percent`, `work_question`, `work_option0` to `work_option3`, and `work_hint`.
It is reached by holding DOWN, left by holding OK, and **a status document takes the screen over
from the home screen but not from the setup screen** — someone halfway through provisioning
should not be thrown out of it by a background update.

Three decisions are visible in the layout:

- **Unknown progress is drawn as absent, not as an empty bar.** A bridge that does not report
  progress would otherwise make every task look stalled at nothing; the bar is hidden and the
  number reads `--`.
- **The selected option is marked and coloured**, which is also how the console can tell which
  one is chosen.
- **The view lives in the application, not in the console.** The console and the button task are
  both thin drivers of one copy of the state, so a document fed over the console updates exactly
  what a button press would, and the two can never disagree.

### Verified on the device

```text
work feed {"task":"deploy","state":"waiting","question":"Which branch?",
           "option0":"main","option1":"develop","selected":0}
work: state=waiting progress=-1% task="deploy"
ui screen     -> screen work  240x320  children=13
ui get work_percent -> text: --            <- unknown, not a stalled zero
ui get work_option0 -> text: > main
work key down
ui get work_option1 -> text: > develop     <- the marker moved on the panel
work key ok   -> answer -> develop
```

A document that cannot be parsed was also checked on the device: feeding a mangled one reports
that the previous view is kept, and `ui get work_task` still reads the earlier task. That is the
"refuse rather than blank the screen" rule holding on hardware.

### Two findings

**A naming inconsistency of mine.** The option objects were named `option0` while the rest of the
screen used a `work_` prefix, so the same path could mean different objects depending on which
screen was in front. Every object on the screen now carries the prefix: console names are unique
across screens, and `ui get work_task` means one thing.

**Non-ASCII still cannot be typed into the console.** A status document containing a Chinese task
name was rejected as unparseable: the console's line handling mangles non-ASCII input, which is
the write-side counterpart of the reading problem recorded earlier. The Chinese *interface* text
on this screen renders correctly, because it is a source literal; it is documents *typed* at the
console that cannot carry Chinese. Test documents should be ASCII, and Chinese payloads arrive
over the network or from speech, not from the console.

---

## 2026-10-14 — Session 21: work mode's document and button rules, verified on the device

**Milestone:** the device half of work mode exists. The bridge is not written yet, so what is
verified here is the protocol and the interaction rules, not a live session.

### What was built

`passport_work` parses a status document and turns button presses into one of a few answers.

**The document is deliberately flat:**

```json
{"task":"indexing","progress":42,"state":"running",
 "question":"Which branch?","option0":"main","option1":"develop","selected":0}
```

Options are numbered fields rather than a JSON array. That is a contract with the bridge, not
an oversight: the device's reader is a few hundred bytes instead of a parser, and numbered
fields need no delimiter escaping, so a branch name containing a comma or a quote cannot
corrupt the list. Four options are read, which is what the panel can show with a highlighted
row.

Four decisions are encoded rather than left to the caller:

- **A missing field is not a zero.** `progress` absent reads as -1 and shows as unknown, where
  zero would claim the task is stalled at nothing. This is why the integer reader reports
  whether the key was present.
- **An out-of-range number is clamped, not rejected**: a progress bar that refuses 250 shows
  nothing at all, which is worse than showing full.
- **The option list ends at the first gap**, so a stray `option3` cannot appear on its own.
- **A document that names neither a task nor a question is refused**, leaving the previous
  view untouched rather than blanking a screen that was showing something useful.

And the button rules: the selection wraps, confirm does nothing unless a question is waiting
and has options, and **one question takes one answer** — a second press is refused.

### Verified on the device

```text
work feed {"task":"indexing repository","progress":42,"state":"running"}
work: state=running progress=42% task="indexing repository"

work feed {"task":"deploy","state":"waiting","question":"Which branch?",
           "option0":"main","option1":"develop","option2":"release","selected":0}
work: state=waiting progress=-1% task="deploy"
work: question="Which branch?"
  > option0: main
    option1: develop
    option2: release

work key down        -> selection moved
  > option1: develop
work key ok          -> answer -> develop
work key ok          -> nothing to do in state waiting      <- one answer per question
```

### A device-only bug that the host tests could not have caught

The first attempt showed `task="indexing"` and `progress=-1%`: the document had been truncated
at its **first space**. The console splits arguments on spaces, and `work feed` was passing only
its first argument to the parser, so the task lost its tail and every later field was discarded.
That looked exactly like a parser that ignores `progress` and `state`.

The host tests passed throughout, because they hand the document over as a single string. Only
the device exercised the console's own splitting. `work feed` now joins its arguments, the same
way `llm ask` already did for prompts.

The lesson is worth keeping: a passing host test says the logic is right, not that the way the
device receives the input is.

### Status

Nine host tests pass, including the new work test; firmware builds; the transcript above is
from the device, and the device is running normally after the previous session's reboot loop.

---

## 2026-10-14 — Session 20: the boot loop I caused, its fix, and what is verified now

**Status:** the device is working again. One of last session's two pending checks passes; the
other does not, for a measured reason.

### The boot loop, and that it was mine

The owner reported the board restarting continuously with the buttons doing nothing. The
console showed a repeating panic immediately after the audio warm-up line:

```text
Guru Meditation Error: Core 0 panic'ed (Stack protection fault).
Stack bounds: 0x3fcaa28c - 0x3fcab280      <- about 4 KB
```

The stack bounds are the main task's. The previous session's change called `qr_reserve()` from
`passport_app_start()`, which runs on the main task — and reserving the buffer encodes a symbol,
which needs about six kilobytes. The device therefore panicked before the interface came up and
restarted, forever, which is why no button did anything.

Two lessons, both recorded where they will be read:

- **A stack-size requirement belongs in the interface, not in the commit that discovers it.**
  `passport_qr.h` now states the encoder's roughly six kilobytes of stack and warns that a small
  caller panics in a way that looks like a reboot loop.
- **A reboot loop is what a stack fault looks like from outside.** The earlier symptom — the
  host reporting "a device attached to the system is not functioning" — was this loop resetting
  the USB console, not a separate fault. I had read it as a hardware problem.

### The fix

The reservation moved to the application worker, which was sized from 4608 to **12288 bytes**
for exactly this reason, and the call was removed from the main task.

### Verified on the device

```text
boot          no panic; the device is running
uptime        21,793 ms then 30,309 ms, so it is not restarting
```

And the first of the two checks from last session now passes: with the access point **down**,
`prov status` already reports `ssid Passport-91E1`, and `prov qr` reports a **40-byte payload**.
That is the on-demand identity fix working — last session the same command reported 18 bytes,
which was the trap that prompted it.

### Still not verified, with the measurement

`font shot qr dark` reports `could not allocate 24642 B for the snapshot`. Reserving the pixel
buffer early did not help, and the reason is now clear: once the setup screen and its code
buffer are up, there is simply not a 24 KB contiguous block left to allocate — the free total
is around 31 KB, but fragmented. The next attempt should use a narrower snapshot format (1 bit
per pixel is 1,541 bytes for this object, and A8 is 12,321) rather than the 16-bit one.

---

## 2026-10-14 — Session 19: the spoken-turn rules, settled before any provider is chosen

**Status:** host-tested and building; **no device verification**, because the device's USB
console is not functioning (below). Nothing here claims to have run on hardware.

### Why this and not something on the device

The console reported `A device attached to the system is not functioning` when written to. The
port opens, no stuck helper holds it, and the same device previously recovered only after a
physical power cycle. That is not something to fix from here, so this session took the part of
the voice milestone that needs no radio, no provider, and no microphone: **the rules of a
spoken turn**.

Those rules are what decide whether a voice interface feels broken, and they are the hardest
part to test once they are tangled up with an audio driver.

### What was built

`passport_voice` is a pure state machine over idle, listening, transcribing, thinking,
speaking, and failed, driven by button and stage events and answering with one action:
open the microphone, transcribe, ask the model, speak, report a failure, return to idle.

Four decisions are encoded rather than left to whoever wires it up:

- **The microphone is never open while the device is speaking.** This board cannot do acoustic
  echo cancellation, so a microphone left open during playback records the device's own voice.
  The machine closes it on leaving listening *and* again before speaking, and the host test
  walks **every event through every state** and asserts that the microphone is only ever open
  in the listening state. A future edit that opens it to "listen for barge-in" now fails a test
  rather than producing a device that argues with itself.
- **A press while an answer is being spoken is refused, not queued.** Cutting in on a
  half-duplex device has to mean cancelling the answer first, and that decision belongs to the
  caller; silently starting a turn underneath the playback is how a device ends up recording
  its own sentence.
- **A failure at any stage is recoverable without a reboot**: a reset clears it, and so does
  pressing the button again.
- **A silent room is not a failure.** The caller reports an empty transcript with a reset, so
  saying nothing returns quietly to idle instead of showing an error.

The provider calls stay outside the machine: it returns an action, and the caller performs it.

### Verified

```text
host tests:  8 of 8 PASS (the new voice test is one of them)
build:       PASS, ESP-IDF 5.5.3, image 0x224090, 73% of the partition free
device:      NOT RUN
```

### Still pending from the previous session

1. `prov qr` before `prov start` should report a 40-byte payload.
2. `font shot qr dark` should allocate and print the code.

Both need the device's console to work again, which needs a power cycle.

---

## 2026-10-14 — Session 18: reserved early, derived on demand — built and flashed, not yet device-verified

**Status:** build PASS, flashed to the device. **Device verification did not happen**, for a
host-side reason recorded below. This entry therefore claims nothing about behaviour.

### Two changes

**The access point credentials are derived on demand.** They come from the MAC, so they are a
function of the device rather than of whether the radio has been started. The accessors used
to return empty strings until `passport_provision_start()` had run, and that trap was observed
last session: `prov qr` reported an 18-byte payload — `WIFI:T:WPA;S:;P:;;` — because the code
was asked for before the access point came up. Nothing would have told a caller that the
answer was incomplete.

**The code's pixel buffer is reserved at start-up** instead of when the setup screen is first
built. The measurement that motivated it: with the setup screen up, free heap was 31,084 bytes
but the largest contiguous block was only 22,528 bytes, and stopping the access point raised
free heap to 49,136 bytes without changing the largest block at all. The pixel buffer, allocated
late, had split the block, so a 24,642-byte snapshot could not be allocated while the code it
wanted to photograph was using exactly that much. The verification tool was starved by the
thing it was verifying. Reserving before the heap grows also matches the rule this project
follows elsewhere: allocate a hot buffer once.

### Why there is no device verification

The host's serial port was left locked by two of my own commands that hung during the
previous session's attempts. I freed it once by killing a stale process, then lost it again —
and in one attempt I killed processes too broadly and took my own shell with them. Later
opens blocked while the device console was streaming.

The lesson is about my own tooling rather than the firmware: a serial helper that can block
indefinitely will eventually hold the port, and the next command then fails in a way that
looks like a device fault. Every serial helper in this project's notes should read with a hard
deadline and never spin without one.

### What the next attempt must check

1. `prov qr` before `prov start` reports a 40-byte payload, which is the on-demand identity fix.
2. `font shot qr dark` allocates and prints the code, which is the reservation fix.
3. Power-cycle the device first: its console may still be streaming.

---

## 2026-10-14 — Session 17: the code is on the panel, and it matches the reference

**Milestone:** the setup screen carries a QR code that joins the hotspot, so setup needs no
typing at all.

### What was built

`qr_render()` builds `WIFI:T:WPA;S:<ssid>;P:<password>;;` from the live access point
credentials, encodes it, and paints it into an RGB565 buffer taken **from the system heap** —
a canvas of this size (24,642 bytes) would have to fit LVGL's own pool, which is an order of
magnitude too small. The image sits in a container with a white background, which is both the
quiet zone a reader needs and a background a snapshot can compare against. The screen was
rearranged to give the code 111 pixels of width, with the credentials and the address below it.

The console gained `prov qr`, which prints the live matrix, and `font shot <path> dark`, for
dark-on-light content.

### Verified on the device

```text
ui key up long   ->  the app logs "code generated: version 3, 29 modules, scale 3,
                     111 pixels, payload 40 bytes" (in Chinese, as the firmware logs)
ui screen        ->  screen setup
ui names         ->  qr (obj), qr/qr_code (widget), plus the Chinese labels
prov qr          ->  40-byte payload, version 3, 29 modules, mask 2
```

Then the decisive comparison: the 29x29 matrix read back from the device, built from the real
access point name and password, was compared against the Python reference for the same
payload — **0 of 841 modules differ**.

### A crash, found by pressing the button

Holding UP built the screen and the device restarted. The code is encoded inside the key
handler, which runs on the input task, and that task had a 4096-byte stack; the encoder needs
about six kilobytes between its working copy and the builder's module map. The task is now
sized for what a key press can do, with the reason recorded beside the setting.

The symptom was misleading in a way worth remembering: the access point appeared to have
switched itself off, because the restart cleared the provisioning state. The first reading of
that evidence was "the button does nothing", not "the device rebooted".

### Not verified

The pixels on the panel. `font shot qr dark` could not allocate its own 24,642-byte snapshot
buffer while the code already occupied that much, so the rendering is unverified visually —
the matrix above proves the *content*, not that a camera would see it. Whether a phone can
scan it is the user check this milestone hands over, alongside the captive portal check from
the previous one.

---

## 2026-10-14 — Session 16: a QR encoder, verified against an independent implementation

**Milestone:** the encoder exists and its symbols are proven correct module by module. It is
**not yet drawn on the panel** — that is the next step, and this entry does not claim it.

### What was built

`passport_qr` encodes byte-mode, level-L symbols in versions 1 to 4, which is everything this
device needs: the longest payload is a Wi-Fi join string, and version 4 holds 78 bytes.
`tools/make_qr_fixture.py` generates the test fixtures from the Python `qrcode` package, and
`tests/test_passport_qr.c` compares matrices module by module, reporting the first differing
coordinate when they disagree.

Verified: four payloads at eight masks each — **32 exact matrix comparisons**. Between them
they cover the mode and length fields, the pad codewords, the Reed-Solomon arithmetic, the
finder, separator, timing and alignment patterns, the zigzag placement, the masking, and the
format information.

### Two of my own mistakes, found on the way

**The fixtures were wrong before the encoder was.** `qrcode` picks the most compact encoding,
and "HELLO" is more compact in alphanumeric mode than in byte mode — so the reference symbols
carried *different codewords*, and the matrices disagreed for a reason that had nothing to do
with this code. Forcing byte mode on the references fixed it. That is recorded in the
generator, because a fixture that is not like-for-like sends the search into the wrong file.

**The test found a real null-pointer dereference.** `passport_qr_encode(text, NULL)` wrote
through the null pointer at the end, because the guard lived only in the internal builder.
The check now happens where the write happens.

### One property that is not verified, and why

The **mask the penalty rules select is not verified against an oracle.** The plan was to
compare against `segno`, which implements the specification's penalty. It cannot be used:
extracting the codewords from segno's own matrix in the standard order gives an extra zero
byte before the pad codewords —

```text
segno, extracted   : 40 54 84 54 C4 C4 F0 00 EC 11 ...
specification      : 40 54 84 54 C4 C4 F0 EC 11 EC 11 ...
```

— so segno encodes a different symbol and scores a different matrix. `qrcode`'s automatic
choice is not an oracle either: it scores masks with a neighbour-count heuristic rather than
the specification's rules.

So the symbol is verified correct **for every mask**, which means any selection still yields a
valid scannable code; the selection itself follows the specification's penalty as implemented
here, is self-consistent, and is recorded in the fixtures next to segno's choice for whoever
picks this up. The mask affects how easily a reader locks on, not what the code says.

A side effect worth noting: reading the reference matrix in the standard order and recovering
the expected codewords also confirmed this encoder's traversal order — that was never a
separate test, and it passed by construction.

---

## 2026-10-14 — Session 15: the setup screen, and the device comes up online by itself

**Milestone:** the hotspot credentials are visible on the device, which is the whole point
of provisioning without a cable. The gap this closes was real: last session the credentials
existed only in the serial console, so setting the device up still required the cable the
feature was meant to remove.

### What was built

A second screen, `setup`, with named objects the console can read: `setup_title`,
`setup_step`, `hotspot`, `password`, `address`, `setup_hint`, and `setup_back`. It is
reachable by holding UP, and OK returns home. **OK cannot start a conversation while the
setup screen is up** — a screen showing a password should not also be a screen that talks
to a model on a single press.

A device with no Wi-Fi configured now raises its own access point and shows this screen at
boot, so a factory-fresh unit explains itself.

**Boot auto-connect**: a configured device came up offline until someone asked it to join,
because the station only retried on a disconnect event. The application worker now joins
once at start — it is a worker, so blocking there is correct — and the device reaches the
network unattended: verified connected at 192.168.3.75 after a reboot with no console
command.

The console's `font shot` now takes any object path and resolves an inactive screen by
name, so a second screen is inspectable rather than invisible.

### Verified on the device

```text
ui key up long   ->  active screen becomes setup (12 children)
ui names         ->  setup_title (the Chinese title, verbatim in the Chinese log),
                     setup_step, hotspot, password, address, setup_hint, setup_back
                     all present with their values
ui get setup/hotspot   text: Passport-91E1
ui get setup/address   text: 192.168.4.1
ui key ok click  ->  back to the passport screen
after a reboot   ->  wifi status: connected at 192.168.3.75 with no command issued
while home is active, "ui get setup/hotspot" still reads the setup screen
```

The Chinese on this screen renders because the generated font covers it — the labels above
were read back verbatim from the display.

### An open limitation, reported rather than papered over

`font shot` renders **container** objects (the conversation panel still snapshots exactly as
before) but currently fails for **label** objects with "the snapshot could not be rendered".
The reason is not yet established, and it is not a regression: the case that worked before
still works.

The consequence is a limit on what this round can claim. The setup screen's **content** is
verified textually — every label and value was read back from the live objects — but its
**pixels** were not verified visually, and this log does not claim otherwise. A whole screen
is also 153,600 bytes at RGB565, which will not fit the largest free block, so snapshots
have to target a child object.

---

## 2026-10-14 — Session 14: the device configures itself over its own access point

**Milestone:** first-time setup no longer needs a serial cable. The device raises a
protected access point, serves a setup page over it, and saves what it is given.

### What was built

`passport_portal` holds the request logic as pure C with host tests: route selection
(including the probe paths Android, Apple, Windows, and Firefox request to decide whether a
network needs a login page), `application/x-www-form-urlencoded` decoding with `%XX` and
`+`, and HTML escaping — the last because values arriving from the network end up inside a
page.

The form-to-configuration mapping lives in that pure module too, and the host test
**cross-checks it against the configuration schema**: every field the portal accepts must
name a field the schema really has. A typo there would otherwise be discovered by a user
whose setting silently did not save.

`passport_provision` is the radio and server side: an APSTA access point whose name and
password derive from the MAC (stable across reboots, unique per device, shown on screen), a
WPA2 password rather than an open network, an `esp_http_server` serving `/`, `/save`,
`/api/status` and `/api/config`, and a catch-all that redirects every probe path to the
setup page. Secrets are never rendered as field values: the page states whether one is
stored and submits nothing unless a replacement is typed. A blank field means "leave
unchanged", uniformly, so a careless save cannot destroy a working key.

The console gained `prov status|start|stop|page [route]|form <k=v>`, which is how every
claim above was checked without a browser.

### Two hardware findings

**Starting the access point dropped the station.** The two interfaces share one radio, and
the access point was forcing channel 6 while the network was on channel 1. It now follows
the station's channel and the association survives — verified by reading the station
address before and after `prov start`.

**The AP's SSID field is a fixed 32-byte array with no terminator.** The compiler's
format-truncation warning caught the same class of bug the station path had; both now check
their lengths explicitly and copy.

A bounded reconnect was added at the same time: a link that drops is retried up to five
times, and a disconnect the owner asked for is not retried at all.

### Verified on the device

```text
prov status        down, then up: Passport-91E1, password derived from the MAC, 192.168.4.1
wifi status        connected at 192.168.3.75 before and after prov start
prov page status   {"wifi":"connected","rssi":-41,...,"largest_block":57344}
prov page /generate_204   resolves to the redirect route
prov form pet_name=Whale&volume=55   applied and saved; the config then reads volume 55
prov form wifi_password=&pet_name=   "nothing to apply"; the stored password is intact
prov stop          ESP_OK
```

### Not verified, and why

A real HTTP request from a client over the access point. The device's own client cannot
reach 192.168.4.1 — "Host is unreachable" — because outgoing traffic uses the station
interface, so the request never loops back to the access point. That is a routing property
rather than a portal defect, and proving the browser path means joining `Passport-XXXX`
from a phone. **That is the user check this milestone hands over**: connect to the access
point and the captive portal should appear by itself.

---

## 2026-10-14 — Session 13: the font is generated from a real inventory, and the failure clears

**Milestone:** the coverage failure reported last session is fixed, and it is fixed
against the instrument that found it.

### What was built

The character inventory is **derived, not copied**: every valid character in the
GB2312/CP936 lead and trail byte ranges `0xA1A1`–`0xF7FE`, plus printable ASCII —
8,273 code points, committed as `assets/fonts/cjk-inventory.txt`. Deriving it means its
membership is verifiable and its reproduction needs no third-party list.

`tools/make_cjk_font.mjs` turns that inventory into an LVGL font with a pinned
`lv_font_conv` 1.5.3, from `SourceHanSansSC-Normal.otf` — the SIL Open Font License file
that already ships inside LVGL. That choice is deliberate: a bitmap subset of an OFL font
may be redistributed, while most system fonts may not, and a repository shipping such a
subset has a licensing problem. The generated file, its inventory, and the command are all
under version control, and `assets/README.md` records the source, license, size, and the
regeneration steps.

LVGL's built-in Source Han Sans sample is now switched **off** rather than left in place;
it was the thing that measured short.

### Verified on the device

The same reply that previously scored 8 missing glyphs, checked after the change:

```text
font check: 22 code points on screen; 0 have no real glyph
  text   : the same Chinese sentence as last session
  missing: (none)
```

The ASCII snapshot changed character too. Where the panel previously showed hollow
placeholder boxes, the shapes are now dense, and the closing question mark is visible.
So the fix is confirmed by the same instrument that reported the defect, on the same
input.

### Cost

The generated font adds about 0.5 MB of Flash and takes the build from a few seconds to
roughly three minutes, because the generated C file is 3.5 MB. The application image is
now 0x21a7d0 with 73% of the partition free. The 699 code points in the inventory that the
source font does not contain are skipped by the converter and are reported by `font check`
if they ever reach the display.

---

## 2026-10-14 — Session 12: the font is wired in and measured — the built-in subset is not enough

**Milestone:** the display renders Chinese through a font the application selects, and
the firmware now carries an instrument that measures glyph coverage on the device. The
measurement says the font that ships with LVGL cannot carry this product's conversation.

### What was built

`passport_fonts` selects Source Han Sans SC 16 for conversation text, with Montserrat 14
attached as a fallback in the pattern the repository's font guide prescribes, and it
exposes the guide's own glyph check. `passport_utf8` handles code points and, more to the
point, truncation on a character boundary: the reply buffer was cut by byte count, which
can split a Chinese character and leave an invalid tail that renders as nothing at all.

The console gained `font probe` (a fixed Chinese sample), `font check` (the text actually
on the display), and `font shot` (an ASCII rendering of the conversation panel). The last
one needed two facts discovered the hard way: LVGL allocates a snapshot from its own 24 KB
pool, so the panel image is taken with a buffer from the system heap instead; and the
panel's own colours must be excluded from the ink test, or the accent bar and the rounded
corners read as text.

The interface text moved to Chinese, which the font work finally allows.

### The measurement, and what it corrects

`font probe` on the sample the repository's guide suggests **passes**: only the
deliberately included emoji is missing. That is a trap. Those sample characters are the
ones LVGL's own font was built around.

`font check` on the model's real answer says something different. One reply contained 22
code points, of which **8 had no real glyph**:

```text
missing: U+563F U+5174 U+89C1 U+FF01 U+4E48 U+5E2E U+5417 U+FF1F
```

Those are ordinary words, and — worse for a chat interface — the full-width exclamation
and question marks. The ASCII snapshot of the panel shows the consequence directly:
hollow placeholder boxes where those characters should be. The verbatim reply and the
missing list are recorded in the Chinese version of this log.

So the built-in subset is a curated sample for LVGL's own tests, not a general Chinese
font, and the conversation screen is not yet honest in Chinese. The instrument that proved
it is now part of the firmware, and the fix is the guide's Option B: generate an
application subset from the OFL Source Han Sans SC file that already ships with LVGL, from
a real character inventory, with both the inventory and the conversion command under
version control.

### Honest status

Coverage infrastructure PASS. Visual rendering confirmed by snapshot for the characters
that do have glyphs. **The font asset itself FAILS for real replies**, which is a
deliberately reported failure rather than a passed probe.

---

## 2026-10-14 — Session 11: the button asks, the screen answers

**Milestone:** pressing OK now holds a conversation with the model, and the answer is
readable from the serial console.

### What was built

`passport_chat` is a worker task with a 12288-byte stack — the size a TLS handshake
needs, established the hard way when the console task panicked at 4096. It allocates its
response buffers once for the lifetime of the device rather than per turn, because
per-turn allocation is what fragments a heap this small. Pressing OK queues a prompt and
returns immediately; the worker takes the LVGL lock, shows the thinking expression,
performs the request, decodes the answer, and shows it with the speaking expression.
Failures the worker can see before touching the network — no model configured, no
association — are reported on screen instead of hiding behind a fifteen-second DNS
timeout.

### Verified on the device

```text
ui names             chat/bubble   label   "Hi. Press OK."
ui get chat/bubble   text: Hi. Press OK.
ui key ok click
ui get chat/bubble   text: Hello there!     <- the model's answer, on the screen
ui get mood          speaking
```

One button press produced a TLS request to the model, decoded the reply, and put it on
the display, where the console reads back exactly what a person sees.

### A contract bug in my own console, found by needing it

`ui names` printed `obj_3/bubble` for the answer, and `ui get obj_3/bubble` refused the
same string. The listings synthesized handles for unnamed objects while the resolver
matched only user-set names, so the console printed paths it could not read — and the one
string needed to prove the model had replied was exactly such a path. The resolver now
accepts either form, and the conversation panel was named `chat`, so the path reads
`chat/bubble`. The documented contract was updated with the reason, not just the rule.

### Prerequisite this milestone exposes

The display font still has no CJK glyphs, so a Chinese answer would render as blank boxes,
and the button therefore asks in English for now. This is recorded as a prerequisite for
the Chinese-first product rather than as a detail: UTF-8 in the reply and a successful
build do not prove the glyphs exist, which the repository's font checklist states
outright. Until the pixel CJK font lands, the conversation screen is honest only in
English — a real gap in the product, not in the plumbing.

---

## 2026-10-14 — Session 10: the decoder, and a real Chinese reply

**Milestone:** the response path is complete — the model's answer is extracted and
rendered, and it works in Chinese.

### What was built

`passport_json` decodes one string value out of a provider response, as pure C with a
host test. Two properties are deliberate:

- The key is matched **with its surrounding quotes**, so asking for `"content"` cannot
  match inside `"reasoning_content"`. This matters: DeepSeek's thinking mode returns the
  model's reasoning alongside the answer, and a naive substring search would feed
  reasoning into the pet's speech bubble.
- The decode is bounded and **truncation is reported together with the full length**,
  so a caller learns how much it lost. This is the repository's own recorded lesson — a
  fixed 4096-byte document meeting a 6971-byte response — turned into an API rather than
  a comment.

Escapes are handled, including `\uXXXX` to UTF-8. A code point outside the Basic
Multilingual Plane becomes `?` rather than being guessed at, and the host test pins
that decision.

`llm ask <text>` posts a real question and prints the reply. Its response and answer
buffers live on the heap, not the stack, because the console task's stack already has
to hold a TLS handshake.

### Verified on the device

```text
llm ask say "hi" in JSON
  1740 ms, 492 B response, largest free block with the session live: 64 KiB
  reply: a fenced JSON snippet          <- embedded quotes and newlines survived

llm ask reply in Chinese, one short sentence: who are you
  1859 ms, 518 B response
  reply: a correct Chinese sentence (the verbatim text is in the Chinese version of
         this log, so that this file stays English-only as the conventions require)
  CJK characters in the reply line: 19
```

### A measurement artifact I nearly recorded as a bug

The first Chinese capture showed `reply: ??????????????`. That was my own port reader
decoding bytes without setting an encoding — the firmware was fine. Reading the raw
bytes and decoding them as UTF-8 showed the correct text. Worth writing down: I nearly
logged a firmware defect that did not exist, and only checking the capture path
prevented it.

The practical TLS-live figure for a conversation turn is **64 KiB** of largest free
block — 68 KiB for a bare probe, minus the ask buffers — which is the budget the audio
pipeline will have to fit beside.

### Known limitation

Typing non-ASCII into the serial console is unreliable: one Chinese prompt was rejected
by the console's line reader, another was accepted. The product path for Chinese input
is speech, not typed console input, so this is a tooling limitation rather than a
product one — but it means Chinese prompts should be exercised through the voice path
rather than assumed to work over the console.

---

## 2026-10-14 — Session 9: the device talks to DeepSeek, and the TLS number arrives

**Milestone:** the provider path is proven end to end, and the measurement the
architecture document has been waiting for is in.

### Result

After associating with the network and issuing `llm probe`:

```text
llm probe: HTTP 200, 452 bytes, 1730 ms
  with the session live: free heap 80376 B, largest free block 69632 B (68 KiB)
  body: {"id":"...","object":"chat.completion",...,"model":"deepseek-flash",
         "choices":[{"index":0,"message":{"role":"assistant","content":"p"},
         "finish_reason":"length"}],...}
```

A real completion came back. That one result proves DNS resolution, the TLS handshake
against the certificate bundle, the HTTP POST, the bearer credential, the provider's
response, and the capped read path — 1,730 ms including the handshake, which is also
the first real number for the latency budget.

### The number that was missing

| Situation | Free heap | Largest free block |
| --- | --- | --- |
| Idle, no radio | 154,624 B | 112 KiB |
| Wi-Fi associated, no TLS | 163,580 B | 112 KiB |
| **TLS session live** | **80,376 B** | **68 KiB** |

The largest contiguous block falls from 112 KiB to **68 KiB** once a TLS session
exists. That is the figure this project has been calling its real constraint, and it
is now measured rather than estimated. It remains comfortably above what a chunked
PCM pipeline needs, so direct mode stays viable — but the margin is 68 KiB, not
112 KiB, and every future buffer has to be weighed against that number.

### A second crash found only by running it

`llm probe` first died with `Guru Meditation Error: Stack protection fault`, in task
`console_repl`. The console task had a 4096-byte stack, which cannot hold an mbedTLS
handshake; it is now 10240 bytes, with the reason recorded next to the setting so the
next person does not shrink it back. This is the second defect this milestone has
produced that was invisible from the source.

### Credentials

The Wi-Fi password and the API key were supplied by the owner and written only to the
device's NVS. They were not committed, not written into documentation, and not logged;
the console renders them only as set or empty, and the captured device output was
checked for both strings — neither appeared. The key did pass through the chat
transcript at the owner's choice, so **rotating it is the owner's call and costs one
console command**.

---

## 2026-10-14 — Session 8: the HTTPS transport, and a crash the device found

**Milestone:** the transport half of the provider path. The device can issue an HTTPS
request and report what came back, including the heap with the session live.

### What was built

`passport_http` performs one request with `esp_http_client`, reads the response into
a caller-supplied buffer with a hard cap, and reports truncation instead of hiding
it — the repository already has an entry about a fixed 4096-byte document meeting a
6971-byte response. It takes the heap and largest-free-block readings **while the TLS
session is established**, because that is the number the direct-mode budget depends
on; measuring before or after the handshake misses the cost it is meant to capture.

The certificate bundle is enabled, and the TLS record buffers are reduced from the
16 KB default to 4 KB so the measurement reflects the configuration actually planned
rather than the default one. Console commands: `net probe <url>` and `llm probe`. The
latter builds a minimal chat-completion request from the configuration and refuses
with instructions when the endpoint or model is unset.

### A crash, found by running it on the device

The first `net probe` on hardware asserted inside lwIP:

```text
assert failed: tcpip_send_msg_wait_sem /IDF/components/lwip/lwip/src/api/tcpip.c:454 (Invalid mbox)
```

The cause was mine: creating a socket requires the network stack, and only the Wi-Fi
command had ever initialized it. `passport_net_init()` now owns `esp_netif` and the
default event loop, the Wi-Fi module builds on it, and the HTTP client calls it
before touching a socket. A library function must not assume a global initializer ran
first.

Verified after the fix: `net probe https://api.deepseek.com/` returns
`ESP_ERR_HTTP_CONNECT` from a clean `getaddrinfo()` failure, the device keeps
running, and the following commands answer normally.

### A tooling mistake of mine

A flash appeared to fail while printing success lines. The cause was
`Select-Object -First N` on a native command's output: it short-circuits the pipeline
and can terminate the upstream process, so the exit code I read was my own filter's,
not the tool's. Read the exit code into a variable before filtering. The flash had
succeeded.

### Blocked on one input

Verifying the TLS handshake, the 401 response path, and the "largest free block with
a live TLS session" number requires the device to be associated with a network. That
needs the owner's SSID and password: they are not mine to invent, and the scan list
must not be committed. Everything up to that point is done and verified; the
association and the provider round trip are the next actions once credentials exist.

---

## 2026-10-14 — Session 7: the Wi-Fi station, and a truncation bug the compiler caught

**Milestone:** the network layer. The device can find and join a network using
credentials from the configuration.

### What was built

Two configuration fields, `wifi_ssid` and `wifi_password` (the password marked
secret). The schema version moved to 2 because the struct layout changed, which gave
the version guard a real workout (below).

`passport_wifi` brings up the station — netif, default event loop, station
interface, event handlers — then scans and connects. Credentials come from the
configuration store and never from source. Both operations block, so they belong in a
task, not in a button callback or the LVGL timer.

Console commands: `wifi status`, `wifi scan`, `wifi connect [ms]`,
`wifi disconnect`.

### The compiler caught a real bug

Writing the SSID and password into the Wi-Fi stack's fields with `snprintf` produced
a `-Werror=format-truncation` diagnostic: the configuration allows a 95-character
string, the stack allows 31 for an SSID and 63 for a password. The silent
consequence would have been **associating with a different network**, or truncating a
password and reporting a wrong-password failure that is really a firmware bug. The
connect path now checks both lengths and refuses with the exact limit instead of
truncating.

### A reporting bug of my own, found by reading the output

After the version mismatch the console printed `stored=yes`, which was untrue: the
live configuration was the defaults while the stored blob was the old version. The
dirty flag now means what the wording claims — *the live configuration is not what is
stored* — and it is set when nothing was found, when the version or size did not
match, and when NVS could not be opened. Verified by watching `stored=no (unsaved
changes)` turn into `stored=yes` across a `config save`.

### Verified on the device

```text
config list   after the version bump: the v1 blob (1260 B) is refused against the
              v2 struct (1452 B), the defaults are installed, reported as stored=no;
              config save then reports stored=yes
wifi status   idle, address (none), ssid (not configured), password (empty)
wifi scan     17 access points, RSSI from -30 to -85 dBm, channels 1 to 11
wifi connect  without an SSID: refused immediately, with the three commands to run
wifi connect  with a non-existent SSID: ESP_FAIL through the disconnect event,
              state becomes failed
```

The scan list is deliberately not reproduced here: it contains the surrounding
neighbourhood's network names, which are not this project's data to record. The
device's own MAC address is left out for the same reason.

---

## 2026-10-14 — Session 6: the configuration store

**Milestone:** the settings and credential layer that the voice, work-mode, and web
console milestones all depend on.

### What was built

`passport_config` is the schema and its accessors as pure C, with a host test: one
field table drives the console today and the web console later; defaults are applied
and idempotent; numeric values are clamped to their bounds; an over-long string is
refused rather than truncated; and **a field marked secret can never be rendered**.
`passport_cfg_format()` returns "(set)" or "(empty)" for it, so a credential cannot
leak through a log line, a transcript, or the console listing.

`passport_config_store` persists the whole struct as one NVS blob under one key and
treats a size or version mismatch as "use the defaults" rather than reinterpreting
old bytes as new fields. An NVS initialization failure never triggers an erase: the
device reports it and runs on defaults.

Startup in `main.c` now initializes NVS and the configuration, and the stored
preferences take effect — brightness is applied to the backlight and the stored
volume is used when the codec opens, replacing the compiled-in constants.

Console commands: `config list`, `config get`, `config set`, `config save`, and
`config reset`.

### Verified on the device

```text
config list           defaults; secrets shown as (empty) [secret];
                      llm_base_url https://api.deepseek.com
config set llm_api_key sk-hidden-42    ->  = (set) [secret]
config set llm_model deepseek-chat
config set volume 35
config save           ESP_OK
reboot
config list           llm_api_key (set), llm_model deepseek-chat, volume 35
config get llm_api_key ->  (set)   (a secret is never printed)
```

Persistence across a reboot works, and a stored credential is never rendered. The
secret rule is **asserted, not eyeballed**: after filtering out the console's echo
of the command I typed, `sk-hidden-42` appears nowhere in the device's own output.

---

## 2026-10-14 — Session 5: the audio path, and two findings from measuring it

**Milestone:** the microphone and speaker path is in the application and verified;
the audio half of the M1 exit criteria is done.

### What was built

`passport_audio_arbiter` is the ownership state machine for the audio path, as pure
logic with a host test: capture and playback cannot both hold the path, only the
current holder may release it, and the microphone is not permitted while playback
owns it. That is where the "microphone muted during playback" requirement now
lives — as enforced code rather than a convention a later feature could forget.

`passport_audio` is the codec side: 16 kHz 16-bit mono capture and playback in
bounded chunks, a peak-hold level measured on the same data (reusing the
host-tested PCM helpers), a 1 kHz tone generator for a speaker check, and a warm-up
path.

The console gained `audio status`, `audio tone`, `audio loopback`, and
`audio mute-check`.

### Two findings, both from measuring rather than assuming

**The codec's first samples are not audio.** The first capture after boot reported
rms 31,106 and peak 32,768 — a railed, full-scale "input" in a quiet room. A 100 ms
per-open guard reduced it to rms 7,627 / peak 9,087, still 76x the steady state of
rms ~100 / peak ~237. The application now warms the codec up once at start with a
500 ms discard, and the first user-facing capture reads rms 148 / peak 286. Without
this, every voice turn after boot would have looked like someone shouting — a
plausible cause of a future "the transcriber hears nothing useful" bug.

**The mute rule is provable on the device, not only in a test.** `audio mute-check`
takes the path for playback, attempts a capture, and reports whether it was refused.
The host test covers the same rule exhaustively; this is the on-hardware counterpart.

### Verified on the device

```text
audio status         owner free, microphone allowed
audio loopback 1000  captured 16000 samples rms=148 peak=286
                     playback done (owner now free)
audio mute-check     capture refused while playback held the path: yes
audio tone 300       ESP_OK
app                  free=217,844 B min=185,144 B largest=114,688 B
                     lvgl free=14,084 biggest=13,996 frag=1% max_used=9,264
```

---

## 2026-10-14 — Session 4: the application replaces the demo menu

**Milestone:** the application body has begun; its first screen is the pet home
screen.

### What was done

The upstream hardware-test menu is gone from the build. `main/main.c` is now the
Passport entry point: BSP initialization, the deep-sleep key-guard hand-off from the
previous session, an input queue whose consumer drives the application, the serial
console, and one call to start the application.

The first screen is designed for this product rather than inherited: a status strip
with the battery top-right, an accent rule, a conversation panel with an accent
edge, the pet sprite, a line naming the current expression, a ground line, and a
footer hint. The demo's cards, clouds, grass, and `ui_pixel` shell are **deleted**,
not recolored.

The pet (`passport_pet.c`) is a sprite widget driven by a 90 ms LVGL timer: a
two-frame breathing bob with an occasional blink while idle, a raised tail while
working, a spout while speaking, and a small vertical reaction for happy and
distressed. Animation swaps the image source on one widget, so the resident object
count never changes. Every mood is reachable with `UP`/`DOWN`, which is how a host
verifies the whole sprite set over the console.

Battery reads happen in the application's worker task, not in the LVGL timer: the
gauge is on I2C, and blocking I/O inside the rendering task is exactly what the
hardware guide warns against. The worker takes `bsp_lvgl_lock()` to update the label.

### Verified on the device

Boot log: two I2C devices found, display and LVGL up, the application started, the
console ready, and a readiness line reporting Display=1 Button=1 Audio=1 Battery=1
App=1. The asset line reports 11 frames at 64x64, a first frame of 12,288 B, and
135,168 B of flash — matching the predicted RGB565A8 cost, so the sprites are
genuinely in the image rather than merely compiled.

Screen read back over the console, which is the correspondence working as intended:

```text
passport            obj      (active screen)
title               label    "PASSPORT"
battery             label    "97%"
obj_3/bubble        label    "Hi. Press OK."
pet                 widget
mood                label    "idle"
hint                label    "OK: react    hold OK: standby"
```

Driven with injected keys: `ui key down click` twice took `mood` from "idle" to
"curious" to "listening", and `ui key ok click` produced "happy", each read back
with `ui get mood`.

### Measurements

| Reading | Baseline demo | This application |
| --- | --- | --- |
| Free heap | 142,600 B | **211,884 B** |
| Largest free block | 112 KiB | 112 KiB |
| LVGL pool high-water mark | 18,452 B | **9,380 B** |
| LVGL pool free | 4,008 B | **14,100 B** |
| Application image | 0x1a39c0 | **0xd0d10 (854 KB)** |

The redesigned UI uses about half the LVGL pool of the demo it replaced, and
removing the demo pages released the unused NimBLE and Wi-Fi stacks from the image,
which is why the binary shrank by roughly half while gaining 135 KB of sprites.

---

## 2026-10-14 — Session 3: device identified, toolchain gap, M1 started

**Milestone:** `M1` in progress.

### What was done

1. **Device identified.** After a cable and USB-port change the Passport
   enumerates as an Espressif USB Serial/JTAG device, `USB\VID_303A&PID_1001`,
   with its console on `COM47`. That matches the documented board mapping: a
   native USB Serial/JTAG console and no UART bridge. The WCH CH342 dual-UART
   bridge on `COM11`/`COM13` belongs to some other device and is not part of this
   board.
2. **Toolchain gap found, and closed.** The machine had only ESP-IDF v6.1
   (`C:\esp\v6.1\esp-idf`, the single entry in the EIM registry, with only a v6.1
   offline archive). The repository pins `>=5.5.3,<5.6.0`, so v6.1 cannot build
   this firmware. ESP-IDF v5.5.3 was then installed **side by side** through EIM
   into `C:\esp`, leaving v6.1 untouched and still the selected version, and the
   firmware was built with v5.5.3 to prove it.
3. **Branch created.** `feature/deepseek-passport`, branched from fork `main`,
   which is level with `upstream/main` — so the baseline is aligned and the design
   documents travel with the branch.
4. **First M1 increment landed and verified.** The pure measurement arithmetic
   plus its host test, wired into the static gate.

### Why the arithmetic came first

The firmware's ESP-IDF modules cannot be compiled until 5.5.3 exists, but the
measurement math can be, and that is exactly where off-by-one and overflow
mistakes hide. Writing the verifiable part first leaves a working checkpoint on
the branch instead of one large change that nothing can build.

### Scope correction for M1

Idle current **cannot** be measured by the firmware. The board has no
current-sense path, and the CW2017 gauge reports the battery, not the board's draw
from USB. M1 can only enter the always-connected idle state and hold it, so that a
person with a USB power meter can read the number. It therefore stays under
`Unverified` until someone takes it, and the `M1` exit criteria in
[roadmap.md](roadmap.md) are corrected accordingly.

### Also recorded

- The pet sprites currently sit beside their design document under `docs/assets/`,
  which the fork `main` scope allows. When the firmware build first consumes them
  they must move to the repository-root `assets/images/` tree with the generator
  under `tools/`, as the asset-placement rule requires, and the design document
  must link to the new location.
- The firmware will read its Wi-Fi credentials from Kconfig, so a real SSID and
  password live only in the gitignored `sdkconfig` and never in source.

### The product owner's hands-on test of the flashed demo

The owner exercised the flashed baseline application and reported that everything
works, with one observation: waking from deep sleep restarts the application.

That is the documented contract, not a defect. `esp_deep_sleep_start()` does not
return; the chip resets and the application boots fresh. The repository's own
reference note records the same behaviour on this board, including that the USB
Serial/JTAG console goes down for the duration of deep sleep and returns after the
wake, and that the boot log prints the wake cause — which is the documented way to
confirm a wake source on real hardware.

Two consequences worth keeping:

- **Light sleep is different.** It resumes in place and does not restart. A design
  that needs a seamless resume must use light sleep or the always-connected
  profile, which is the default the owner already chose (`Q-8`). The shipped
  default therefore will not restart on idle.
- **Deep sleep is why the console vanished** during the first flash attempt, and
  it is why the baseline demo's Low Power page is not a debugging tool while a
  host is attached to the USB port.

Unverified: the observation was not instrumented. No serial capture of the wake
cause was taken at the time, so the wake source rests on documentation rather than
on a log from this device. A future capture of the wake-cause line would close
that gap.

### Serial console verified on the device

The console was flashed and exercised over `COM47`. Inspection and control both
work, and the exercise found one real defect in the console itself.

Working: `ver` reports build `4d05296`; `ui screen` reports `menu 240x320
children=39`; `ui names` prints every object as a path with its on-screen text,
including names derived from the visible labels (`display`, `button`, `audio`,
`battery`, `wi-fi`, `ble`, `low-power`); `ui key down click` followed by
`ui key ok click` enters a page; `ui tree 1` dumps the new screen; `ui key ok
long` returns to the menu. The loop closes, so a host can drive the device and
read back what is drawn.

Defect found and fixed: `lv_obj_get_name_resolved()` faults with a load access
fault on an **unnamed root screen** in this LVGL version. It appeared to work only
because the menu screen carries a user-set name. The console now reads
`lv_obj_get_name()` directly, builds a synthetic label for unnamed objects, and
resolves paths with its own walk instead of LVGL's name lookup. The consequence is
worth stating plainly: **only objects with a user-set name are addressable**;
unnamed objects still appear in the tree for inspection.

Also learned: the baseline demo's menu is driven by button events, not by widget
clicks, so `ui tap` cannot enter a page there — its cards register no click
handler. `ui tap` stays correct for a UI that does register handlers, which the M1
application will.

Measurements taken while the console was up, on the menu screen: free heap
142,600 B, minimum 138,544 B, **largest free block 112 KiB**; the LVGL pool free
4,008 B of 24 KB with a high-water mark of 18,452 B and 9% fragmentation; battery
97%; released keypad 2,964 mV. The LVGL pool figure is the first concrete sign
that the baseline demo already spends most of a 24 KB pool.

### Validation for this session

| Check | Result |
| --- | --- |
| Host tests | **PASS** — the new `test_passport_metrics_math` and the pre-existing pure tests compile with `-Wall -Wextra -Werror` and pass |
| Firmware build | **PASS** — ESP-IDF v5.5.3 from `C:\esp\v5.5.3\esp-idf`, target `esp32c3`, isolated build in `build/fw` with its own `sdkconfig`. Image `FoloToy-AI-Passport.bin`, 0x174ea0 bytes (about 1.5 MB) in an 8.3 MB application partition, 82% free. This builds the **baseline** application; the M1 application is not implemented yet, so this proves the toolchain and the repository, not M1 |
| Device tests | **PASS for the board sanity check** — the validated v5.5.3 build was flashed to `COM47` (`USB\VID_303A&PID_1001`) using segmented flashing, after explicit approval. The boot log reports application version `208f1ff`, chip revision v1.1, an I2C scan finding the ES8311 codec at `0x18` and the CW2017 gauge at `0x63`, the display and LVGL task starting, the ES8311 in slave mode, the 500 mAh battery profile matching, and a readiness line reporting all four subsystems up. This verifies the board and the toolchain end to end. It does **not** verify M1, which is not implemented yet. The firmware's Chinese log text renders as mojibake in `idf.py monitor` on this host, which is a terminal decoding artifact and not a firmware fault |
| Unverified | Idle current (an external instrument check); every firmware measurement; the TLS probe's behaviour on this network |

---

## 2026-10-13 (later) — Session 2: product owner answers and the pet design

**Milestone:** `M0` complete.

### Answers received

Six of the ten questions were answered. The answers are recorded as decisions in
[requirements.md](requirements.md#61-answered); what follows is their effect on
the design.

| ID | Answer | Effect |
| --- | --- | --- |
| `Q-1` | An original Q-version whale in the brand blue; the project may draw it | The pet design and its generator were produced (below) |
| `Q-4` | Provider credentials live on the device; plain text is accepted | Device-direct becomes a supported deployment rather than a fallback, which puts TLS on the critical path. `M1` must now measure the largest free block with a live TLS session before any provider code is written |
| `Q-5` | Partial transcription is required on screen | A streaming transcription service becomes the baseline and batch transcription the cost-saving alternative |
| `Q-6` | Over-the-air update is deferred | The partition table stays as it is: one factory application, no second slot |
| `Q-8` | The always-connected profile is the default | Deep sleep becomes opt-in; idle current is now a number the project owes the user, measured in `M1` |
| `Q-10` | Long in-pocket recording is deferred | The storage design is unchanged |

### Decisions

| ID | Decision | Reason |
| --- | --- | --- |
| D-13 | The pet is an original Q-version whale drawn by this project, not a copy of the DeepSeek logo | The owner asked for the brand blue and a Q-style character while allowing the project to draw it; keeping the artwork original avoids a trademark problem and gives the project a character it owns |
| D-14 | Device-direct provider calls are supported, with provider credentials stored on the device in plain text | Product owner answer to `Q-4`. The consequence is accepted explicitly: a lost device is a lost key |
| D-15 | A streaming transcription back end is the default; batch transcription remains available | Product owner answer to `Q-5` |
| D-16 | Over-the-air update is deferred and the partition table is left alone | Product owner answer to `Q-6` |
| D-17 | The always-connected power profile is the default; the battery profile and deep sleep are deferred, and the upstream key-wake guard is preserved for them | Product owner answer to `Q-8` |
| D-18 | The pet is generated procedurally from a checked-in script rather than hand-drawn pixel by pixel | Redrawing a 32 × 32 character by hand for every expression is slow and unreviewable; a generator makes the geometry, palette, and frame list reviewable and reproducible |

### The pet design

A first-pass design exists, produced by a new generator.

- The character is a 32 × 32 logical whale facing right, written at 2× as 64 × 64
  sprites, with eleven opaque colors anchored on the brand blue `#4D6BFE`.
- Eleven frames cover idle (a two-frame breathing bob), blink, curious,
  listening, thinking, working, speaking, happy, sleeping, and distressed.
- Each frame is 12,288 B as RGB565A8; eleven frames total 132 KiB of flash, which
  is irrelevant against 8 MB. Frames share identical bounds, so animation swaps
  one image source and never changes the resident object count.
- The generator uses the Python standard library only, so it runs anywhere and
  produces byte-identical output.
- The design is awaiting product-owner sign-off, and redrawing is cheap: the
  geometry constants and the palette are the whole surface area.

Artwork and generator: [`art/`](art/README.md).

### Reference research

The design was checked against published references before it was drawn, and it
copies none of them.

| Finding | Source | Effect |
| --- | --- | --- |
| The official brand colour is `#4D6BFE` | DeepSeek's own organisation publishes a colour swatch whose only fill is that value | The body colour is exactly it, so the palette is verified rather than estimated |
| DeepSeek's mark is a whale, and its eye is lost at small sizes | Press coverage of the logo | The pet's eyes are deliberately oversized and high-contrast |
| No official pixel-art mascot or Q-version character exists | Reference search | The project duplicates nothing, and there was nothing to reuse |
| The logo whale faces left with its flukes raised at the upper right | A rendered copy of the official logo, inspected directly | See the orientation decision below |
| DeepSeek's own harness uses an animated whale tail as its "agent is working" indicator | The harness client sources | The pet's `working` frame raises the tail instead of inventing a new signal |
| The companion desktop pet's artwork is not reusable | That project's own notice: no commercial use, mandatory attribution, and its art is soft-shaded animation rather than pixel art | Confirms that original artwork is required; nothing from it was copied |

Orientation: the pet faces right rather than matching the logo's left-facing pose.
Matching the official pose would move the character toward the part most protected
by DeepSeek's brand rules, and facing right points the pet into the screen
content. It is a single constant to change if the owner prefers the logo pose.

The official silhouette was not traced or modified, no wordmark is used, and this
project is not affiliated with DeepSeek.

### Documents updated

- `requirements.md`: two new requirements (`R-B10`, `R-B11`), four modified
  requirements, one new assumption (`A-9`), and the question table split into
  answered and still open.
- `architecture.md`: the deployment-mode section now describes both modes as
  supported; a standby-default subsection; TLS moved onto the critical path in
  the memory budget; the security table states the accepted plain-text risk; the
  risk table gained the credential-loss row; and the pet section now carries the
  concrete design.
- `voice-backends.md`: the decision record reflects device-direct support, and the
  transcription selection changed to a streaming baseline.
- `roadmap.md`: `M0` marked complete; `M1` now includes a live-TLS memory
  measurement and an idle-current measurement; `M3` includes the direct-mode path;
  `M5` and `M7` reflect the deferred update and battery work.
- `art/`: new — the sprite generator, eleven frames, and a README recording the
  design, the palette, the reference research, and the licence position.

### Still open

`Q-2` (default voice), `Q-3` (out-of-home delivery), `Q-7` (writing a setup URL
to the NFC tag), and `Q-9` (whether the persona is user-editable). None blocks
`M1`.

### Next steps

1. Start `M1`: the measurement-only firmware. It now answers two questions that
   can change the design: the largest free block with a live TLS session, and the
   idle current with the network held open.
2. Audio loopback through the real codec path, confirming the mute-during-playback
   rule is enforceable in code.
3. Collect product-owner feedback on the pet art before `M6` depends on it.

### Validation run for this session

| Check | Result |
| --- | --- |
| Repository checks (`tools/check_repo.py`) | **PASS** — all text files scanned; every English/Chinese pair present and structurally aligned; links, secret patterns, and root-Markdown rules clean |
| Host tests | **PASS, unchanged** — no C source, Python source, or build configuration was touched, so the suite was not re-run |
| Firmware build | **NOT RUN** — no ESP-IDF environment is activated, and no firmware source changed |
| Device tests | **NOT RUN** — no board is connected |
| Unverified | The same items as Session 1, plus the pet design, which is a proposal awaiting the owner's sign-off |

---

## 2026-10-13 — Session 1: reconnaissance, research, and the design record

**Milestone:** `M0` in progress.

**First project commit:** `059c74a` — the design record and the fork readme.

### What was done

1. Read the repository's mandatory agent entry point and its routing table, and
   confirmed the fork workflow rules that constrain where fork-owned content may
   live.
2. Established the hardware facts from the single source of truth rather than
   from the chip datasheet, and read the board's own measured experience notes.
3. Researched three open questions in parallel: what the companion agent-side
   project actually exposes, which real-time voice architectures fit this chip,
   and how provisioning plus a local web console should be built.
4. Wrote the requirement record from the product owner's brief, the architecture,
   the provider survey, and this roadmap.
5. Created the fork readme pair.

### Hardware facts confirmed

Taken from the board's pin definition file and its hardware guide, not inferred.

| Fact | Value |
| --- | --- |
| MCU | ESP32-C3, single core, no PSRAM, 8 MB flash |
| Display | ST7789P3 240 × 320, SPI, one 9.6 KB partial draw buffer |
| Audio | ES8311, full-duplex I2S, 16 kHz 16-bit mono, microphone and speaker |
| Input | Three buttons on one ADC ladder, plus a hardware power button |
| Radio | 2.4 GHz Wi-Fi station and access point, Bluetooth 5 LE peripheral |
| Battery | 500 mAh with a fuel gauge, USB Type-C charging |
| Console | Native USB serial/JTAG on fixed pins; the UART default conflicts with the backlight |
| Partitions | One factory application; no OTA slots |

### Measured evidence found inside this repository

These are the numbers the design leans on, all from existing upstream notes on
the same board.

| Measurement | Value | Source |
| --- | --- | --- |
| Free system heap with an 80 KB LVGL pool and a rich UI | About 131 KB | LVGL pool budgeting reference note |
| Largest free block before a soft access point | About 13 KiB, rising to about 31 KiB after deferring audio initialization | SoftAP provisioning reference note |
| Working single-client HTTP server configuration | 3 sockets, backlog 2, 6 KB task stack, 2880-byte TCP windows | SoftAP provisioning reference note |
| A real JSON failure | A fixed 4096-byte document met a 6971-byte response | Network audio streaming reference note |
| MP3 decoder cost | 30–40 KB flash plus a measured 24 KiB compressed-input buffer | Audio compression and streaming reference notes |
| Opus decoder cost | 60–80 KB flash plus RAM | Audio compression reference note |
| Raw PCM at 16 kHz mono | 32000 B/s | Audio compression reference note |

### Research findings

**Companion agent-side project (reported, not verified against a running host).**
The companion plugin repository is a DeepSeek Harness plugin package rather than a
standalone application. It publishes read-only local JSON endpoints for work
status and incremental notification frames, and an endpoint accepting answers to
pending questions. Two constraints came out of that work and both matter:

- Its own documentation treats exposing the harness web server to the network as
  remote-code-execution equivalent, and its server binds to loopback only.
- The question-delivery endpoint is described inside that repository as a
  degraded path that does not fire reliably, so question capture must be verified
  first and must not be assumed.

Consequence: work-mode integration goes through a purpose-built bridge that is the
only LAN-facing surface.

**Voice architecture.** Two published facts collapsed the design space. Free heap
with the Wi-Fi station plus one TLS connection is about 210 KB on this chip with a
default configuration, and the Espressif audio front end — including every
echo-cancellation mode, which additionally needs PSRAM — does not support this
target. The one genuinely useful positive finding is that raw linear PCM output is
available from several synthesis providers, which removes the audio decoder from
the firmware entirely.

**Provisioning and the console.** The ESP-IDF provisioning manager's soft-access-
point transport speaks protobuf and requires a vendor phone SDK, so it cannot
satisfy "no separate application"; the provisioning server and captive portal must
be written directly. The pinned LVGL version already contains a QR widget that
stores its canvas at one bit per pixel, so no new dependency is needed for the
code. A partition-table change becomes necessary only if over-the-air update is
required.

### Decisions

| ID | Decision | Reason |
| --- | --- | --- |
| D-1 | The new fork readme is a repository-root readme pair; the upstream project overview document is left untouched | The fork workflow reserves the root readme for the fork owner, forbids editing the upstream overview in place, and renaming it would create a conflict on every upstream sync. Creating the root readme also wins GitHub's display priority, so it replaces the old readme as the visible project page |
| D-2 | Fork-private design documents live under `docs/assets/` | That is the documented fork-owned location for architecture notes, product design, and supplementary documents |
| D-3 | Provider orchestration lives in a companion bridge by default; device-direct calls are a configurable fallback only | Removes the largest RAM risk, keeps keys off a losable device, makes providers configurable without a firmware release, and normalizes audio format |
| D-4 | Work mode reaches the agent host only through the bridge, never directly | The agent host's local server is loopback-only for a documented security reason |
| D-5 | Half-duplex push-to-talk with the microphone muted during playback | Echo cancellation is unavailable on this hardware for two independent reasons; without the mute the device transcribes its own speaker |
| D-6 | Raw PCM end to end; no audio codec on the device | Measured decoder costs are large relative to a 210 KB heap, and providers offer PCM for free |
| D-7 | Pixel art is rendered from flash-resident image descriptors, not from many LVGL objects or per-frame canvases | Object and canvas allocations come out of a small LVGL pool; flash is effectively free |
| D-8 | The provisioning server is written directly rather than using the provisioning manager's soft-access-point transport | The manager's transport requires a vendor phone application and provides no portal page |
| D-9 | Over-the-air update is deferred until the product owner answers `Q-6` | It forces a partition-table change and a rollback policy, which is not a decision to make silently |
| D-10 | Wake-word activation is deferred rather than declared impossible | The small no-PSRAM wake-word models do support this chip; the reason to defer is cost and idle power, not capability |
| D-11 | The design documents are this set: requirements, architecture, voice back ends, roadmap, and this log | One authoritative home per question, no duplicated source of truth |
| D-12 | Every quantitative claim in the architecture document is labelled as a measurement, a published figure, or an estimate | Two of the first drafts contained estimates that read as facts; the board has enough measured experience available to avoid that |

### Verification status of the research

| Claim | Status |
| --- | --- |
| Companion plugin endpoints, payload shapes, and the loopback-only bind | Reported from the project's public sources; **not** exercised against a running host. Verify in `M4` |
| The question endpoint is a degraded path | Reported by that project's own documentation; treat as a hard risk |
| Free heap with Wi-Fi plus one TLS connection | Published figure for this chip, not measured on this board. `M1` measures it |
| Audio front end excludes this target, and echo cancellation additionally needs PSRAM | Published component and documentation facts |
| Wake-word models support this target | Published component fact |
| Raw PCM output available from synthesis providers | Documented by the providers |
| LVGL's in-tree QR widget at one bit per pixel | Reported from the pinned LVGL version's sources; verify against the resolved component after the first build |
| Time to first byte for synthesis providers | **Unverified.** Not published; must be measured |
| Provider prices | Indicative only, as of the survey date |

### Open questions carried forward

`Q-1` through `Q-10` in [requirements.md](requirements.md#6-open-questions) are
unanswered. The ones that block implementation are `Q-1` (the pet's design), `Q-4`
(whether device-direct calls are needed at all) and `Q-8` (the default power
profile and the acceptable standby delivery delay). `Q-2`, `Q-5`, `Q-6`, `Q-7`,
`Q-9`, and `Q-10` can be deferred to the milestone that needs them.

### Next steps

1. Collect the product owner's answers, starting with `Q-1`, `Q-4`, and `Q-8`.
2. Start `M1`: a measurement-only firmware that logs free heap, largest free
   block, and the LVGL pool at every phase boundary, plus an audio loopback check.
3. Verify the reported companion endpoints against a running host as soon as `M4`
   can begin, because a negative result changes the work-mode design.

### Repository state at the end of this session

```text
README.md / README.zh_CN.md                       new fork readme pair
docs/assets/README.md(+zh)                        fork asset and design index
docs/assets/deepseek-passport/requirements.md      requirement record
docs/assets/deepseek-passport/architecture.md      design and constraints
docs/assets/deepseek-passport/voice-backends.md    provider survey and decision
docs/assets/deepseek-passport/roadmap.md           milestones and validation plan
docs/assets/deepseek-passport/progress-log.md      this file
```

### Validation run for this session

| Check | Result |
| --- | --- |
| Repository checks (`tools/check_repo.py`) | **PASS** — all text files scanned; links, language pairs, secret patterns, and root-Markdown rules clean |
| Host tests, C logic suite | **PASS** — 8 of 8 test binaries compiled and passed with the host compiler |
| Host tests, Python modules | **PASS** — 6 of 6 modules passed, including the repository-checker and skill-installer suites |
| Required AI skills | **PASS** — all five required skills are installed at the discovery path `.agents/skills/` as relative links, and each `SKILL.md` is readable through that path |
| Firmware build | **NOT RUN** — no ESP-IDF environment is activated, and this change touches no firmware source |
| Device tests | **NOT RUN** — no board is connected |
| Unverified | The reported agent-side endpoints; the composition of the resolved LVGL component; every provider claim except those documented by the provider |

`tools/validate.sh` itself is not runnable on this host: it needs a POSIX shell,
`cc`, `python3`, and `actionlint`, and none of those names resolve here. Its steps
were run individually instead, with the host C compiler and the available Python.

One environment finding is recorded so the next session does not rediscover it:
under the session's initial confined file policy, both directory-symlink creation
and `tempfile.mkdtemp` directories failed with access denials, the latter blocking
any child directory. Both are confinement artifacts rather than repository
defects: with unrestricted file access in the same session, the skill installer
succeeded and every host test passed with no repository change in between.

No firmware source was changed. No flash, no device test, and no firmware build
was run.
