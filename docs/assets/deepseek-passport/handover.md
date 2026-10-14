<p align="right">
  <a href="handover.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Handover

What this device is, how to flash it, how to use it, and — the part that matters most — what has been
verified and what has not.

Everything below was measured on the board in this repository's hands. Where something is a person's
check rather than a machine's, it says so instead of being rounded up.

## 1. What it is

A pocket-sized DeepSeek companion on an ESP32-C3. Its home screen is a pixel pet that breathes and
blinks. Hold a button, speak, and release: the speech is streamed to a companion bridge on the local
network, transcribed, sent to DeepSeek, and the answer comes back on the screen and out of the speaker.
A second screen mirrors a desktop work session — task, progress and a question with options — answered
with the same three buttons. It provisions itself over a hotspot with a QR code and a web console, and
it knows how to sleep.

The interface is this application's own: the upstream demo's menu, screens and visual shell were removed,
not renamed. Nothing under `main/` or `tests/` mentions `demo` any more.

## 2. Flashing

Requires ESP-IDF 5.5.3 activated.

```bash
idf.py -p <port> flash monitor      # build and flash from source
```

Or flash the verified merged image, which contains the bootloader, the partition table and the
application, and can be written to a blank board in one command:

```bash
esptool.py --chip esp32c3 write_flash 0x0 FoloToy-AI-Passport-full.bin
```

The repository's `tools/validate.sh --firmware` produces that file under `build/`. It is not committed:
it is a build artifact, not source.

**Never run a full-chip erase.** The Wi-Fi credentials and the API key live in NVS and a full erase
destroys them; `idf.py flash` in segmented form does not.

## 3. Using it

On the home screen:

| Gesture | What it does |
| --- | --- |
| **Hold OK, speak, release** | A spoken turn: record → transcribe → ask DeepSeek → answer on screen and spoken |
| Hold DOWN | Work mode |
| Hold UP | The setup screen: the join QR code, the hotspot name and password, the address |
| Double-press UP | Stand by: deep sleep, waking on any key |

On the work screen: UP and DOWN move the selection, OK confirms, holding OK goes home.

On the setup screen: OK goes home. While it is open the device runs its own hotspot
(`Passport-91E1`, WPA2) and serves a configuration page at `http://192.168.4.1/` — every setting the
serial console can change, without a cable. Joining the hotspot usually opens that page by itself,
because the captive-portal probes are answered. The password is ten characters from an alphabet with
`i`, `l`, `o`, `0` and `1` removed, because it is read off a small screen and typed by a person.

If a phone will not connect, ask the device first: `prov status` reads the access point's configuration
back **from the radio**, prints the driver's mode, and reports whether it agrees with what the screen
shows. A disagreement there means nothing typed will ever work, and it is a different problem from a
mistyped character. `prov events` then lists the last eight access-point events with uptimes, and every
client departure carries the Wi-Fi reason code in words — "4-way handshake timed out: the password did
not match" is a different problem from "the client left on its own: often no internet on this network",
and from no attempt at all. `prov clients` shows who is associated right now.

**The hotspot is not needed to configure the device.** The web console also answers on the station
interface, so any phone or computer on the same network can open `http://<the device's address>/` and use
every setting. This was verified from a host on the same LAN, and it is the first thing to try when the
hotspot will not cooperate.

The serial console at 115200 baud is the whole diagnostic surface: `help` lists the commands and the
[serial console document](serial-cli.md) explains the two contracts they keep. `ui audit` is the one
worth knowing about: it walks every screen and reports any label whose font cannot draw its text.

## 4. The bridge

The device is a thin terminal. Speech recognition, the model call and speech synthesis all go through a
companion process on the local network, which is what `bridge_url` points at. Its contract is
[the bridge protocol](bridge-protocol.md), and [`tools/mock_bridge.py`](../../../tools/mock_bridge.py)
is a stand-in that implements every endpoint so the device can be exercised without one:

```bash
python tools/mock_bridge.py --port 8080 --scenario running
# then, on the device:
#   config set bridge_url http://<this host>:8080
#   config save
```

`tools/check_device_qr.py` compares the join code the device prints against an independent
implementation, which is how the symbol was verified without a camera.

**What the bridge still needs to be real**: the provider calls behind `/voice/transcript` and `/speak`,
and the desktop session behind `/work-status`. The device side of all of it is written and verified
against the stand-in; a real deployment is a matter of implementing three HTTP endpoints on a computer,
with no firmware change.

## 5. Verified, with the evidence

