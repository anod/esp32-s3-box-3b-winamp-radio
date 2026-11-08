#!/usr/bin/env python3
"""Bounded, opt-in radio hardware regression with PCM, heap and lifecycle evidence.

Every check is judged from the full device log, not only from sampled windows:
unexpected control or startup timeouts, player failures, resets and decoder memory
errors fail the run even when they happen between measurement windows.
"""

import argparse
import asyncio
import contextlib
import io
import json
from pathlib import Path
import re
import statistics
import time
import sys
from types import SimpleNamespace
from urllib.request import urlopen

import yaml

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.ha_build import guest, snapshot

ANSI = re.compile(r"\x1b\[[0-9;]*m")
RUNTIME = re.compile(
    r"Runtime diag: internal=(\d+) min=(\d+) largest=(\d+) psram=(\d+) "
    r"loop_us=(\d+) gap_ms=(\d+) control=(\d+) reset=(\d+)"
    r"(?: announce=(\d+) rx=(\d+) voice=(\d+))?"
)
PCM = re.compile(r"Audio diag: cb=(\d+) bytes=(\d+) i2s0_to=(\d+) i2s0_err=(\d+)"
                 r" bridge_to=(\d+) bridge_err=(\d+) bridge_short=(\d+) i2s0_short=(\d+)")
AUDIO_EXTRA = re.compile(r"sr=(\d+) bt=(\w+) written=(\d+)(?: ann=(\d+) peak=(\d+))?")
# Cumulative since boot: connections released after a cancelled ON_REQUEST / stale at
# PRE_REQUEST, each as (all, TLS only).
HTTP_CLOSE = re.compile(r"http_close=cancel:(\d+),(\d+) stale:(\d+),(\d+)")
# Cumulative: RUN/STOP commands that first waited for GMF's thread to park after an
# ERROR/FINISHED, the longest wait (ms), and waits that gave up (must stay 0).
GMF_SETTLE = re.compile(r"gmf_settle=(\d+),(\d+),(\d+)")
GMF_STOP = re.compile(r"gmf_stop=(\d+),(\d+)")
CONTROL = re.compile(r"Player control: generation=(\d+) op=(\d+) ms=(\d+) result=(-?\d+)")
OPS = ("STOP", "RUN", "PAUSE", "RESUME")
TONE_URL = "http://192.168.1.10:8794/{}"
TONE_SECONDS = 3
RATE_RATIO = 0.8  # each full-second diag bucket must carry >= 80% of the nominal PCM rate
# Events that must occur exactly as often as the run deliberately injects them.
EXPECTED_EVENTS = ("control_timeouts", "startup_timeouts", "player_failures",
                   "cleanup_stop_failures", "worker_late_returns", "allocation_failures",
                   "decoder_memory_errors", "announcement_stalls")


def clean(line):
    return ANSI.sub("", line)


def audio_line(line):
    match = PCM.search(line)
    if not match:
        return None
    values = list(map(int, match.groups()))
    extra = AUDIO_EXTRA.search(line)
    sr = int(extra[1]) if extra else 0
    return {
        "cb": values[0], "bytes": values[1], "counters": values[2:],
        "sr": sr, "written": int(extra[3]) if extra else None,
        "ann": int(extra[4]) if extra and extra[4] is not None else 0,
        "peak": int(extra[5]) if extra and extra[5] is not None else None,
    }


def rate_ratio(audio):
    return audio["bytes"] / (audio["sr"] * 4) if audio["sr"] else 0.0


HEALTH_UPTIME = re.compile(r"Health: uptime=(\d+)s")
SLOW_COMPONENT = re.compile(r"\]: (\S+) took a long time for an operation \((\d+) ms\)")


def slow_components(lines):
    result = {}
    for line in lines:
        if match := SLOW_COMPONENT.search(line):
            result[match[1]] = max(result.get(match[1], 0), int(match[2]))
    return result


def summarize(lines):
    lines = [clean(line) for line in lines]
    runtime = [tuple(int(x or 0) for x in match.groups()) for line in lines
               if (match := RUNTIME.search(line))]
    audio = [x for line in lines if (x := audio_line(line))]
    written = [x["written"] for x in audio if x["written"] is not None]
    radio = [x for x in audio if x["ann"] == 0 and x["bytes"] > 0]
    controls = [tuple(map(int, match.groups())) for line in lines if (match := CONTROL.search(line))]
    control_ms = {}
    for _, op, ms, _ in controls:
        name = OPS[op] if 0 <= op < len(OPS) else str(op)
        control_ms[name] = max(control_ms.get(name, 0), ms)
    errors = [x for x in lines if re.search(r"\[E\]\[", x)]
    return {
        "runtime_samples": len(runtime),
        "internal_free_min": min((x[0] for x in runtime), default=None),
        "internal_free_max": max((x[0] for x in runtime), default=None),
        "internal_low_water": min((x[1] for x in runtime), default=None),
        "internal_largest_min": min((x[2] for x in runtime), default=None),
        "psram_free_min": min((x[3] for x in runtime), default=None),
        "radio_loop_max_us": max((x[4] for x in runtime), default=None),
        "loop_gap_max_ms": max((x[5] for x in runtime), default=None),
        "reset_reasons": sorted({x[7] for x in runtime}),
        "pcm_callbacks": sum(x["cb"] for x in audio),
        "pcm_samples": len(audio),
        "pcm_bytes": sum(x["bytes"] for x in audio),
        "announcement_bytes": sum(x["ann"] for x in audio),
        "radio_bytes": sum(x["bytes"] - x["ann"] for x in audio),
        "announcement_seconds": sum(1 for x in audio if x["ann"] > 0),
        "announcement_peak_min": min((x["peak"] for x in audio if x["ann"] > 0
                                      and x["peak"] is not None), default=None),
        "radio_peak_max": max((x["peak"] for x in radio if x["peak"] is not None), default=None),
        "pcm_rate_min_ratio": round(min((rate_ratio(x) for x in audio), default=0.0), 3),
        # The first bucket of a window may straddle the preceding command.
        "pcm_rate_settled_min_ratio": round(min((rate_ratio(x) for x in audio[1:]), default=0.0), 3),
        "i2s0_written_bytes": sum(written) if written else None,
        "write_counters": [sum(x["counters"][i] for x in audio) for i in range(6)],
        "control_ops": len(controls),
        "control_max_ms": control_ms,
        "error_lines": len(errors),
        "error_examples": errors[:5],
        "decoder_memory_errors": sum("insufficient memory" in x.lower() or
                                     "enough memory" in x.lower() for x in lines),
        "allocation_failures": sum("Allocation failed:" in x for x in lines),
        "control_timeouts": sum("Player control timed out" in x for x in lines),
        "startup_timeouts": sum("Playback startup timed out" in x for x in lines),
        "worker_late_returns": sum("Player worker returned after timeout" in x for x in lines),
        "player_failures": sum(bool(re.search(r"Player (STOP|RUN|PAUSE|RESUME) failed", x))
                               for x in lines),
        "cleanup_stop_failures": sum("Cleanup stop after failed" in x for x in lines),
        "http_cancellations": sum("HTTP stream cancelled" in x for x in lines),
        "stream_errors": sum("Stream error" in x for x in lines),
        # ESPHome's own per-component loop timing (whole main loop, not radio_control).
        "slow_components": slow_components(lines),
        "stream_finished": sum("Stream finished" in x for x in lines),
        "underrun_reconnects": sum("No audio output for" in x for x in lines),
        "announcement_stalls": sum("Announcement stalled" in x for x in lines),
        "gmf_settle": next(([int(x) for x in m.groups()] for line in reversed(lines)
                            if (m := GMF_SETTLE.search(line))), None),
        # Stops repeated after GMF returned early on a stale STOP bit, and unconfirmed ones.
        "gmf_stop": next(([int(x) for x in m.groups()] for line in reversed(lines)
                          if (m := GMF_STOP.search(line))), None),
        "gmf_run_timeouts": sum("ESP_GMF_TASK: Run timeout" in x for x in lines),
        "http_close": next(([int(x) for x in m.groups()] for line in reversed(lines)
                            if (m := HTTP_CLOSE.search(line))), None),
    }


