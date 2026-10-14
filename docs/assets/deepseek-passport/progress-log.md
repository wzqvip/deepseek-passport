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
