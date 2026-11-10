# ESP32-S3-BOX-3 Internet Radio

Internet radio for the [ESP32-S3-BOX-3](https://github.com/espressif/esp-box) built on ESPHome with custom external components. Streams internet radio stations with a Winamp 2 themed touch UI, real-time FFT spectrum visualizer, optional Bluetooth speaker output, and full Home Assistant integration.

## Features

- 10 pre-configured internet radio stations (electronic, trance, rock, alternative, pop, news)
- Winamp 2 inspired UI: beveled buttons, grooved title bar, signal bars, state indicators
- Real-time 16-band FFT spectrum visualizer (segmented gradient bars)
- Touch screen station selection & volume control
- Hardware buttons: Boot (cycle brightness), Mute switch (GPIO 1, hardware mic mute), Home (voice assistant)
- Auto-dim screen when stopped, restore on play
- Scrolling song title ticker with ID3/ICY metadata (Artist - Title)
- WiFi signal strength & bitrate display
- Bluetooth speaker output via I2S bridge to ESP32-WROOM-32D
- Home Assistant integration via ESPHome native API (media player, controls, sensors)
- Home Assistant voice assistant: Home button, "Ask Assistant" button or the `okay_nabu` wake word; replies and cues on the internal speaker, then the radio resumes
- Station, volume, brightness & output mode persisted across reboots
- OTA firmware updates

## Quick Start

### 1. Install ESPHome

```bash
pip install "esphome==2026.10.2"
```

Use the same ESPHome 2026.10.2 environment for configuration, compilation,
upload and tests (`esphome version`). The radio uses native ESP-IDF 6.0.1;
PlatformIO is only required for the separate BT bridge firmware.

### 2. Configure Secrets

```bash
cp secrets.yaml.sample secrets.yaml  # edit with your WiFi/API credentials
```

`secrets.yaml` should contain:
```yaml
wifi_ssid: "YourSSID"
wifi_password: "YourPassword"
esp32radio_api_key: "your-api-encryption-key"
esp32radio_ota_password: "your-ota-password"
```

### 3. Build & Flash

Development builds run in the existing **ESPHome Device Builder on Home Assistant
VM 100 (`192.168.1.10`) on Proxmox**, not on the development PC:

```bash
python3 tools/ha_build.py sync       # copy current YAML and components, no build
python3 tools/ha_build.py compile    # sync, then compile remotely; never flash
python3 tools/ha_build.py verify     # test matching HA-generated C++ and ELF/bin
python3 tools/ha_build.py status     # remote snapshot checksum and build log
```

`verify` refuses mismatched source checksums or skipped artifact tests. It runs
against the HA-generated code and linked firmware, without compiling locally.
The compile helper also reapplies requested SDK options if the initial native
configure discarded them before downloading GMF dependencies.

This requires SSH access through the `proxmox` alias and the HA VM's existing
QEMU guest agent. Defaults target container `app_5c53de3b_esphome`; use `--host`,
`--vm` and `--container` if the installation changes. The tool checks for
ESPHome 2026.10.2, uses one compile job with reduced priority, and refuses to
start with less than 1.5 GiB available in the HA VM. It does not restart HA,
change add-on versions or configure USB passthrough. Compilation never installs
firmware; the separate `upload` action requires explicit OTA authorization.

The separate `esp32radio-development.yaml` appears in Device Builder. It includes
a checksummed source snapshot under `/config/esphome/radio-development/`, using
**local external components** so both uncommitted YAML and C++/Python changes are
built without committing or pushing. Existing `esp32radio.yaml` remains unchanged
and continues to build GitHub `main`. Local secrets and build caches are never
copied: the development snapshot uses HA's existing `secrets.yaml`.

Each successful compile prints the OTA artifact path, size and SHA256. Build
output stays in `/config/esphome/radio-development/compile.log`; native artifacts
are under `/data/esp32radio-development-build`. Old source snapshots are retained
for inspection. Sync is refused during a managed compile; do not edit or install
the managed development config through the dashboard while a build is running.

After reviewing the result, explicitly choose **Install wirelessly** on the
development entry when you want to flash it. Wi-Fi OTA works with the radio
remaining in place; connecting USB to Proxmox does not speed compilation.
Device Builder's wireless logs use ESPHome's API and cannot capture the earliest
boot messages or crashes before networking starts. Genuine serial boot/crash
logs require USB connected to a host, with passthrough to VM 100 if using HA.

For an authorized, checksum-guarded OTA followed by bounded hardware regression:

```bash
python3 tools/ha_build.py compile
python3 tools/ha_build.py verify
python3 tools/ha_build.py upload --device 192.168.1.107
python3 tools/hardware_validate.py --stage regression --scenario all \
  --report .esphome/hardware/regression.json
python3 tools/hardware_validate.py --stage soak --scenario soak --soak-minutes 30 \
  --transitions 20 --dwell 60 --report .esphome/hardware/soak.json
```

Use a Python environment with ESPHome's `aioesphomeapi` and PyYAML installed.
The runner checks source/build/upload digests and the device's compilation time,
records original station/volume/mute/BT state, and restores it on exit. It expects
the radio initially playing and exercises pause/stop/resume, HTTPS AAC, standalone
and queued announcements, and the assistant session. API latency, actual PCM/I2S
bytes, write errors, internal heap low-water/largest block, PSRAM and loop timing
are saved with raw logs beside the JSON report. Lifecycle failures are counted
over the whole log, not only inside sampled windows: any control timeout, player
failure, allocation/decoder-memory failure or startup timeout other than the one
deliberately injected fails the run. Sustained PCM means every settled one-second
`Audio diag` bucket carries at least 80% of the nominal byte rate (the first bucket
of each window may straddle a command); "PLAYING" without PCM is never accepted.
Announcement PCM (`ann=`) and post-gain peak (`peak=`) are logged separately, so
each announcement, both of two queued announcements, an announcement while the
radio is muted (radio peak stays 0) and a paused radio staying silent after an
announcement or a voice session are proven from output bytes. Bridge enable's
two-second transition separately records the nonblocking bridge's startup
backpressure, then requires clean settled writes. A connection loss fails rather
than silently treating a reboot as recovery. Logs confirm PCM flow, **not audible
fidelity or successful speech**; per-second counters cannot rule out sub-second
underruns, so a listener's stutter report outranks clean counters.
Durations (deadlines, recovery times, soak length) are measured in device time:
the runner fits the `radio_health` uptime against the host clock
(`clock_device_per_host` in the report), because a drifting host clock (one dev PC
ran ~10% fast) would otherwise inflate deadlines and shorten soaks.
The quiet synthetic-tone server (`tools/ha_tone.py`, local `ffmpeg`, no HA
restart) runs inside HA's container for a bounded lifetime and logs each request.
`/tone.mp3` is a 3-second -30 dB tone; `/slow.mp3` sends headers and then one byte
per second until the server expires, so the lifecycle scenario proves the 20-second
startup deadline (measured from the actual launch), HTTP cancellation, a single
connection closed by the device long before the server expires, and recovery
within 15 seconds; `/invalid.mp3` is non-MP3 data that forces a decoder error and
proves teardown and recovery. The soak scenario cycles AAC, MP3, announcement,
superseded (PLAY during switch teardown), invalid-stream and stalled-startup
transitions with a sustained-PCM dwell after each and reports the internal heap
trend (free, low-water, largest block), loop timing, errors and resets. The
first cycle is reported as a warm-up step; the leak check compares per-kind heap
ceilings across the later cycles.
`--scenario connection` is the short targeted check for HTTP connection reuse and GMF
run/stop races, judged by firmware counters in the `Runtime diag` line
(`http_close=cancel:all,tls stale:all,tls gmf_settle=waits,max_ms,timeouts
gmf_stop=retries,unconfirmed`, cumulative since boot):
- three HTTP 404 opens of a different host (`/missing.mp3`) that must recover;
- `--same-host-runs` 404 announcements followed by a queued same-server announcement: the
  tone server keeps the 404 connection alive (HTTP/1.1, held up to 120 s, logged as
  `KEEPALIVE_404`/`KEEPALIVE_END`), so each run must add exactly one plain stale close and
  the device, not the server, must end every held connection;
- PLAY during KROQ (HTTPS) starts until `--tls-cancels` starts were cancelled after their
  TLS connect, with flat settled PSRAM (a leaked TLS session is ~22 KB);
- `--stale-stop-runs` STOPs of a queued announcement whose open waits 5 s for headers
  (`/slow-open.mp3`) right after a 404 announcement: each must count one repeated GMF stop,
  no unconfirmed stop and no GMF `Run timeout` (`--scenario stale_stop` runs only this);
- `--superseded-runs` PLAY-during-switch starts with flat settled PSRAM.
Any GMF `Run timeout`, settle-wait timeout or unconfirmed stop fails every scenario.
Use `--scenario full`, `announcement`, `lifecycle`, `connection`, `stale_stop`, `assistant` or `soak` for stage checks.

For manual builds outside this remote development workflow:

```bash
esphome compile esp32radio.yaml                          # build
esphome upload esp32radio.yaml --device /dev/ttyACM0     # flash via USB
esphome upload esp32radio.yaml                           # flash via OTA (after first USB flash)
esphome logs esp32radio.yaml --device /dev/ttyACM0       # serial logs
```

Run commands from the repository root. To build pending local component changes,
use a temporary configuration with `external_components` set to
`source: {type: local, path: components}` instead of the Git source; retain the
node name so build and upload use the same native build directory.

Mute is independent of the 22-step volume setting: HA and touch mute preserve
volume, changing volume does not unmute, and zero volume is silent without
setting mute. Voice replies and cues retain their fixed internal-speaker level.
Audio pins use ESPHome GPIO validation; the shared amplifier pin declares
`allow_other_uses: true` in both components. LovyanGFX's autodetected display pins
remain board-specific and are not registered by these audio schemas.

The GMF engine still owns the shared speaker/microphone I2S bus, ICY/AAC decoding,
the FFT spectrum PCM tap and BT PCM routing. Display volume, mute and playback state use the standard
MediaPlayer interface, and its BT toggle uses the standard Switch interface;
station selection and metadata still use the custom radio backend.
These remain migration work before adopting ESPHome's
standard audio sources; Sendspin is not enabled by this preparation.

### Native audio migration experiment

`experiments/native-audio.yaml` is a separate BOX-3 probe using ESPHome
2026.11.0b2's native full-duplex I2S, a 48 kHz speaker/microphone bus, 16 kHz
microphone resampling, a standard media player/mixer/output resamplers, and custom
GMF HTTP sources for AAC/MP3 and ICY titles. Separate software gain adapters keep
radio volume/mute away from the shared DAC and fixed-level voice output. Radio-only
FFT capture taps accepted PCM before gain, including while radio output is muted.
It does **not** replace the working radio or provide Sendspin, the display or voice
assistant yet. Bluetooth is a separate experimental probe.

```bash
python3 tools/ha_native_build.py compile
python3 tools/ha_native_build.py status
# Experimental I2S1 variant; full-load/soak validation remains:
python3 tools/ha_native_build.py compile --bluetooth
# Add standard wake-word/assistant ownership (optionally with I2S1):
python3 tools/ha_native_build.py compile --voice --bluetooth
ESPHOME_NATIVE_BETA=/path/to/isolated/beta/bin/esphome \
  python3 -m unittest discover -s tests -p test_native_audio.py -v
```

The helper installs the pinned beta requirements into a separate virtual
environment under `/data/radio-native-beta/` inside the existing HA Device
Builder. The stable add-on stays on 2026.10.2. It shares the stable build lock
and resource checks, uses one compile job, links only HA's existing secrets,
and records checksums for both the OTA binary and ELF. Configuration, logs and
source snapshots live under `/config/esphome/radio-native-beta/`, with build
artifacts under `/data/radio-native-beta/build/`. There is deliberately no
upload action; a beta flash requires separate permission and a verified stable
rollback artifact.

For an authorized hardware trial, start **Probe Microphone** and exercise
playback, pause, stop/idle and announcements on **Probe Player**. The amplifier
starts **off**; enable **Probe Amplifier** only for listening. At steady state,
`native_probe` logs should show about 192,000 raw microphone bytes/s, 32,000
resampled bytes/s, and 48,000 speaker frames/s during playback. Mic peak checks
require an unmuted hardware microphone. Heap, low-water, largest free block,
PSRAM and loop-time sensors accompany the logs. Counters do not prove audible
quality or clock continuity: those require hardware trials. `Probe Phase` changes
from connecting to playing only after actual DAC playback, not buffer acceptance.
The shared DAC stays at unity/unmuted; radio mute and volume zero do not silence
fixed-gain announcements. GMF handles both HTTP pipelines because its codec archive
and ESPHome's standard HTTP MP3 decoder export conflicting decoder symbols.

Hardware trial on 2026-11-09 passed on beta firmware compiled at 15:10:40 +0200
(OTA SHA-256 `09aba5c2…6882cf63`): 48 kHz stereo microphone capture and 16 kHz
resampling continued through MP3 playback, pause/resume, stop/idle and a
three-second announcement. Settled speaker output was 48,000 frames/s, and
stopping microphone capture did not stop playback. The native-only probe had
180,636 bytes minimum free heap and a 131,072-byte minimum largest block.
One STOP operation blocked the main loop for 319 ms (323 ms maximum loop time);
this remains a responsiveness concern for the full UI/wake workload.
Reports and logs are in `.esphome/hardware/native-beta-confirm.*`. The amplifier
was off, so audible fidelity was not verified. The stable firmware and original
station, volume/mute, Bluetooth and wake settings were restored and PCM checked.
Subsequent native GMF trials passed MP3/HTTPS AAC, ICY titles, pause/resume,
independent radio/announcement gain, six invalid/404 recoveries, slow-stream
cancellation and the actual-PCM startup deadline. Control remained off the main
loop (55 ms maximum measured loop time). A later spectrum-only trial passed the
same duplex/gain baseline with a 68 ms maximum loop time. Reports are
`.esphome/hardware/native-gmf-{trial,lifecycle}.*` and `native-spectrum-isolation.*`.
Stable firmware and original settings were restored after every trial.

The isolated I2S1 probe initially crashed and rolled back at boot. USB logs from
the Windows host (attached to WSL using usbipd) identified switch restoration
calling `ResamplerSpeaker::stop()` before the resampler's event group existed.
The gain adapter now stops only taps it has actually started; no I2S driver
changes were needed. A reversible routing trial on firmware compiled
2026-11-09 21:48:24 +0200 (OTA SHA-256 `c47f059e…9889ea98`) then passed:
simultaneous 48 kHz internal output, 44.1 kHz bridge output and continuous mic,
pre-gain FFT while muted, fixed-gain announcements, three BT toggles, and
standalone voice output restricted to I2S0. Settled measurement windows had no
tap drops; startup/toggle/announcement transitions did log drops, so this is not
a lossless-output guarantee. Baseline loop time peaked at 75 ms. Evidence is in
`.esphome/hardware/native-routing-usb-fixed.*`; the boot capture is
`native-routing-usb.serial.log`. Stable firmware and settings were restored.
The variant remains separate from the default probe. Its PCM tap is nonblocking,
reports dropped bytes explicitly, and does not receive voice PCM.
Bluetooth clock drift, audible output, full wake/assistant/UI load and the long
soak remain readiness gates. A2DP latency is not Sendspin group synchronization.

`experiments/native-audio-voice.yaml` adds standard `micro_wake_word` (`okay_nabu`)
and `voice_assistant` on the same native 16 kHz microphone resampler. Wake starts
only when **Probe Wake Word** is enabled, and stops during sessions and replies.
**Probe Ask Assistant** / **Probe Stop Assistant** exercise the real HA pipeline;
**Probe Voice Begin** / **Probe Voice End** exercise the shared session scripts
without requiring speech. A session locally ducks the radio, pauses only the
GMF radio source, enables the internal amplifier and waits for reply buffers to
drain before restoring playback and the previous amplifier state. Follow-up
sessions cancel pending restoration; an initially paused or explicitly stopped
radio is not restarted. This is a diagnostic configuration, not production
session/source arbitration: cues, display, standalone-announcement PA ownership
and Sendspin's **local duck without group pause** still need integration.

The native probe disables **hardware SHA acceleration**, not certificate
verification. Repeated HTTP/voice-to-HTTPS trials exposed incorrect PSA
SHA-256 certificate digests despite a successful hash return; temporary
diagnostics compared them with the independent libsodium software implementation.
TLS-first trials passed, but the reuse sequence failed even after wake stopped,
with ample heap and stack. The experiment therefore selects ESP-IDF's software
SHA implementation and the build helper checks the compiled SDK configuration.
The stable production configuration is unchanged.

Voice trials passed the shared native microphone/wake load for two 180-second
windows, fixed-gain reply/radio restoration, follow-up cancellation, preservation
of a manually paused radio and STOP during a session. Bluetooth dropped 148 and
69,408 tap bytes respectively during those windows, so sustained losslessness
has **not** passed. Separate HTTPS-first trials passed all six AAC/MP3 cases
with wake on/off; real HA Assist reached `listening`, then stopped and restored
the radio. This does not verify spoken wake detection, STT or audible replies.

The combined voice gate is still **blocked by HTTP-to-HTTPS source reuse**.
The software-SHA image (compiled 2026-11-09 23:46:31 +0200, OTA SHA-256
`4e790532…10994ca0`) no longer logged the certificate-signature error in that
sequence, but it stalled during HTTP open, hit the actual-PCM startup deadline
and then the control-timeout quarantine. HTTPS-first playback on the same image
passed. Do not treat the SHA change as a complete fix or relax the deadlines.
Evidence: `.esphome/hardware/native-voice-{trial,confirm,tls,tls-sequence}.*`,
`native-voice-psa{,-hash}.*`, `native-voice-tls-software-{sha,first}.*`.
Stable firmware, original settings and actual radio PCM were restored after
every trial. Display/cues, full source arbitration and the long soak remain open.

### Phase 1 (preparation) status

Accepted on firmware compiled 2026-11-08 10:20:45 +0200 (ESPHome 2026.10.2, ESP-IDF
6.0.1, artifact SHA-256 `cbda1435…2382ff`): the full regression scenario passed 14/14
checks, and a 30.5-minute soak (`phase1-final-soak3`) passed 48 AAC/MP3/announcement/
PLAY-during-switch/invalid-stream/stalled-startup transitions with flat internal heap
(low-water 34880, largest block 31744), flat PSRAM, no control timeouts, write errors
or resets, and only the deliberately injected startup timeouts. Spoken TTS reply and
radio resume were confirmed by ear. Known limitations:
- Line-rate invalid/undecodable streams starve Core 1 (wake-word inference outranks
  the ESPHome loop), giving 200–300 ms UI/main-loop gaps; audio is unaffected and
  normal playback/transitions stay below ~190 ms.
- Enabling the BT bridge has an expected ~2 s backpressure transient.
- A stale TLS connection held at PRE_REQUEST has not been reproduced on hardware;
  plain-HTTP stale closes and TLS cancel-after-connect are covered.
- Internet stations can underrun upstream (one ~1.5 s mid-stream PCM gap was seen on
  KROQ); there is no HTTP receive-rate diagnostic to attribute such gaps.

## Testing with Copilot remote agent

This repository includes `.github/workflows/copilot-setup-steps.yml` to prepare Copilot cloud agent with build tools and run smoke builds:

- `esphome compile esp32radio.yaml`
- `pio run -e bt-bridge --project-dir bt-bridge`

To avoid TLS download failures in corporate/proxied networks, set these optional values as repository- or organization-level Actions secrets/variables (not in an environment unless the workflow is updated to use one):

- `HTTPS_PROXY`, `HTTP_PROXY`, `NO_PROXY` (secret or variable)
- `COPILOT_CA_CERT_PEM` (secret or variable containing your PEM CA chain)

The setup workflow writes `COPILOT_CA_CERT_PEM` to a temp file and exports:
`SSL_CERT_FILE`, `REQUESTS_CA_BUNDLE`, `CURL_CA_BUNDLE`, `NODE_EXTRA_CA_CERTS`, and `ssl_cert_file`.

After merging this workflow to the default branch, you can validate it from **Actions** by running **Copilot Setup Steps** manually.

### WSL USB passthrough

WSL can't see USB devices natively. Install [usbipd-win](https://github.com/dorssel/usbipd-win) on **Windows** (`winget install usbipd`), then:

```powershell
usbipd list                          # find the ESP32-S3 bus ID
usbipd bind --busid <BUSID>          # first time only
usbipd attach --wsl --busid <BUSID>  # re-run after each device reset
```

In WSL, grant serial access once: `sudo usermod -aG dialout $USER` (re-login required).

### Crash diagnostics

DEBUG `Health` logs include internal heap free/minimum/largest block, free PSRAM,
maximum main-loop time, and uptime every 5 seconds. The debug component also reports the
reset reason. These sensors are internal and do not add Home Assistant entities.
`Runtime diag` adds one-second radio-loop work/gap, control ownership and RX/voice
state. `Audio diag` distinguishes decoded bytes from `written` I2S0 bytes.
Allocation failures retain requested size/capabilities and internal fragmentation
at the failure point. The public **Playback Phase** diagnostic distinguishes
`connecting`, `playing`, `paused`, `announcing`, `voice`, `stopping`, `retrying`,
`control_timeout` and `idle`. ESPHome 2026.10.2 deliberately maps ANNOUNCING to
PLAYING on its native media-player API; use Playback Phase to distinguish them.
PLAYING is entered only after PCM actually reaches I2S0, not when GMF accepts a URL.

All player stop/start/pause/resume calls are serialized on a dedicated Core 0
control worker. Superseded starts are discarded (without a second back-to-back GMF
stop, which can deadlock GMF's task); teardown callbacks cannot finish
a new announcement. A 10-second control deadline quarantines audio until the
worker returns (it never starts another operation over a stuck worker), and a
20-second no-PCM startup deadline, started by the worker when the pipeline is
actually launched, requests a stop/retry. Every stop or new stream bumps a stream
epoch; the HTTP hooks then fail (PRE_REQUEST/ON_REQUEST/ON_RESPONSE return -1), so
GMF's internal reconnect cannot contact the server after cancellation, and our own
reads poll in 200 ms slices so a trickling server cannot pin GMF's IO task (a fully
idle socket still fails after 30 s). Remaining bounds: connect/header fetch is limited
only by GMF's 30-second client timeout. A failed PAUSE/RESUME leaves GMF in an
unknown state, so the worker immediately stops the pipeline in the same serialized
command and logs the result. PAUSE clears the playback request: a paused radio is
not resumed after an announcement or voice session (it stays idle until PLAY). Wake capture releases RX
when a voice/standalone-announcement session owns the bus and resumes after restore.
Codec CPU workspaces use the archives' weak memory hooks overridden in a separate,
explicitly retained translation unit; TLS buffers also use PSRAM. I2S DMA remains
internal. This avoids the measured 35,460-byte AAC allocation failing against a
31,744-byte largest internal block; abundant total free memory alone is insufficient.
`Mic diag` reports capture mode, sample rate, input/output sample counts, minimum
remaining task stack in bytes, maximum resampling time in microseconds, I2S
timeouts/errors, and buffer backlog/dropped bytes.

A small RTC-memory `Previous mic trace` survives panic/software resets while the
board remains powered and is replayed when an API log client requests the config.
Its `read`, `resample`, `buffer`, or `stopped` stage identifies the last microphone
operation, not necessarily the cause of a reset elsewhere. Keep the matching
`firmware.elf` for each flashed build to decode ESPHome's retained crash addresses.

## Controls

| Input              | Action                         |
|--------------------|--------------------------------|
| Touch station name | Switch to that station         |
| Touch VOL −/+      | Decrease/increase volume       |
| Touch MUTE         | Toggle mute                    |
| Touch BT SPEAKER   | Toggle Bluetooth/local speaker (green indicator = active) |
| Home button (red ●)| Start/stop voice assistant     |
| Boot button (GPIO0)| Cycle screen brightness        |
| Mute switch (GPIO1)| Hardware mic mute              |

## Hardware — ESP32-S3-BOX-3

| Peripheral         | Pin(s)                             |
|--------------------|------------------------------------|
| Display (ILI9342C) | SCK=7, MOSI=6, CS=5, DC=4, BL=47  |
| I2S0 Audio (ES8311)| MCLK=2, BCLK=17, LRCK=45, DOUT=15 |
| I2S1 Bridge Output | BCLK=10, LRCK=14, DOUT=11         |
| I2C bus            | SDA=8, SCL=18                      |
| Touch (GT911)      | INT=3 (shared I2C bus)             |
| Power amplifier    | GPIO 46                            |
| Home button (red ●)| GT911 soft key (I2C reg 0x814E)    |
| Boot button        | GPIO 0                             |
| Mute switch        | GPIO 1                             |

## Bluetooth Speaker Output

The ESP32-S3 does not support Bluetooth Classic (A2DP) — only BLE 5.0. To stream audio to a Bluetooth speaker, a second ESP32-WROOM-32D board acts as an I2S-to-A2DP bridge.

### How It Works

1. The main ESP32-S3 decodes the MP3 stream as normal
2. The `audio_process_i2s()` hook siphons decoded PCM to I2S1 TX (when BT mode is active)
3. The WROOM-32D receives audio on I2S0 as a slave, buffers it in a lock-free ring buffer, and streams to a Bluetooth speaker via A2DP

### Wiring (3 signal wires + GND)

| ESP32-S3 (I2S1 TX) | ESP32-WROOM-32D (I2S0 RX) |
|---------------------|----------------------------|
| GPIO 10 (BCLK)     | GPIO 25 (BCLK)            |
| GPIO 14 (LRCK)     | GPIO 26 (LRCK)            |
| GPIO 11 (DOUT)     | GPIO 27 (DIN)             |
| GND                 | GND                        |

### BT Bridge Firmware

The bridge firmware lives in `bt-bridge/`. Configure the target Bluetooth speaker name in `bt-bridge/.env` (or `bt-bridge/include/config.h`, default: `JBL Flip 4`).

```bash
cd bt-bridge
cp .env.sample .env       # set BT_SINK_NAME
pio run -e bt-bridge      # build
pio run -e bt-bridge -t upload --upload-port /dev/ttyUSB0  # flash
```

## Home Assistant Integration

The radio appears as a native ESPHome media player in Home Assistant — no MQTT required.

### Entities

| Entity | Type | Description |
|--------|------|-------------|
| ESP32 Radio | Media Player | Play/pause/stop/volume/mute + play_media for arbitrary URLs |
| Station | Select | Station picker with all configured stations |
| Now Playing | Text Sensor | Current song (Artist - Title) |
| Screen Brightness | Number | Display brightness (1-255) |
| BT Speaker | Switch | Toggle Bluetooth/local speaker output |
| Next/Previous Station | Buttons | Station navigation |

### Setup

1. ESPHome device auto-discovers in HA (Settings → Devices → ESP32 Radio)
2. All entities appear automatically via the native API
3. No MQTT broker required

## Architecture

- **Core 0**: ESP-GMF worker thread — HTTP fetch, decode, PCM output callback, and stream events
- **Core 1**: ESPHome main loop — UI rendering at ~15fps, touch/button input, FFT computation, WiFi, HA API
- lwIP's `tcpip` task is pinned to Core 0 with WiFi and the HTTP reader, so fast downloads do not starve the main loop, which shares Core 1 with mic capture and wake-word inference
- **Display**: PSRAM-backed LovyanGFX sprite (320×218), composed and pushed once per frame
- **Audio**: ES8311 codec via `esp_codec_dev`, ESP-GMF `esp_audio_simple_player` for HTTP/TLS streaming
- **Platform APIs**: Custom components use ESP-IDF/FreeRTOS plus ESPHome network/native API helpers; no Arduino framework
- **FFT**: 128-point radix-2 FFT — Core 0 captures samples (double-buffered), Core 1 computes spectrum
- **I2S Bridge**: `audio_process_i2s()` hook sends decoded PCM to I2S1 TX when BT mode is active
- **HA Integration**: ESPHome native API with publish-on-change (zero polling)

## Project Structure

```
├── esp32radio.yaml                  # Main ESPHome config
├── secrets.yaml                     # WiFi/API credentials (gitignored)
├── components/
│   ├── internet_radio/              # Media player component (audio streaming)
│   ├── i2s_bridge/                  # I2S bridge switch (BT speaker output)
│   └── winamp_display/              # Winamp 2 display, touch, FFT visualizer
├── bt-bridge/                       # ESP32-WROOM-32D BT A2DP bridge firmware
└── archive/
    └── platformio/                  # Legacy PlatformIO firmware (reference only)
```

## License

ES8311 driver: Apache-2.0 (Espressif Systems). Application code: MIT.
