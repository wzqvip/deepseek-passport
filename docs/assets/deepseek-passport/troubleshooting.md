<p align="right">
  <a href="troubleshooting.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Troubleshooting

Symptoms the owner actually reports, and the reading of the device's own console that distinguishes
their causes. Every entry here came from a real observation on this board, not from imagination; the
date and the exact text are quoted so a later reader can tell whether the same thing is happening.

Use the serial console at 115200 baud (`tools/passport-console.ps1` drives it on Windows). It is the
only place that says *why*.

## "Holding OK does nothing"

Three unrelated causes, distinguished by two commands.

### 1. The device is not on the screen that talks

```text
ui screen
```

`screen work` or `screen settings` means the talk button is bound to that page's own meaning — on the
work screen it confirms an option. A long OK returns to the pet. If `ui screen` says `passport` and the
button still does nothing, keep reading.

### 2. The recording is discarded as silence

```text
voice status
```

```text
state=idle mic=closed turns=0 gate=3694 (twice the measured room) last="no turn yet"
```

A `gate` far above what a person can produce is the cause. Observed on 2026-10-31: the boot warm-up
measured a room peak of 1,847 and the threshold was twice that, 3,694, so ordinary speech was discarded.
Firmware from that date onward derives the threshold from the room's average instead of its peak and
clamps it to 300..1500; the same reading then reports something near 600. To pin it by hand:

```text
voice gate 600      # a fixed threshold
voice gate 0        # never treat anything as silence, for a bench test
```

The device reports the raw measurement beside the threshold as `room rms=… peak=…`, so the two numbers
that produced a wrong threshold are visible rather than inferred.

### 3. There is nowhere to send the turn, or the bridge rejects it

```text
config list
work client
```

Observed on 2026-10-31, on the device as it shipped from the previous session:

```text
  stt_base_url       (empty)      <- no direct transcription configured
  bridge_url         http://192.168.2.183:8080
work client: bridge=http://192.168.2.183:8080 polls=14 failures=14 last="fetch failed: ESP_ERR_HTTP_CONNECT"
```

With `stt_*` empty, every turn goes through the bridge. `ESP_ERR_HTTP_CONNECT` means the host or port is
not reachable — in this case the computer's address had changed, so the stored one pointed at nothing.
After correcting `bridge_url` to the computer's current address the same command reported:

```text
work client: ... last="fetch returned HTTP 401"
```

`401` is progress, not a new fault: the device reached the bridge, and the bridge rejected it for a
missing or wrong `bridge_token`. Firmware older than the authentication work does not send the token at
all, so an up-to-date bridge always answers it `401`; updating the firmware and setting the same token on
both sides fixes it.

## Reading the states

| Symptom | Means |
| --- | --- |
| `ESP_ERR_HTTP_CONNECT` | Wrong address, wrong port, bridge not running, or a different network |
| `HTTP 401` | Reachable, but the token is missing or different — check both sides |
| `HTTP 404` on `/work-status` | Reachable and authenticated; the companion has published no task |
| `gate` in the thousands | The threshold is above a real voice; see above |
| `last="capture failed: …"` | The microphone path failed, not the network |

## Pointing the device at this computer without guessing the address

`ESP_ERR_HTTP_CONNECT` is almost always "the address moved". Rather than reading `ipconfig` and copying a
number by hand:

```text
python tools/start_bridge.py --print-only
```

The launcher asks the operating system which address this machine would use to reach the LAN, refuses to
offer a loopback or link-local one to a device, starts the bridge bound to that address, and prints the two
console lines that point the device at it — with the token it generated, so both ends match without anyone
copying a secret. `--fake` runs it with a canned transcript and a tone, so the whole path can be checked
with no provider key. Without provider credentials it says so instead of starting something that will fail
on the first request.

Nothing is written to the device: run the printed lines yourself, or add `--apply` to have the console
driver run them. Flashing is not part of this tool.

## Changing the configuration without the web page

```text
config set bridge_url http://<computer>:8080
config set bridge_token <token>
config save
```

`config save` is a separate step on purpose: `config set` changes the running configuration only, so a
typo cannot become the stored one until it has been tried.