| Area | What was verified | How |
| --- | --- | --- |
| Home screen | Its own design: pet, bubble, battery, hint, and nine distinct expressions | `ui names`, `ui get`, stepping every mood on the device |
| Chinese text | Every label is drawn with a font that has its glyphs | `ui audit` — written for this, and it failed before the fix |
| Voice turn | Hold → streamed audio → transcript → DeepSeek → spoken answer → turn closed | Byte-exact on the bridge (231,040 bytes = 7.2 s), transcript returned, 1,023-byte reply in 2,796 ms, `audio status` showing the microphone shut during playback |
| Audio paths | The microphone is never open while the speaker plays | The arbiter, its host test, and the device reporting `owner=playback, microphone=closed` mid-sentence |
| Spoken answers | Synthesised audio plays as it arrives, holding the path for the whole utterance | 16,000 samples played for a 1 s tone; path released afterwards |
| Work mode | A status document renders, the buttons select, the answer is posted | Against a real HTTP server on the local network, with the server's own log as the witness |
| Announcements | Chimes on a question or a finish, silence otherwise | Silent for ten seconds of a running task, chime when the state changed |
| Provisioning | Hotspot, captive portal, QR code, web console | The page rendered and saved a submission; the QR symbol matched the reference implementation cell for cell and the drawn pixels matched the encoder |
| Web console | Every setting editable from a phone, secrets never rendered | 19 inputs generated from the schema, and the page contains neither stored secret |
| Configuration | 19 fields, NVS-backed, secrets masked | Host tests plus the console |
| Standby | Idle screen-off, wake on a key without acting on it, deep sleep with an orderly shutdown | Timed idle-off, an injected press that only lit the screen, and a sleep that the panel and codec survived |
| Validation | 17 C tests, 6 Python contract tests, a clean firmware build, a verified merged image | `tools/validate.sh` step by step; the merged image checked against the partition table |

## 6. Not verified

Three of these need a person, and one needs a piece of equipment.

**Whether a phone can read the join code off the glass.** The symbol is verified twice — cell for cell
against an independent implementation, and the drawn pixels against the device's own encoder — so what
remains is optical: the contrast, the size on a 240-pixel-wide panel, and the camera.

**Waking from deep sleep by a key press.** The wake source is armed and the device has been observed
sleeping and coming back on a timer. Pressing a key is a hand on the device, which this session does not
have.

**Whether the chimes sound right.** They are measured — the waveform against the C library's sine, the
pitch by counting zero crossings — but nobody in this work has heard them.

**Standby current.** This needs an external USB power meter, as the measurement plan has said from the
start. No figure is invented here.

**The silence threshold against a real speaker.** The device measures its room at start-up and sets the
threshold from that measurement, twice the room's peak. The room was measured; a person speaking into
this microphone was not, so the margin is a documented heuristic rather than a tuned value.

**The real providers.** Everything provider-shaped was verified against the stand-in bridge. The device
has never spoken to a commercial transcription or synthesis service, and the deferred question of which
one is still open.

## 7. Honest debts

- **Memory is the binding constraint.** The largest free block at rest is about 34 KB, and a TLS
  session needs most of it. Adding a feature means finding memory for it first; this document exists
  partly because that lesson cost a reboot loop and a TLS regression on the way.
- **Half duplex is permanent on this board.** There is no acoustic echo cancellation, so the
  microphone is shut whenever the speaker is not. Interrupting an answer means cancelling it, and the
  device must not be asked to listen and speak at once.
- **The mask oracle gap.** Which of the eight masks the encoder *should* choose has never been checked
  against an oracle: `segno` disagreed with the specification about a padding byte. What is verified is
  that the drawn symbol is valid under the mask the device claims.
- **OTA is not implemented**, deferred by the owner's decision.
- **The work-mode screen has no bridge behind it yet** in the sense that a real desktop session does not
  feed it; the protocol is written and the device side is verified.

## 8. Where things are

| Path | What |
| --- | --- |
| [`requirements.md`](requirements.md) | What was asked for, decomposed, with the open questions |
| [`architecture.md`](architecture.md) | Hardware constraints, the memory budget as measured, the system shape |
| [`roadmap.md`](roadmap.md) | Milestones and exit criteria |
| [`voice-backends.md`](voice-backends.md) | The provider survey behind the voice decision |
| [`bridge-protocol.md`](bridge-protocol.md) | The device-to-companion contract |
| [`serial-cli.md`](serial-cli.md) | The console, and the two contracts it keeps |
| [`progress-log.md`](progress-log.md) | Every session: what was built, what was measured, what was wrong |
| [`main/`](../../../main) | The application, one module per concern |
| [`components/bsp/`](../../../components/bsp) | The board support package, upstream and reused |
| [`tools/`](../../../tools) | The gate, the fixture generator, the stand-in bridge, the QR checker, and the console driver |
| [`tools/passport-console.ps1`](../../../tools/passport-console.ps1) | Drives the device's serial console: waits for the prompt instead of sleeping, so a reading is repeatable rather than intermittent |

The progress log is the honest record. It contains the mistakes as well as the results — three wrong
diagnoses retracted, a reboot loop shipped and fixed, a protocol document corrected after it turned out
to describe a defect that never existed. That is deliberate: a record that only lists successes is not
evidence of anything.