def sustained_problems(metrics, seconds=None):
    """Return why a window is not sustained real PCM (empty list when it is)."""
    problems = []
    # Diagnostics fire every >=1000 ms of loop time (~1.05 s apart), so compare
    # PCM buckets with telemetry lines and require telemetry for most of the window.
    if seconds is not None and metrics["runtime_samples"] < int(seconds * 0.85) - 1:
        problems.append(f"only {metrics['runtime_samples']} telemetry lines in {seconds} s")
    if metrics["pcm_samples"] < max(1, metrics["runtime_samples"] - 1):
        problems.append(f"PCM present in {metrics['pcm_samples']} of {metrics['runtime_samples']} seconds")
    if metrics["pcm_rate_settled_min_ratio"] < RATE_RATIO:
        problems.append(f"PCM rate dropped to {metrics['pcm_rate_settled_min_ratio']:.2f} of nominal")
    if not metrics["i2s0_written_bytes"]:
        problems.append("no actual I2S0 writes")
    if metrics["decoder_memory_errors"]:
        problems.append("decoder memory failure")
    return problems


def final_problems(overall, expected):
    problems = []
    for name in EXPECTED_EVENTS:
        if overall[name] != expected.get(name, 0):
            problems.append(f"{name}={overall[name]} (expected {expected.get(name, 0)})")
    if overall.get("gmf_run_timeouts"):
        problems.append(f"GMF run timeouts={overall['gmf_run_timeouts']} (expected 0)")
    if overall.get("gmf_settle") and overall["gmf_settle"][2]:
        problems.append(f"GMF settle waits gave up {overall['gmf_settle'][2]} times")
    if overall.get("gmf_stop") and overall["gmf_stop"][1]:
        problems.append(f"GMF stops not confirmed by a terminal event: {overall['gmf_stop'][1]}")
    if len(overall["reset_reasons"]) > 1:
        problems.append(f"reset reason changed: {overall['reset_reasons']}")
    return problems


def tone_requests(text):
    requests, ends = [], []
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[1] == "REQ":
            requests.append({"t": float(parts[0]), "client": parts[2], "path": parts[3]})
        elif len(parts) >= 5 and parts[1] == "SLOW_END":
            ends.append({"t": float(parts[0]), "client": parts[2],
                         "sent": int(parts[3].split("=")[1]), "reason": parts[4].split("=", 1)[1]})
    return requests, ends


def keepalive_problems(text, max_held_s=15.0):
    """Every kept-alive HTTP 404 connection must be closed by the device, promptly."""
    opened, ended, problems = 0, [], []
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] == "KEEPALIVE_404":
            opened += 1
        elif len(parts) >= 6 and parts[1] == "KEEPALIVE_END":
            ended.append({"t": float(parts[0]), "path": parts[3],
                          "held_s": float(parts[4].split("=")[1]), "reason": parts[5].split("=", 1)[1]})
        elif len(parts) >= 3 and parts[1] == "KEEPALIVE_OPEN":
            problems.append(f"device never closed its {parts[3]} connection ({parts[4]})")
    if len(ended) != opened:
        problems.append(f"{opened} kept-alive 404 connections, {len(ended)} ended")
    for end in ended:
        # A close with unread data in the receive buffer makes lwIP send RST.
        if not end["reason"].startswith("client-") or end["held_s"] > max_held_s:
            problems.append(f"404 connection not released by the device in time: {end}")
    return ended, problems


def slow_server_problems(text, recovered_after_s):
    """recovered_after_s: request-to-recovery seconds of each stalled startup, in order."""
    if not isinstance(recovered_after_s, (list, tuple)):
        recovered_after_s = [recovered_after_s]
    requests, ends = tone_requests(text)
    slow = [x for x in requests if x["path"] == "/slow.mp3"]
    problems = []
    if len(slow) != len(recovered_after_s):
        problems.append(f"slow stream requested {len(slow)} times for {len(recovered_after_s)} "
                        "stalled startups (reconnect after cancellation?)")
    if len(ends) != len(slow) or not all(x["reason"].startswith("client-closed") for x in ends):
        problems.append(f"slow stream not closed by the device: {ends}")
    else:
        for request, end, recovered in zip(slow, ends, recovered_after_s):
            if end["t"] - request["t"] > recovered + 3:
                problems.append(f"device kept the slow stream from t={request['t']} open after it had recovered")
    return problems


def worker_done_after(lines, anchor):
    """First main-loop Runtime diag after a main-loop anchor that shows no pending control.

    The anchor must be logged by the main loop right where it enqueues the request, so
    the next Runtime diag line already reflects that request. Unlike the worker's own
    "Player control" lines, these can't be dropped by ESPHome's task log buffer."""
    for index in range(anchor + 1, len(lines)):
        if (match := RUNTIME.search(lines[index])) and match[7] == "0":
            return index
    return None


def kind_drift(transitions, key="internal_free_max"):
    """Per transition kind: median of the last third minus median of the first third.

    Window minima swing by several KB with transient HTTP/decoder allocations; a leak
    lowers the steady-state ceiling (the window maximum) of every later dwell."""
    by_kind = {}
    for entry in transitions:
        if entry.get(key) is not None:
            by_kind.setdefault(entry["kind"], []).append(entry[key])
    drift = {}
    for kind, values in by_kind.items():
        if len(values) < 3:
            continue
        third = len(values) // 3
        drift[kind] = int(statistics.median(values[-third:]) - statistics.median(values[:third]))
    return drift


def warmup_step(transitions, cycle_length, key="internal_free_max"):
    """Per kind: median ceiling after the first cycle minus the first cycle's ceiling."""
    step = {}
    for entry in transitions[:cycle_length]:
        later = [x[key] for x in transitions[cycle_length:] if x["kind"] == entry["kind"]]
        if later and entry.get(key) is not None:
            step[entry["kind"]] = int(statistics.median(later) - entry[key])
    return step


