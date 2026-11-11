#!/usr/bin/env python3
"""Compile the native audio probe in HA, without upgrading the add-on or flashing."""

import argparse
import base64
import hashlib
import json
import subprocess
import sys

from ha_build import ROOT, guest

BASE = "/config/esphome/radio-native-beta"


def snapshot(root, bluetooth=False, voice=False):
    files = {
        "native-audio.yaml": (root / "experiments/native-audio.yaml").read_text().replace(
            "path: ../components", "path: components"
        ),
        "requirements.txt": (root / "tools/requirements-native-beta.txt").read_text(),
    }
    if bluetooth:
        files["native-audio-base.yaml"] = files["native-audio.yaml"]
        files["native-audio.yaml"] = (root / "experiments/native-audio-bt.yaml").read_text().replace(
            "!include native-audio.yaml", "!include native-audio-base.yaml"
        )
    if voice:
        files["native-audio-routing.yaml"] = files["native-audio.yaml"]
        files["native-audio.yaml"] = (root / "experiments/native-audio-voice.yaml").read_text().replace(
            "!include native-audio.yaml", "!include native-audio-routing.yaml"
        )
    for component in ("audio_gain", "gmf_audio", "audio_spectrum"):
        for path in sorted((root / "components" / component).rglob("*")):
            if path.is_symlink():
                raise ValueError(f"Refusing source symlink: {path.relative_to(root)}")
            if path.is_file() and "__pycache__" not in path.parts and path.suffix != ".pyc":
                files[path.relative_to(root).as_posix()] = path.read_text()
    digest = hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()
    return digest, base64.b64encode(json.dumps(files).encode())