def heap_trend(samples):
    """samples: [(seconds, internal_free, low_water, largest)] -> drift summary."""
    if len(samples) < 4:
        return None
    third = max(1, len(samples) // 3)
    first, last = samples[:third], samples[-third:]
    xs = [x[0] for x in samples]
    ys = [x[1] for x in samples]
    mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
    var = sum((x - mx) ** 2 for x in xs)
    slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / var if var else 0.0
    return {
        "samples": len(samples),
        "first_third_free_min": min(x[1] for x in first),
        "last_third_free_min": min(x[1] for x in last),
        "first_third_largest_min": min(x[3] for x in first),
        "last_third_largest_min": min(x[3] for x in last),
        "low_water_start": samples[0][2], "low_water_end": samples[-1][2],
        "free_slope_bytes_per_hour": round(slope * 3600),
    }


def provenance():
    digest, _ = snapshot(ROOT)
    code = r'''
import json, pathlib, sys
base = pathlib.Path("/config/esphome/radio-development")
info = json.loads((base / "built-info.json").read_text())
for name in ("current-sha256", "built-sha256", "uploaded-sha256"):
    if (base / name).read_text().strip() != sys.argv[1]:
        raise RuntimeError("Hardware validation source/build/upload mismatch")
if (base / "uploaded-artifact-sha256").read_text().strip() != info["artifact_sha256"]:
    raise RuntimeError("Uploaded artifact does not match build metadata")
print(json.dumps(info))
'''
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        guest(SimpleNamespace(host="proxmox", vm=100, container="app_5c53de3b_esphome"),
              ["python3", "-c", code, digest])
    return json.loads(output.getvalue().splitlines()[-1])


def tone_lifetime(args):
    if args.scenario == "soak":
        return min(3600, args.soak_minutes * 60 + 600)
    if args.scenario == "invalid":
        return 120 + 60 * args.invalid_runs
    return {"announcement": 300, "lifecycle": 420, "connection": 600, "all": 900}.get(args.scenario, 300)


def clock_scale(samples, min_span_s=30.0):
    """Device seconds per host second from (host_time, device_uptime_s) samples.

    Host clocks (notably VMs) can drift by several percent; the device's crystal
    is the reference for deadlines and soak length. Returns None until the
    samples span enough host time for whole-second uptime to be accurate.
    """
    if len(samples) < 3 or samples[-1][0] - samples[0][0] < min_span_s:
        return None
    n = len(samples)
    mx = sum(h for h, _ in samples) / n
    my = sum(u for _, u in samples) / n
    sxx = sum((h - mx) ** 2 for h, _ in samples)
    return sum((h - mx) * (u - my) for h, u in samples) / sxx


async def run(args):
    from aioesphomeapi import APIClient, LogLevel, MediaPlayerCommand, MediaPlayerState

    secrets = yaml.safe_load((ROOT / "secrets.yaml").read_text())
    api = APIClient(args.device, 6053, "", noise_psk=secrets["esp32radio_api_key"])
    states, lines, times, checks = {}, [], [], []
    uptime_samples = []  # (host monotonic, device uptime s)
    expected = {name: 0 for name in EXPECTED_EVENTS}
    report = {"stage": args.stage, "scenario": args.scenario, "checks": checks,
              "windows": {}, "expected_events": expected, "measurements": {}}
    original = None
    sampler = None
    tone_server = None
    tone_log = None
    tone_log_path = args.report.with_suffix(".tone.log")
    latencies = []
    player_states = []
    phase_events = []  # (line index, phase): published by the main loop, never dropped
    slow_recoveries = []
    log_path = args.report.with_suffix(".log")
    args.report.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    with log_path.open("w", buffering=1) as log:
        def record(item):
            line = clean(item.message.decode(errors="replace"))
            lines.append(line)
            times.append(time.monotonic())
            log.write(f"{times[-1]:.3f} {line}\n")
            if match := HEALTH_UPTIME.search(line):
                uptime_samples.append((times[-1], int(match[1])))

        def dev(host_seconds):
            """Convert a host-clock duration to device (real) seconds."""
            return host_seconds * (clock_scale(uptime_samples) or 1.0)

        def phase():
            return states[keys["Playback Phase"]].state

        def find(pattern, start, end=None):
            for index in range(start, len(lines) if end is None else end):
                if re.search(pattern, lines[index]):
                    return index
            return None

        async def wait(predicate, timeout=30, what="hardware condition"):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if predicate():
                    return
                await asyncio.sleep(0.1)
            raise AssertionError(f"Timed out after {timeout}s waiting for {what}")

        async def wait_line(pattern, start, timeout, what):
            await wait(lambda: find(pattern, start) is not None, timeout, what)
            return find(pattern, start)

        def first_radio_pcm(start):
            for index in range(start, len(lines)):
                audio = audio_line(lines[index])
                if audio and audio["ann"] == 0 and rate_ratio(audio) >= RATE_RATIO:
                    return index
            return None

        async def wait_radio_pcm(start, timeout, what):
            await wait(lambda: first_radio_pcm(start) is not None, timeout, what)
            return first_radio_pcm(start)

        async def window(label, seconds, pcm="sustained", bridge_transition=False):
            start = len(lines)
            await asyncio.sleep(seconds)
            await api.device_info()  # A reset/disconnection fails the run, never silently reconnect.
            metrics = summarize(lines[start:])
            report["windows"][label] = metrics
            if not metrics["runtime_samples"]:
                raise AssertionError(f"{label}: missing memory/performance telemetry")
            if pcm == "sustained" and (problems := sustained_problems(metrics, seconds)):
                raise AssertionError(f"{label}: " + "; ".join(problems))
            if pcm == "none" and metrics["pcm_bytes"]:
                raise AssertionError(f"{label}: unexpected PCM ({metrics['pcm_bytes']} bytes)")
            counters = metrics["write_counters"]
            if bridge_transition:
                report.setdefault("transition_backpressure", {})[label] = {
                    "bridge_timeouts": counters[2], "bridge_short_writes": counters[4],
                }
                # The bridge intentionally uses nonblocking writes. Record startup
                # backpressure separately; the following settled window must be clean.
                counters = [counters[i] for i in (0, 1, 3, 5)]
            if any(counters):
                raise AssertionError(f"{label}: I2S errors/timeouts/short writes {counters}")
            if metrics["control_timeouts"] or metrics["player_failures"]:
                raise AssertionError(f"{label}: player control failure")
            print(label, json.dumps({k: metrics[k] for k in (
                "internal_free_min", "internal_largest_min", "radio_loop_max_us",
                "loop_gap_max_ms", "pcm_bytes", "pcm_rate_settled_min_ratio", "announcement_bytes")}),
                flush=True)
            return metrics

        async def start_tone_server():
            nonlocal tone_server, tone_log
            if tone_server:
                return
            tone_log = tone_log_path.open("w")
            tone_server = await asyncio.create_subprocess_exec(
                sys.executable, str(ROOT / "tools/ha_tone.py"), "--seconds", str(tone_lifetime(args)),
                stdout=tone_log, stderr=tone_log)
            report["tone_server_started_s"] = round(time.monotonic() - started, 1)
            deadline = time.monotonic() + 30
            url = TONE_URL.format("tone.mp3")
            while True:
                if tone_server.returncode is not None:
                    raise RuntimeError("HA tone server exited before becoming ready")
                try:
                    def probe():
                        with urlopen(url, timeout=2) as response:
                            return response.status == 200
                    if await asyncio.to_thread(probe):
                        return
                except OSError:
                    if time.monotonic() >= deadline:
                        raise RuntimeError("HA tone server did not become responsive")
                await asyncio.sleep(1)

        async def ensure_playing(label):
            if states[player].state != MediaPlayerState.PLAYING or phase() != "playing":
                start = len(lines)
                api.media_player_command(player, command=MediaPlayerCommand.PLAY)
                await wait_radio_pcm(start, 25, f"{label}: radio PCM")
            await window(label, 6)

        async def announce(label, count=1, then="playing", radio_after=True):
            """Play `count` quiet tones as announcements and prove each one was output."""
            start = len(lines)
            sent = time.monotonic()
            url = TONE_URL.format("tone.mp3")
            api.media_player_command(player, media_url=url, announcement=True)
            await wait(lambda: phase() == "announcing", 10, f"{label}: announcing phase")
            for _ in range(count - 1):
                api.media_player_command(player, command=MediaPlayerCommand.ENQUEUE,
                                         media_url=url, announcement=True)
            await wait(lambda: phase() != "announcing", 10 + 8 * count, f"{label}: announcement end")
            ended = time.monotonic()
            ended_index = len(lines)
            if radio_after:
                await wait_radio_pcm(ended_index, 20, f"{label}: radio resume")
            else:
                await wait(lambda: phase() == then, 10, f"{label}: phase {then}")
            await asyncio.sleep(1.2)  # include the final diagnostic bucket
            metrics = summarize(lines[start:])
            need = int(count * TONE_SECONDS * 44100 * 4 * RATE_RATIO)
            if metrics["announcement_bytes"] < need:
                raise AssertionError(f"{label}: announcement PCM {metrics['announcement_bytes']} < {need}")
            if not metrics["announcement_peak_min"]:
                raise AssertionError(f"{label}: announcement output was silent")
            report["measurements"][label] = {
                "announcement_bytes": metrics["announcement_bytes"],
                "announcement_seconds": metrics["announcement_seconds"],
                "announcement_peak_min": metrics["announcement_peak_min"],
                "phase_seconds": round(ended - sent, 1),
                "radio_peak_max": metrics["radio_peak_max"],
            }
            return metrics

        async def announcement_checks():
            await start_tone_server()
            await ensure_playing("pre_announcement")
            await announce("announcement")
            await window("announcement_restored", 8)
            checks.append("announcement_pcm_and_radio_restore")

            await announce("queued_announcements", count=2)
            await window("queued_announcements_restored", 8)
            checks.append("two_queued_announcements_both_output")

            api.media_player_command(player, command=MediaPlayerCommand.MUTE)
            await wait(lambda: states[player].muted, 5, "mute")
            await asyncio.sleep(1.2)  # discard the diagnostic bucket that straddles the mute
            muted = await window("muted_radio", 4)
            if muted["radio_peak_max"] != 0:
                raise AssertionError("muted radio produced nonzero PCM")
            metrics = await announce("announcement_while_muted")
            after = await window("muted_radio_after_announcement", 5)
            if after["radio_peak_max"] != 0:
                raise AssertionError("radio unmuted itself after an announcement")
            if metrics["announcement_peak_min"] <= 0:
                raise AssertionError("announcement was silenced by radio mute")
            api.media_player_command(player, command=MediaPlayerCommand.UNMUTE)
            await wait(lambda: not states[player].muted, 5, "unmute")
            checks.append("announcement_audible_while_radio_muted")

            api.media_player_command(player, command=MediaPlayerCommand.PAUSE)
            await wait(lambda: states[player].state == MediaPlayerState.PAUSED, 5, "pause")
            await asyncio.sleep(1.2)
            await window("paused_before_announcement", 3, "none")
            await announce("announcement_while_paused", then="idle", radio_after=False)
            idle = await window("paused_after_announcement", 10, "none")
            if states[player].state == MediaPlayerState.PLAYING:
                raise AssertionError("paused radio resumed after an announcement")
            report["measurements"]["paused_after_announcement_radio_bytes"] = idle["radio_bytes"]
            start = len(lines)
            api.media_player_command(player, command=MediaPlayerCommand.PLAY)
            await wait_radio_pcm(start, 20, "play after paused announcement")
            await window("played_after_paused_announcement", 6)
            checks.append("paused_radio_stays_silent_after_announcement")

        async def assistant_check(paused):
            label = "assistant_paused" if paused else "assistant"
            if paused:
                api.media_player_command(player, command=MediaPlayerCommand.PAUSE)
                await wait(lambda: states[player].state == MediaPlayerState.PAUSED, 5, "pause")
                await asyncio.sleep(1.2)
            start = len(lines)
            api.button_command(keys["Ask Assistant"])
            began = await wait_line(r"Runtime diag: .* voice=1", start, 15, f"{label}: voice session")
            ended = await wait_line(r"Runtime diag: .* announce=0 rx=\d voice=0", began, 65,
                                    f"{label}: voice session end")
            report["measurements"][label] = {"session_s": round(dev(times[ended] - times[began]), 1)}
            if paused:
                await window(f"{label}_after", 10, "none")
                if states[player].state == MediaPlayerState.PLAYING:
                    raise AssertionError("paused radio resumed after a voice session")
                start = len(lines)
                api.media_player_command(player, command=MediaPlayerCommand.PLAY)
                await wait_radio_pcm(start, 20, "play after paused voice session")
                await window(f"{label}_played", 6)
                checks.append("paused_radio_stays_silent_after_voice_session")
            else:
                index = await wait_radio_pcm(ended, 25, f"{label}: radio resume")
                report["measurements"][label]["resume_s"] = round(dev(times[index] - times[ended]), 1)
                await window(f"{label}_restored", 10)
                checks.append("assistant_restores_radio")

        async def stalled_startup_check():
            await start_tone_server()
            await ensure_playing("pre_stalled_startup")
            start = len(lines)
            sent = time.monotonic()
            api.media_player_command(player, media_url=TONE_URL.format("slow.mp3"))
            requested = await wait_line(r"Play URL: .*slow\.mp3", start, 10, "slow stream request")
            await wait(lambda: worker_done_after(lines, requested) is not None, 10, "slow stream launch")
            # Prefer the worker's exact line; ESPHome may drop it, then fall back to the
            # main-loop control flag (upper bound with ~1 s resolution).
            timeout = await wait_line("Playback startup timed out", requested, 25, "startup timeout")
            expected["startup_timeouts"] += 1
            launch = find(r"Player control: .* op=1 ", requested, timeout)
            source = "worker_line"
            if launch is None:
                launch, source = worker_done_after(lines, requested), "runtime_diag"
                launch_ms = round(dev(times[launch] - times[requested]) * 1000)
            else:
                launch_ms = int(CONTROL.search(lines[launch])[3])
            await wait(lambda: worker_done_after(lines, timeout) is not None, 5, "teardown STOP")
            done = worker_done_after(lines, timeout)
            recovered = await wait_radio_pcm(done, 20, "station recovery after stalled startup")
            stop = find(r"Player control: .* op=0 ", timeout, recovered)
            if stop is None:
                stop, stop_source = done, "runtime_diag"
                stop_ms = round(dev(times[stop] - times[timeout]) * 1000)
            else:
                stop_ms = int(CONTROL.search(lines[stop])[3])
                stop_source = "worker_line"
            # Ignore the first post-launch bucket: it may contain the old stream's tail.
            first_bucket = find("Audio diag:", launch, timeout) or launch
            stalled = summarize(lines[first_bucket + 1:timeout])
            launch_to_timeout = dev(times[timeout] - times[launch])
            measurement = {
                "launch_to_timeout_s": round(launch_to_timeout, 2),
                "launch_control_ms": launch_ms, "launch_source": source,
                "teardown_stop_ms": stop_ms, "teardown_source": stop_source,
                "timeout_to_pcm_s": round(dev(times[recovered] - times[timeout]), 2),
                "request_to_pcm_s": round(dev(times[recovered] - sent), 2),
                "stalled_pcm_bytes": stalled["pcm_bytes"],
                "http_cancellations": summarize(lines[timeout:recovered + 1])["http_cancellations"],
            }
            report["measurements"]["stalled_startup"] = measurement
            if stalled["pcm_bytes"]:
                raise AssertionError("stalled startup produced PCM")
            if any(times[launch] <= t <= times[timeout] and state == MediaPlayerState.PLAYING
                   for t, state in player_states):
                raise AssertionError("stalled startup falsely published PLAYING")
            # The Runtime-diag launch estimate is up to one diag interval late.
            slack = 0 if source == "worker_line" else 1.2
            if not 19.0 - slack <= launch_to_timeout <= 21.5 + launch_ms / 1000:
                raise AssertionError(f"startup deadline not measured from launch: {launch_to_timeout:.2f}s")
            if stop_ms > (2000 if stop_source == "worker_line" else 3200):
                raise AssertionError(f"stalled HTTP teardown took {stop_ms} ms ({stop_source})")
            if measurement["http_cancellations"] < 1:
                raise AssertionError("HTTP reader was not cancelled")
            if measurement["timeout_to_pcm_s"] > 15:
                raise AssertionError("recovery after stalled startup exceeded 15 s")
            slow_recoveries.append(measurement["request_to_pcm_s"])
            await window("stalled_startup_restored", 8)
            checks.append("stalled_startup_times_out_from_launch_cancels_http_and_recovers")

        invalid_runs = []
        report["measurements"]["invalid_runs"] = invalid_runs

        async def invalid_stream_check():
            await start_tone_server()
            await ensure_playing("pre_invalid_stream")
            start = len(lines)
            api.media_player_command(player, media_url=TONE_URL.format("invalid.mp3"))
            requested = await wait_line(r"Play URL: .*invalid\.mp3", start, 10, "invalid stream request")
            await wait(lambda: worker_done_after(lines, requested) is not None, 10, "invalid stream launch")
            # The Core-0 "Stream error" line can be relayed late or dropped; the main loop's
            # retry is then the evidence (its other causes log their own main-loop lines).
            failure = await wait_line(r"Stream error|Stream finished|Playback startup timed out|"
                                      r"Retrying stream", requested, 25, "decoder failure")
            launch = find(r"Player control: .* op=1 ", requested, failure)
            if launch is None:
                launch = min(worker_done_after(lines, requested), failure)
            outcome = lines[failure]
            if "Retrying stream" in outcome:
                if (find(r"Player control timed out|Player control: .* op=1 .*result=[^0]",
                         requested, failure) is not None or launch == failure):
                    raise AssertionError("invalid stream retried without a decoder failure")
                outcome = "Stream error (inferred from main-loop retry; task log late or dropped)"
            if "startup timed out" in outcome:
                expected["startup_timeouts"] += 1
            recovered = await wait_radio_pcm(failure, 20, "station recovery after decoder failure")
            first_bucket = find("Audio diag:", launch, failure) or launch
            bad = summarize(lines[first_bucket + 1:failure])
            stops = [int(m[3]) for line in lines[failure:recovered]
                     if (m := CONTROL.search(line)) and m[2] == "0"]
            report["measurements"]["invalid_stream"] = {
                "outcome": outcome.split(": ", 1)[-1][:80],
                "launch_to_failure_s": round(dev(times[failure] - times[launch]), 2),
                "failure_to_pcm_s": round(dev(times[recovered] - times[failure]), 2),
                "pcm_bytes_before_failure": bad["pcm_bytes"],
                "teardown_stop_ms": max(stops, default=None),
            }
            if stops and max(stops) > 2000:
                raise AssertionError("decoder failure teardown took more than 2 s")
            await window("invalid_stream_restored", 8)
            during = summarize(lines[requested:])
            report["measurements"]["invalid_stream"].update(
                loop_gap_max_ms=during["loop_gap_max_ms"], radio_loop_max_us=during["radio_loop_max_us"],
                slow_components=during["slow_components"])
            invalid_runs.append(report["measurements"]["invalid_stream"])
            checks.append("decoder_failure_tears_down_and_recovers")

        async def failed_open_check(repeats=3):
            # An HTTP 404 fails the open after connecting; GMF then skips the IO close and
            # keeps the connected client. Reopening it must not leak the socket
            # (LWIP_MAX_SOCKETS is 10); a stale fd, when logged, must not grow.
            await start_tone_server()
            runs, fds = [], []
            for n in range(repeats):
                await ensure_playing(f"pre_failed_open_{n}")
                ceiling = summarize(lines[-12:])["internal_free_max"]
                before = await http_close_counts(f"failed_open_{n}")
                start = len(lines)
                api.media_player_command(player, media_url=TONE_URL.format("missing.mp3"))
                requested = await wait_line(r"Play URL: .*missing\.mp3", start, 10, "missing stream request")
                failure = await wait_line(r"Stream error|Stream finished|Playback startup timed out",
                                          requested, 25, "HTTP 404 failure")
                if "startup timed out" in lines[failure]:
                    raise AssertionError("HTTP 404 was not reported as a stream failure")
                recovered = await wait_radio_pcm(failure, 20, "station recovery after HTTP 404")
                stale = [int(m[1]) for line in lines[requested:recovered + 1]
                         if (m := re.search(r"Closing stale HTTP connection .*\(fd (\d+)\)", line))]
                fds.extend(stale)
                metrics = await window(f"failed_open_{n}_restored", 8)
                after = await http_close_counts(f"failed_open_{n}")
                runs.append({"failure_to_pcm_s": round(dev(times[recovered] - times[failure]), 2),
                             # Recovery reconnects to the station's host: set_url() closes.
                             "http_close_delta": [a - b for a, b in zip(after, before)],
                             "stale_fds": stale, "free_max_before": ceiling,
                             "free_max_after": metrics["internal_free_max"]})
            report["measurements"]["failed_open"] = runs
            if len(set(fds)) > 1:
                raise AssertionError(f"stale HTTP socket numbers grew across failed opens: {fds}")
            checks.append("failed_open_releases_http_connection_and_recovers")

        async def http_close_counts(label):
            """Counters from the first main-loop Runtime diag logged after this call."""
            start = len(lines)
            index = await wait_line(HTTP_CLOSE.pattern, start, 5, f"{label}: http_close counters")
            return [int(x) for x in HTTP_CLOSE.search(lines[index]).groups()]

        async def same_host_failed_open_check(repeats=3):
            # A 404 announcement followed by a queued announcement from the same server:
            # set_url() keeps the failed open's connection (same host/port), so only the
            # PRE_REQUEST guard can release it before esp_http_client_open() reconnects.
            await start_tone_server()
            runs = []
            for n in range(repeats):
                label = f"same_host_failed_open_{n}"
                await ensure_playing(f"pre_{label}")
                before = await http_close_counts(label)
                start = len(lines)
                api.media_player_command(player, media_url=TONE_URL.format("missing-announce.mp3"),
                                         announcement=True)
                api.media_player_command(player, command=MediaPlayerCommand.ENQUEUE,
                                         media_url=TONE_URL.format("tone.mp3"), announcement=True)
                await wait(lambda: phase() == "announcing", 10, f"{label}: announcing phase")
                await wait(lambda: phase() != "announcing", 25, f"{label}: announcement end")
                await wait_radio_pcm(len(lines), 20, f"{label}: radio resume")
                await asyncio.sleep(1.2)
                metrics = summarize(lines[start:])
                after = await http_close_counts(label)
                delta = [a - b for a, b in zip(after, before)]
                need = int(TONE_SECONDS * 44100 * 4 * RATE_RATIO)
                runs.append({"http_close_delta": delta, "announcement_bytes": metrics["announcement_bytes"],
                             "announcement_peak_min": metrics["announcement_peak_min"],
                             "stale_log_lines": sum("Closing stale HTTP connection" in x
                                                    for x in lines[start:])})
                if delta != [0, 0, 1, 0]:
                    raise AssertionError(f"{label}: expected exactly one plain-HTTP stale close, "
                                         f"http_close delta (cancel,tls,stale,tls)={delta}")
                if metrics["announcement_bytes"] < need or not metrics["announcement_peak_min"]:
                    raise AssertionError(f"{label}: queued announcement after the 404 not output: {runs[-1]}")
                await window(f"{label}_restored", 6)
            report["measurements"]["same_host_failed_open"] = runs
            checks.append("same_host_failed_open_closes_stale_connection_before_reopen")

        async def gmf_stop_counts(label):
            start = len(lines)
            index = await wait_line(GMF_STOP.pattern, start, 5, f"{label}: gmf_stop counters")
            return [int(x) for x in GMF_STOP.search(lines[index]).groups()]

        async def stale_stop_check(repeats=3):
            # A run that ends by itself (here a 404 announcement) leaves GMF's STOP bit set
            # until the next run reaches its job loop. Stopping the queued announcement while
            # its open waits 5 s for response headers used to return at once on that stale
            # bit, leaving the pipeline running; the next RUN then timed out after 5 s.
            await start_tone_server()
            runs = []
            for n in range(repeats):
                label = f"stale_stop_{n}"
                await ensure_playing(f"pre_{label}")
                before = await gmf_stop_counts(label)
                start = len(lines)
                api.media_player_command(player, media_url=TONE_URL.format("missing-stale.mp3"),
                                         announcement=True)
                api.media_player_command(player, command=MediaPlayerCommand.ENQUEUE,
                                         media_url=TONE_URL.format("slow-open.mp3"), announcement=True)
                # The 404 fails within ~0.2 s; the queued open then waits 5 s for headers.
                # (The relayed tone-server log is not timely enough to synchronise on.)
                await wait(lambda: phase() == "announcing", 10, f"{label}: announcing phase")
                await asyncio.sleep(2.0)
                stopped = len(lines)
                api.media_player_command(player, command=MediaPlayerCommand.STOP)
                await asyncio.sleep(0.3)
                api.media_player_command(player, command=MediaPlayerCommand.PLAY)
                recovered = await wait_radio_pcm(stopped, 15, f"{label}: radio PCM after stop+play")
                await asyncio.sleep(1.2)
                after = await gmf_stop_counts(label)
                metrics = summarize(lines[start:])
                delta = [a - b for a, b in zip(after, before)]
                runs.append({"gmf_stop_delta": delta, "stop_to_pcm_s": round(dev(times[recovered] - times[stopped]), 2),
                             "gmf_run_timeouts": metrics["gmf_run_timeouts"],
                             "player_failures": metrics["player_failures"],
                             "control_timeouts": metrics["control_timeouts"]})
                print("stale_stop", json.dumps(runs[-1]), flush=True)
                if delta[0] < 1 or delta[1] or metrics["gmf_run_timeouts"] or metrics["player_failures"] \
                        or metrics["control_timeouts"]:
                    raise AssertionError(f"{label}: stale GMF stop not repeated cleanly: {runs[-1]}")
                await window(f"{label}_restored", 6)
            report["measurements"]["stale_stop"] = runs
            checks.append("stop_while_opening_after_self_ended_run_is_confirmed")

        async def tls_cancel_check(min_cancels=3, max_runs=16):
            # PLAY while KROQ's (HTTPS) start is connecting cancels it at ON_REQUEST, after
            # the TLS session exists. Sweep the delay until enough such cancels happened;
            # each used to leak ~22 KB PSRAM, so settled KROQ PSRAM must stay flat.
            delays = [0.6, 0.8, 1.0, 1.2, 1.4, 1.7, 2.0]
            hits, runs, psram, largest = 0, [], [], []
            for n in range(max_runs):
                if hits >= min_cancels:
                    break
                hit_delays = [r["delay"] for r in runs if r["tls_cancels"]]
                delay = hit_delays[-1] if hit_delays else delays[n % len(delays)]
                label = f"tls_cancel_{n}_{delay}"
                before = await http_close_counts(label)
                to_pcm = await superseded_switch("KROQ", delay, label)
                settled = await window(f"{label}_settled", 4)
                after = await http_close_counts(label)
                delta = [a - b for a, b in zip(after, before)]
                hits += delta[1]
                psram.append(settled["psram_free_min"])
                largest.append(settled["internal_largest_min"])
                runs.append({"delay": delay, "tls_cancels": delta[1], "http_close_delta": delta,
                             "time_to_pcm_s": to_pcm, "settled_psram_min": settled["psram_free_min"],
                             "settled_internal_free_max": settled["internal_free_max"],
                             "settled_internal_largest_min": settled["internal_largest_min"]})
                print("tls_cancel", json.dumps(runs[-1]), flush=True)
                report["measurements"]["tls_cancel_runs"] = runs
                await switch_station(original["station"], f"{label}_back")
                await window(f"{label}_back", 4)
            report["measurements"]["tls_cancels_after_connect"] = hits
            if hits < min_cancels:
                raise AssertionError(f"only {hits} HTTPS starts cancelled after connect in {len(runs)} runs")
            if min(psram[1:]) < psram[0] - 8192:
                raise AssertionError(f"settled KROQ PSRAM dropped across HTTPS cancels: {psram}")
            checks.append("https_cancel_after_connect_releases_tls_session")

        async def lifecycle_checks():
            api.media_player_command(player, command=MediaPlayerCommand.PAUSE)
            await wait(lambda: states[player].state == MediaPlayerState.PAUSED, 5, "pause")
            await asyncio.sleep(1.2)
            await window("paused", 3, "none")
            api.media_player_command(player, command=MediaPlayerCommand.PLAY)
            await wait(lambda: states[player].state == MediaPlayerState.PLAYING, 10, "resume")
            await window("resumed", 5)
            api.media_player_command(player, command=MediaPlayerCommand.STOP)
            await wait(lambda: states[player].state == MediaPlayerState.IDLE, 10, "stop")
            await asyncio.sleep(1.2)  # Discard the final pre-command diagnostic bucket.
            await window("stopped", 4, "none")
            start = len(lines)
            api.media_player_command(player, command=MediaPlayerCommand.PLAY)
            await wait_radio_pcm(start, 20, "restart")
            await window("restarted", 8)
            checks.append("pause_resume_stop_restart")

        def replayed(start):
            # Only PCM after the phase left and re-entered "playing" belongs to the new
            # stream (worker log lines can be dropped by ESPHome's task log buffer).
            left = next((i for i, (index, value) in enumerate(phase_events)
                         if value != "playing" and index >= start), None)
            if left is None:
                return None
            return next((index for index, value in phase_events[left:] if value == "playing"), None)

        async def switch_station(name, label):
            start = len(lines)
            sent = time.monotonic()
            api.select_command(station, name)
            await wait(lambda: states[station].state == name, 10, f"{label}: station {name}")
            await wait(lambda: replayed(start) is not None, 25, f"{label}: phase playing on {name}")
            index = await wait_radio_pcm(replayed(start), 25, f"{label}: PCM from {name}")
            return round(dev(times[index] - sent), 2)

        async def superseded_switch(name, delay, label):
            # PLAY during a station switch's ~1 s teardown supersedes the queued RUN; the
            # worker must not issue a second back-to-back GMF stop (that deadlocked GMF).
            start = len(lines)
            api.select_command(station, name)
            await asyncio.sleep(delay)
            api.media_player_command(player, command=MediaPlayerCommand.PLAY)
            await wait(lambda: states[station].state == name, 10, f"{label}: station")
            await wait(lambda: replayed(start) is not None, 25, f"{label}: phase playing")
            index = await wait_radio_pcm(replayed(start), 25, f"{label}: PCM")
            if summarize(lines[start:])["control_timeouts"]:
                raise AssertionError(f"{label}: control timeout")
            return round(dev(times[index] - times[start]), 2)

        async def superseded_run_check(runs=4):
            # A start cancelled after its TLS/TCP connect used to leak that connection
            # (~22 KB PSRAM + ~7 KB internal per TLS session), so the settled PSRAM of
            # each station must not step down across repeats.
            names = ["KROQ", original["station"]]
            psram = {}
            for n in range(runs):
                delay, name = (0.2, 0.5, 0.8, 0.5)[n % 4], names[n % 2]
                label = f"superseded_run_{n}_{name}_{delay}"
                report["measurements"][f"{label}_time_to_pcm_s"] = await superseded_switch(name, delay, label)
                await window(label, 3)
                psram.setdefault(name, []).append((await window(f"{label}_settled", 3))["psram_free_min"])
            report["measurements"]["superseded_settled_psram_min"] = psram
            for name, values in psram.items():
                if min(values[1:], default=values[0]) < values[0] - 8192:
                    raise AssertionError(f"{name}: settled PSRAM dropped across superseded starts: {values}")
            report["measurements"]["superseded_cancelled_after_connect"] = sum(
                "user callback:181" in line for line in lines)
            checks.append("play_during_switch_teardown_has_no_control_deadlock_or_connection_leak")

        async def codec_checks():
            aac_start = len(lines)
            report["measurements"]["aac_time_to_pcm_s"] = await switch_station("KROQ", "aac")
            await window("aac", 20)
            aac_titles = [x for x in lines[aac_start:] if "ICY: " in x]
            mp3_start = len(lines)
            report["measurements"]["mp3_time_to_pcm_s"] = await switch_station(
                original["station"], "switch_from_aac")
            await window("switch_from_aac", 10)
            checks.append("aac_and_mp3_sustained")
            # ICY StreamTitle parsing (GMF + response-header API), required for the MP3 station.
            mp3_titles = [x for x in lines[mp3_start:] if "ICY: " in x]
            report["measurements"]["icy_titles"] = {"aac": len(aac_titles), "mp3": len(mp3_titles),
                                                    "mp3_example": mp3_titles[0].split("ICY: ", 1)[1]
                                                    if mp3_titles else None}
            if not mp3_titles:
                raise AssertionError(f"no ICY StreamTitle from {original['station']}")
            checks.append("icy_title_parsed")

        async def routing_checks():
            api.switch_command(bridge, False)
            await wait(lambda: states[bridge].state is False, 5, "bridge off")
            await window("internal_routing", 6)
            api.switch_command(bridge, original["bridge"])
            await wait(lambda: states[bridge].state == original["bridge"], 5, "bridge restore")
            await window("bridge_enable_transition", 2, bridge_transition=True)
            await window("original_routing", 6)
            api.media_player_command(player, command=MediaPlayerCommand.MUTE)
            await wait(lambda: states[player].muted, 5, "mute")
            api.media_player_command(player, volume=0)
            await wait(lambda: states[player].muted and states[player].volume == 0, 5, "volume 0")
            await asyncio.sleep(1.2)
            muted = await window("muted_zero_volume", 4)
            if muted["radio_peak_max"] != 0:
                raise AssertionError("muted zero-volume radio produced nonzero PCM")
            api.media_player_command(player, volume=original["volume"])
            await wait(lambda: states[player].muted and
                       abs(states[player].volume - original["volume"]) < 0.001, 5, "volume")
            api.media_player_command(player, command=MediaPlayerCommand.UNMUTE)
            await wait(lambda: not states[player].muted, 5, "unmute")
            checks.append("mute_volume_and_routing_preserved")

        async def soak():
            await start_tone_server()
            cycle = [("aac", "KROQ"), ("mp3", args.mp3_station), ("announcement", None),
                     ("superseded_aac", "KROQ"), ("superseded_mp3", args.mp3_station),
                     ("invalid", None), ("stalled", None)]
            transitions = []
            soak_start = time.monotonic()
            soak_index = len(lines)
            n = 0
            while (dev(time.monotonic() - soak_start) < args.soak_minutes * 60
                   or n < args.transitions):
                kind, name = cycle[n % len(cycle)]
                label = f"soak_{n:02d}_{kind}"
                entry = {"n": n, "kind": kind, "t_s": round(dev(time.monotonic() - soak_start), 1)}
                if kind == "announcement":
                    metrics = await announce(label)
                    entry["announcement_bytes"] = metrics["announcement_bytes"]
                elif kind.startswith("superseded"):
                    entry["time_to_pcm_s"] = await superseded_switch(name, 0.2 + 0.3 * (n % 3), label)
                elif kind == "invalid":
                    await invalid_stream_check()
                    entry["invalid_stream"] = dict(report["measurements"]["invalid_stream"])
                elif kind == "stalled":
                    await stalled_startup_check()
                    entry["stalled_startup"] = dict(report["measurements"]["stalled_startup"])
                else:
                    entry["time_to_pcm_s"] = await switch_station(name, label)
                metrics = await window(label, args.dwell)
                report["windows"].pop(label, None)  # keep the soak report compact
                entry.update({k: metrics[k] for k in (
                    "internal_free_min", "internal_free_max", "internal_low_water",
                    "internal_largest_min", "psram_free_min", "radio_loop_max_us", "loop_gap_max_ms",
                    "pcm_rate_settled_min_ratio", "error_lines")})
                transitions.append(entry)
                print("soak", json.dumps(entry), flush=True)
                n += 1
            samples = []
            for index in range(soak_index, len(lines)):
                if match := RUNTIME.search(lines[index]):
                    samples.append((dev(times[index] - soak_start), int(match[1]), int(match[2]), int(match[3])))
            trend = heap_trend(samples)
            # The first cycle exercises every path once; one-time lazy allocations show up
            # there as a step. A leak keeps lowering the ceiling in the later cycles.
            drift = kind_drift(transitions[len(cycle):])
            warmup = warmup_step(transitions, len(cycle))
            # Leaks are timing dependent (the first one may land in the first cycle), so the
            # whole run, PSRAM and the largest internal block are checked too.
            full = {key: kind_drift(transitions, key) for key in
                    ("internal_free_max", "psram_free_min", "internal_largest_min")}
            psram_warmup = warmup_step(transitions, len(cycle), "psram_free_min")
            report["soak"] = {
                "minutes": round(dev(time.monotonic() - soak_start) / 60, 1),
                "transitions": transitions, "heap_trend": trend,
                "same_kind_free_max_drift": drift,
                "warmup_cycle_free_max_step": warmup,
                "warmup_cycle_psram_step": psram_warmup,
                "full_run_drift": full,
                "summary": {k: v for k, v in summarize(lines[soak_index:]).items()
                            if not k.endswith("examples")},
            }
            limits = {"internal_free_max": args.leak_bytes, "psram_free_min": args.psram_leak_bytes,
                      "internal_largest_min": args.largest_drop_bytes}
            problems = [f"{key} {kind} {value}" for key, values in full.items()
                        for kind, value in values.items() if value < -limits[key]]
            problems += [f"internal_free_max first-cycle step {kind} {value}" for kind, value in warmup.items()
                         if value < -args.leak_bytes]
            problems += [f"psram first-cycle step {kind} {value}" for kind, value in psram_warmup.items()
                         if value < -args.psram_leak_bytes]
            if any(value < -args.leak_bytes for value in drift.values()):
                problems.append(f"internal heap drift after first cycle: {drift}")
            if problems:
                raise AssertionError("memory drift across soak: " + "; ".join(problems))
            checks.append("soak_transitions_without_leak_or_fault")

        try:
            report["build"] = provenance()
            await api.connect(login=True)
            info = await api.device_info()
            report["firmware"] = {"version": info.esphome_version, "compiled": info.compilation_time}
            if info.compilation_time != report["build"]["compilation_time"]:
                raise AssertionError("Device firmware build time does not match guarded OTA artifact")
            entities, _ = await api.list_entities_services()
            keys = {x.name: x.key for x in entities}
            player, station, bridge = (keys[x] for x in ("ESP32 Radio", "Station", "BT Speaker"))
            api.subscribe_logs(record, LogLevel.LOG_LEVEL_DEBUG, dump_config=False)

            def state_received(state):
                states[state.key] = state
                log.write(f"{time.monotonic():.3f} STATE {state}\n")
                if state.key == player:
                    player_states.append((time.monotonic(), state.state))
                if state.key == keys["Playback Phase"]:
                    phase_events.append((len(lines), state.state))
            api.subscribe_states(state_received)
            await wait(lambda: all(x in states for x in (player, station, bridge, keys["Playback Phase"])),
                       30, "initial states")
            original = {
                "station": states[station].state, "volume": states[player].volume,
                "muted": states[player].muted, "bridge": states[bridge].state,
                "state": int(states[player].state),
            }
            report["original"] = original

            async def sample_responsiveness():
                while True:
                    start = time.monotonic()
                    await asyncio.wait_for(api.device_info(), 3)
                    latencies.append((time.monotonic() - start) * 1000)
                    await asyncio.sleep(1)
            sampler = asyncio.create_task(sample_responsiveness())
            # Only the mute comparisons need audible radio; otherwise keep the user's mute.
            if original["muted"] and args.scenario in ("announcement", "all"):
                api.media_player_command(player, command=MediaPlayerCommand.UNMUTE)
                await wait(lambda: not states[player].muted, 5, "unmute for tests")
            await ensure_playing("baseline")
            await window("baseline_settled", 8)
            if args.scenario in ("full", "all"):
                await lifecycle_checks()
                await codec_checks()
            if args.scenario == "all":
                await routing_checks()
            if args.scenario in ("announcement", "all"):
                await announcement_checks()
            if args.scenario == "connection":
                await failed_open_check()
                await same_host_failed_open_check(args.same_host_runs)
                await tls_cancel_check(args.tls_cancels)
                await stale_stop_check(args.stale_stop_runs)
                await superseded_run_check(args.superseded_runs)
            if args.scenario == "stale_stop":
                await stale_stop_check(args.stale_stop_runs)
            if args.scenario == "invalid":
                for _ in range(args.invalid_runs):
                    await invalid_stream_check()
            if args.scenario in ("lifecycle", "all"):
                await failed_open_check()
                await superseded_run_check()
                await stalled_startup_check()
                await invalid_stream_check()
            if args.scenario in ("assistant", "all"):
                await assistant_check(paused=False)
                await assistant_check(paused=True)
            if args.scenario == "soak":
                await soak()
            await asyncio.sleep(1.5)
            report["status"] = "passed"
        except Exception as error:
            report["status"] = "failed"
            report["error"] = f"{type(error).__name__}: {error}"
            raise
        finally:
            if sampler:
                sampler.cancel()
                try:
                    await sampler
                except asyncio.CancelledError:
                    pass
                except Exception as error:
                    report["responsiveness_error"] = f"{type(error).__name__}: {error}"
                    report["status"] = "failed"
            if original:
                try:
                    api.select_command(station, original["station"])
                    api.switch_command(bridge, original["bridge"])
                    api.media_player_command(player, volume=original["volume"])
                    api.media_player_command(player, command=MediaPlayerCommand.MUTE if original["muted"]
                                             else MediaPlayerCommand.UNMUTE)
                    await asyncio.sleep(1)
                    if original["state"] == int(MediaPlayerState.IDLE):
                        api.media_player_command(player, command=MediaPlayerCommand.STOP)
                    elif original["state"] == int(MediaPlayerState.PAUSED):
                        if states[player].state != MediaPlayerState.PLAYING:
                            api.media_player_command(player, command=MediaPlayerCommand.PLAY)
                            await wait(lambda: states[player].state == MediaPlayerState.PLAYING, 25,
                                       "playing before restoring pause")
                        api.media_player_command(player, command=MediaPlayerCommand.PAUSE)
                    elif states[player].state != MediaPlayerState.PLAYING:
                        api.media_player_command(player, command=MediaPlayerCommand.PLAY)
                    await asyncio.sleep(8)
                    if states[station].state != original["station"] or states[bridge].state != original["bridge"]:
                        raise AssertionError("Station/BT restoration failed")
                    if states[player].muted != original["muted"] or abs(states[player].volume - original["volume"]) > 0.001:
                        raise AssertionError("Volume/mute restoration failed")
                    await wait(lambda: int(states[player].state) == original["state"], 25, "original state")
                    report["restored"] = str(states.get(player))
                except Exception as error:
                    report["restore_error"] = f"{type(error).__name__}: {error}"
                    report["status"] = "failed"
            checks[:] = dict.fromkeys(checks)  # soak repeats checks
            overall = summarize(lines)
            report["overall"] = overall
            report["api_rtt_max_ms"] = round(max(latencies, default=0), 1) or None
            report["duration_s"] = round(dev(time.monotonic() - started), 1)
            report["clock_device_per_host"] = clock_scale(uptime_samples)
            if problems := final_problems(overall, expected):
                report["unexpected_events"] = problems
                report["status"] = "failed"
            args.report.write_text(json.dumps(report, indent=2) + "\n")
            await api.disconnect()
            if tone_server:
                def shutdown():
                    with urlopen(TONE_URL.format("shutdown"), timeout=5) as response:
                        return response.status
                try:
                    report["tone_shutdown"] = await asyncio.to_thread(shutdown)
                except OSError as error:
                    report["tone_shutdown"] = f"{type(error).__name__}: {error}"
                await tone_server.wait()  # Exits on shutdown, at the latest after its lifetime.
                tone_log.close()
                text = tone_log_path.read_text()
                requests, ends = tone_requests(text)
                report["tone_requests"] = requests
                report["slow_stream_ends"] = ends
                if tone_server.returncode:
                    report["tone_server_error"] = tone_server.returncode
                    report["status"] = "failed"
                ends, problems = keepalive_problems(text)
                report["keepalive_404_ends"] = ends
                if problems:
                    report["keepalive_problems"] = problems
                    report["status"] = "failed"
                if slow_recoveries and (problems := slow_server_problems(text, slow_recoveries)):
                    report["slow_server_problems"] = problems
                    report["status"] = "failed"
                args.report.write_text(json.dumps(report, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="192.168.1.107")
    parser.add_argument("--stage", required=True)
    parser.add_argument("--scenario", default="full",
                        choices=["full", "announcement", "lifecycle", "connection", "stale_stop", "invalid", "assistant", "all",
                                 "soak"])
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--mp3-station", default="Rock Antenne")
    parser.add_argument("--soak-minutes", type=int, default=30)
    parser.add_argument("--transitions", type=int, default=20)
    parser.add_argument("--dwell", type=int, default=30, help="Sustained-PCM seconds after each soak transition")
    parser.add_argument("--leak-bytes", type=int, default=4096)
    parser.add_argument("--psram-leak-bytes", type=int, default=8192,
                        help="A leaked TLS session costs ~22 KB PSRAM")
    parser.add_argument("--largest-drop-bytes", type=int, default=8192)
    parser.add_argument("--superseded-runs", type=int, default=4)
    parser.add_argument("--same-host-runs", type=int, default=5,
                        help="404 announcement + queued same-host announcement repeats")
    parser.add_argument("--stale-stop-runs", type=int, default=3,
                        help="Stops of a queued announcement still opening after a 404 (connection)")
    parser.add_argument("--invalid-runs", type=int, default=6,
                        help="Decoder-failure injections (invalid scenario)")
    parser.add_argument("--tls-cancels", type=int, default=3,
                        help="HTTPS starts that must be cancelled after connect (connection scenario)")
    args = parser.parse_args()
    asyncio.run(run(args))
    if json.loads(args.report.read_text()).get("status") != "passed":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