def compile_remote(args):
    digest, payload = snapshot(ROOT, bluetooth=getattr(args, "bluetooth", False),
                               voice=getattr(args, "voice", False))
    code = r'''
import base64, fcntl, hashlib, json, pathlib, re, shutil, subprocess, sys
base = pathlib.Path("/config/esphome/radio-native-beta")
data = pathlib.Path("/data/radio-native-beta")
base.mkdir(parents=True, exist_ok=True)
data.mkdir(parents=True, exist_ok=True)
# Share the stable build lock: two native compiles would exhaust this VM.
stable = pathlib.Path("/config/esphome/radio-development")
stable.mkdir(parents=True, exist_ok=True)
lock = (stable / "compile.lock").open("w")
fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
version = subprocess.check_output(["esphome", "version"], text=True).strip()
if version != "Version: 2026.10.2":
    raise RuntimeError("Stable add-on version changed; expected 2026.10.2")
available = int(next(line.split()[1] for line in pathlib.Path("/proc/meminfo").read_text().splitlines()
                     if line.startswith("MemAvailable:"))) * 1024
if available < 1500 * 1024 * 1024:
    raise RuntimeError("Less than 1.5 GiB available in HA VM; compile deferred")
if shutil.disk_usage(data).free < 6 * 1024**3:
    raise RuntimeError("Less than 6 GiB disk space available; compile deferred")
files = json.loads(base64.b64decode(sys.stdin.buffer.read(), validate=True))
required = {"native-audio.yaml", "requirements.txt"}
if not required <= files.keys():
    raise RuntimeError("Unexpected probe snapshot files")
for name in files:
    path = pathlib.PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or (
            name not in required | {"native-audio-base.yaml", "native-audio-routing.yaml"} and not name.startswith((
                "components/audio_gain/", "components/gmf_audio/", "components/audio_spectrum/"))):
        raise RuntimeError("Unsafe probe snapshot path")
digest = hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()
if digest != sys.argv[1]:
    raise RuntimeError("Probe snapshot checksum mismatch")
target = base / digest
target.mkdir(exist_ok=True)
for name, text in files.items():
    path = target / name
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text() != text:
        raise RuntimeError("Existing probe snapshot differs")
    path.write_text(text)
secrets = pathlib.Path("/config/esphome/secrets.yaml")
if not secrets.is_file():
    raise RuntimeError("HA secrets.yaml is missing")
link = target / "secrets.yaml"
if not link.exists():
    link.symlink_to(secrets)
config = base / "probe.yaml"
config.write_text("packages:\n  probe: !include " + digest + "/native-audio.yaml\n"
                  "esphome:\n  compile_process_limit: 1\n"
                  "  build_path: /data/radio-native-beta/build\n")
(base / "current-sha256").write_text(digest + "\n")
python = data / "venv/bin/python"
esphome = data / "venv/bin/esphome"
log = base / "compile.log"
try:
    with log.open("w") as output:
        def run(command):
            subprocess.run(command, stdout=output, stderr=subprocess.STDOUT, check=True)
        if not python.is_file():
            run(["uv", "venv", "--python", sys.executable, str(data / "venv")])
        run(["uv", "pip", "install", "--python", str(python), "-r", str(target / "requirements.txt")])
        beta_version = subprocess.check_output([str(esphome), "version"], text=True).strip()
        if beta_version != "Version: 2026.11.0b2":
            raise RuntimeError("Expected isolated ESPHome 2026.11.0b2")
        print(beta_version + " (isolated; stable add-on unchanged)", flush=True)
        run(["nice", "-n", "10", str(esphome), "compile", "--only-generate", str(config)])
        build = data / "build"
        desired = build / "sdkconfig.radio-native-beta-probe.esphomeinternal"
        actual = build / "sdkconfig.radio-native-beta-probe"
        if actual.exists():
            lines = set(actual.read_text().splitlines())
            expected = [("# " + line[:-2] + " is not set") if line.endswith("=n") else line
                        for line in desired.read_text().splitlines() if line.startswith("CONFIG_")]
            if any(line not in lines for line in expected):
                shutil.copyfile(desired, actual)
        run(["nice", "-n", "10", str(esphome), "compile", str(config)])
    build = data / "build"
    artifacts = {}
    for name in ("firmware.ota.bin", "radio-native-beta-probe.elf"):
        artifact = build / "build" / name
        if not artifact.is_file() or artifact.stat().st_size == 0:
            raise RuntimeError("Missing or empty linked artifact: " + name)
        artifacts[name] = {
            "path": str(artifact),
            "bytes": artifact.stat().st_size,
            "sha256": hashlib.sha256(artifact.read_bytes()).hexdigest(),
        }
    generated = (build / "src/main.cpp").read_text()
    defines = (build / "src/esphome/core/defines.h").read_text()
    sdkconfig = (build / "build/config/sdkconfig.h").read_text()
    for accelerator in ("SHA", "AES"):
        if f"#define CONFIG_MBEDTLS_HARDWARE_{accelerator} 1" in sdkconfig:
            raise RuntimeError(f"Probe linked with known-failing hardware {accelerator} enabled")
    if ("#define USE_I2S_AUDIO_FULL_DUPLEX" not in defines or
            "#define USE_GMF_MEDIA_SOURCE" not in defines or
            "duplex->set_audio_in(native_mic)" not in generated or
            "duplex->set_audio_out(native_speaker)" not in generated):
        raise RuntimeError("Generated code did not enable native full duplex")
    bluetooth = "native-audio-base.yaml" in files
    voice = "native-audio-routing.yaml" in files
    if bluetooth and ("bridge_bus->set_port(1)" not in generated or
                      "radio_gain->set_tap_speaker(bridge_resampler)" not in generated):
        raise RuntimeError("Generated code did not enable isolated I2S1 routing")
    if voice and ("#define USE_VOICE_ASSISTANT" not in defines or
                  "#define USE_MICRO_WAKE_WORD" not in defines or
                  "probe_assistant->set_media_player(probe_player)" not in generated):
        raise RuntimeError("Generated code did not enable native wake/assistant")
    nm = next(pathlib.Path("/root/.cache/esphome/idf/tools/xtensa-esp-elf").glob(
        "*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm"))
    symbols = subprocess.check_output(
        [str(nm), "-C", str(build / "build/radio-native-beta-probe.elf")], text=True)
    for symbol in ("media_lib_module_malloc", "media_lib_module_calloc", "media_lib_module_realloc"):
        if not re.search(r"^[0-9a-f]+ T " + symbol + r"$", symbols, re.MULTILINE):
            raise RuntimeError("Codec PSRAM override is not linked strongly: " + symbol)
    if "esphome::gmf_audio::GmfMediaSource::pcm_(" not in symbols:
        raise RuntimeError("Native GMF PCM adapter is missing from the linked ELF")
    if not re.search(r"\bT feed_fft_samples\(unsigned char const\*, int, int\)", symbols):
        raise RuntimeError("Radio PCM spectrum tap is missing from the linked ELF")
    if subprocess.check_output(["esphome", "version"], text=True).strip() != version:
        raise RuntimeError("Stable add-on version changed during beta compile")
    compilation = re.search(r"build_time_str=([^\n]+)", log.read_text(errors="replace"))
    if not compilation:
        raise RuntimeError("Missing probe firmware build-time identity")
    info = {"source_sha256": digest, "compilation_time": compilation[1].strip(),
            "esphome": "2026.11.0b2", "idf": "6.0.1",
            "artifacts": artifacts, "bluetooth": bluetooth, "voice": voice,
            "hardware_validated": False}
    (base / "built-info.json").write_text(json.dumps(info, indent=2) + "\n")
    (base / "built-sha256").write_text(digest + "\n")
    print(json.dumps(info, indent=2), flush=True)
finally:
    if log.exists():
        print("\n".join(log.read_text(errors="replace").splitlines()[-60:]), flush=True)
'''
    guest(args, ["python3", "-c", code, digest], payload)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["compile", "status"])
    parser.add_argument("--host", default="proxmox")
    parser.add_argument("--vm", type=int, default=100)
    parser.add_argument("--container", default="app_5c53de3b_esphome")
    parser.add_argument("--bluetooth", action="store_true", help="Build the isolated experimental I2S1 variant")
    parser.add_argument("--voice", action="store_true", help="Add native wake-word and assistant experiment")
    args = parser.parse_args()
    try:
        if args.action == "compile":
            compile_remote(args)
        else:
            guest(args, ["python3", "-c",
                         f"from pathlib import Path; p=Path({BASE!r}); "
                         "info=p/'built-info.json'; "
                         "print(info.read_text() if info.exists() else 'No successful beta build'); "
                         "current=p/'current-sha256'; "
                         "print('Current source: ' + (current.read_text().strip() "
                         "if current.exists() else 'not synced')); "
                         "log=p/'compile.log'; "
                         "print('\\n'.join(log.read_text(errors='replace').splitlines()[-60:]) "
                         "if log.exists() else 'No beta build log')"])
    except (RuntimeError, ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"Native probe build failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
