#!/usr/bin/env python3
"""Sync uncommitted radio sources and compile in HA's existing ESPHome add-on."""

import argparse
import base64
import hashlib
import io
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
CONFIG_DIR = "/config/esphome"
DEV_CONFIG = f"{CONFIG_DIR}/esp32radio-development.yaml"
DEV_DIR = f"{CONFIG_DIR}/radio-development"


def snapshot(root):
    text = (root / "esp32radio.yaml").read_text()
    text, count = re.subn(
        r"  - source:\n      type: git\n      url: [^\n]+\n      ref: main\n",
        "  - source:\n      type: local\n      path: components\n",
        text,
    )
    if count != 1:
        raise ValueError("Expected one radio Git source; refusing an ambiguous snapshot")
    files = {"esp32radio.yaml": text.encode()}
    for path in sorted((root / "components").rglob("*")):
        if path.is_symlink():
            raise ValueError(f"Refusing source symlink: {path.relative_to(root)}")
        if path.is_file() and "__pycache__" not in path.parts and path.suffix != ".pyc":
            files[path.relative_to(root).as_posix()] = path.read_bytes()
    digest = hashlib.sha256()
    for name, data in sorted(files.items()):
        digest.update(name.encode() + b"\0" + data + b"\0")
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, data in files.items():
            archive.writestr(name, data)
    payload = base64.b64encode(buffer.getvalue())
    if len(payload) > 1024 * 1024:
        raise ValueError("Source snapshot exceeds guest-agent's 1 MiB input limit")
    return digest.hexdigest(), payload


def ssh_json(host, command, payload=None):
    result = subprocess.run(
        ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", host, shlex.join(command)],
        input=payload,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors="replace").strip())
    return json.loads(result.stdout)


def guest(args, command, payload=None):
    qm = ["qm", "guest", "exec", str(args.vm)]
    if payload is not None:
        qm += ["--pass-stdin", "1"]
    result = ssh_json(args.host, qm + ["--", "/usr/bin/docker", "exec"]
                      + (["-i"] if payload is not None else [])
                      + [args.container] + command, payload)
    pid = result.get("pid")
    if not result.get("exited"):
        print(f"Remote guest PID: {pid}", flush=True)
    while not result.get("exited"):
        time.sleep(5)
        result = ssh_json(args.host, ["qm", "guest", "exec-status", str(args.vm), str(pid)])
    if result.get("out-data"):
        print(result["out-data"], end="", flush=True)
    if result.get("err-data") and result.get("exitcode") == 0:
        print(result["err-data"], end="", file=sys.stderr, flush=True)
    if result.get("exitcode") != 0:
        raise RuntimeError(result.get("err-data") or f"Guest command failed: {result.get('exitcode')}")
    if result.get("out-truncated") or result.get("err-truncated"):
        raise RuntimeError("Guest-agent output was truncated; inspect the persistent remote log")


def sync(args):
    digest, payload = snapshot(ROOT)
    # No local secrets, build caches or Git metadata are transferred.
    code = r'''
import base64, hashlib, io, os, pathlib, sys, zipfile
base = pathlib.Path("/config/esphome/radio-development")
digest = sys.argv[1]
base.mkdir(parents=True, exist_ok=True)
import fcntl
lock = (base / "compile.lock").open("w")
fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
archive = zipfile.ZipFile(io.BytesIO(base64.b64decode(sys.stdin.buffer.read(), validate=True)))
files = {}
for name in archive.namelist():
    path = pathlib.PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or not (name == "esp32radio.yaml" or name.startswith("components/")):
        raise RuntimeError("Unsafe source archive path")
    files[name] = archive.read(name)
check = hashlib.sha256()
for name, data in sorted(files.items()):
    check.update(name.encode() + b"\0" + data + b"\0")
if check.hexdigest() != digest:
    raise RuntimeError("Source snapshot checksum mismatch")
target = base / digest
target.mkdir(parents=True, exist_ok=True)
for name, data in files.items():
    path = target / name
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_bytes() != data:
        raise RuntimeError("Existing snapshot differs; refusing to overwrite")
    path.write_bytes(data)
secrets = pathlib.Path("/config/esphome/secrets.yaml")
if not secrets.is_file():
    raise RuntimeError("HA secrets.yaml is missing")
link = target / "secrets.yaml"
if not link.exists():
    link.symlink_to(secrets)
wrapper = pathlib.Path("/config/esphome/esp32radio-development.yaml")
marker = "# Managed by tools/ha_build.py; never installs firmware.\n"
if wrapper.exists() and not wrapper.read_text().startswith(marker):
    raise RuntimeError("Development config already exists and is not managed by this tool")
text = marker + "packages:\n  radio: !include radio-development/" + digest + "/esp32radio.yaml\n"
text += "esphome:\n  compile_process_limit: 1\n  build_path: /data/esp32radio-development-build\n"
temporary = wrapper.with_suffix(".yaml.tmp")
temporary.write_text(text)
os.replace(temporary, wrapper)
(base / "current-sha256").write_text(digest + "\n")
print("Synced source SHA256: " + digest)
print("Development config: " + str(wrapper))
'''
    guest(args, ["python3", "-c", code, digest], payload)
    return digest


def compile_remote(args, digest):
    code = r'''
import json, pathlib, re, shutil, subprocess, sys
base = pathlib.Path("/config/esphome/radio-development")
base.mkdir(parents=True, exist_ok=True)
import fcntl
lock = (base / "compile.lock").open("w")
fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
if (base / "current-sha256").read_text().strip() != sys.argv[1]:
    raise RuntimeError("Remote source changed before compile; rerun")
available = int(next(line.split()[1] for line in pathlib.Path("/proc/meminfo").read_text().splitlines()
                     if line.startswith("MemAvailable:"))) * 1024
if available < 1500 * 1024 * 1024:
    raise RuntimeError("Less than 1.5 GiB available in HA VM; compile deferred")
version = subprocess.check_output(["esphome", "version"], text=True).strip()
print(version, flush=True)
if version != "Version: 2026.10.2":
    raise RuntimeError("Expected ESPHome 2026.10.2; do not upgrade HA automatically")
log = base / "compile.log"
with log.open("w") as output:
    config = "/config/esphome/esp32radio-development.yaml"
    result = subprocess.run(["nice", "-n", "10", "esphome", "compile", "--only-generate", config],
                            stdout=output, stderr=subprocess.STDOUT)
    if result.returncode == 0:
        build = pathlib.Path("/data/esp32radio-development-build")
        sdkconfig = build / "sdkconfig.esp32radio"
        desired = build / "sdkconfig.esp32radio.esphomeinternal"
        actual = set(sdkconfig.read_text().splitlines())
        options = [line for line in desired.read_text().splitlines() if line.startswith("CONFIG_")]
        expected = [("# " + line[:-2] + " is not set") if line.endswith("=n") else line
                    for line in options]
        # The first IDF configure can discard GMF options before dependencies exist.
        if any(line not in actual for line in expected):
            shutil.copyfile(desired, sdkconfig)
            print("Reapplying configured SDK options before native rebuild.", flush=True)
        result = subprocess.run(["nice", "-n", "10", "esphome", "compile", config],
                                stdout=output, stderr=subprocess.STDOUT)
print("\n".join(log.read_text(errors="replace").splitlines()[-45:]))
if result.returncode:
    sys.exit(result.returncode)
artifacts = list(pathlib.Path("/data/esp32radio-development-build").rglob("firmware.ota.bin"))
if len(artifacts) != 1:
    raise RuntimeError("Expected exactly one OTA artifact")
artifact = artifacts[0]
import hashlib
print("OTA artifact: " + str(artifact))
print("Artifact bytes: " + str(artifact.stat().st_size))
print("Artifact SHA256: " + hashlib.sha256(artifact.read_bytes()).hexdigest())
(base / "built-artifact-sha256").write_text(hashlib.sha256(artifact.read_bytes()).hexdigest() + "\n")
(base / "built-sha256").write_text(sys.argv[1] + "\n")
time_match = re.search(r"build_time_str=([^\n]+)", log.read_text())
if not time_match:
    raise RuntimeError("Missing firmware build-time identity")
(base / "built-info.json").write_text(json.dumps({
    "source_sha256": sys.argv[1],
    "artifact_sha256": hashlib.sha256(artifact.read_bytes()).hexdigest(),
    "compilation_time": time_match[1].strip(),
    "esphome": "2026.10.2",
    "idf": "6.0.1",
}))
'''
    guest(args, ["python3", "-c", code, digest])


def verify_remote(args):
    digest, _ = snapshot(ROOT)
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as archive:
        for name in ("test_config.py", "test_generated.py"):
            archive.writestr(name, (ROOT / "tests" / name).read_bytes())
    code = r'''
import base64, fcntl, io, os, pathlib, subprocess, sys, zipfile
base = pathlib.Path("/config/esphome/radio-development")
lock = (base / "compile.lock").open("w")
fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
for marker in ("current-sha256", "built-sha256"):
    if (base / marker).read_text().strip() != sys.argv[1]:
        raise RuntimeError("Build does not match current local sources; compile first")
build = pathlib.Path("/data/esp32radio-development-build")
for name in ("src/main.cpp", "build/esp32radio.elf", "build/esp32radio.bin", "sdkconfig.esp32radio"):
    if not (build / name).is_file():
        raise RuntimeError("Missing build artifact: " + name)
os.environ["ESP32RADIO_TEST_BUILD_DIR"] = str(build)
directory = base / "verification-tests"
directory.mkdir(exist_ok=True)
try:
    archive = zipfile.ZipFile(io.BytesIO(base64.b64decode(sys.stdin.buffer.read(), validate=True)))
    for name in ("test_config.py", "test_generated.py"):
        (pathlib.Path(directory) / name).write_bytes(archive.read(name))
    runner = ("import unittest; "
              "suite=unittest.defaultTestLoader.loadTestsFromName('test_generated'); "
              "result=unittest.TextTestRunner(verbosity=2).run(suite); "
              "raise SystemExit(0 if result.wasSuccessful() and not result.skipped else 1)")
    result = subprocess.run([sys.executable, "-c", runner], cwd=directory)
    sys.exit(result.returncode)
finally:
    for name in ("test_config.py", "test_generated.py"):
        (directory / name).unlink(missing_ok=True)
'''
    guest(args, ["python3", "-c", code, digest],
          base64.b64encode(buffer.getvalue()))


def upload_remote(args):
    digest, _ = snapshot(ROOT)
    code = r'''
import fcntl, hashlib, pathlib, subprocess, sys
base = pathlib.Path("/config/esphome/radio-development")
lock = (base / "compile.lock").open("w")
fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
for name in ("current-sha256", "built-sha256"):
    if (base / name).read_text().strip() != sys.argv[1]:
        raise RuntimeError("Source/build mismatch; refusing OTA")
artifact = pathlib.Path("/data/esp32radio-development-build/build/firmware.ota.bin")
if hashlib.sha256(artifact.read_bytes()).hexdigest() != (base / "built-artifact-sha256").read_text().strip():
    raise RuntimeError("Artifact changed after compilation; refusing OTA")
print("Uploading verified source SHA256: " + sys.argv[1], flush=True)
log = base / "upload.log"
with log.open("w") as output:
    result = subprocess.run(["esphome", "upload", "/config/esphome/esp32radio-development.yaml",
                             "--device", sys.argv[2]], stdout=output, stderr=subprocess.STDOUT)
print("\n".join(log.read_text(errors="replace").splitlines()[-12:]))
if result.returncode == 0:
    (base / "uploaded-sha256").write_text(sys.argv[1] + "\n")
    (base / "uploaded-artifact-sha256").write_text(hashlib.sha256(artifact.read_bytes()).hexdigest() + "\n")
sys.exit(result.returncode)
'''
    guest(args, ["python3", "-c", code, digest, args.device])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["sync", "compile", "verify", "status", "upload"])
    parser.add_argument("--device", default="192.168.1.107")
    parser.add_argument("--host", default="proxmox")
    parser.add_argument("--vm", type=int, default=100)
    parser.add_argument("--container", default="app_5c53de3b_esphome")
    args = parser.parse_args()
    try:
        if args.action == "sync":
            sync(args)
        elif args.action == "compile":
            digest = sync(args)
            compile_remote(args, digest)
        elif args.action == "verify":
            verify_remote(args)
        elif args.action == "upload":
            upload_remote(args)
        else:
            guest(args, ["python3", "-c",
                         f"from pathlib import Path; p=Path({DEV_DIR!r}); "
                         "print('Current source: ' + (p/'current-sha256').read_text().strip()); "
                         "b=p/'built-sha256'; print('Last successful source: ' + "
                         "(b.read_text().strip() if b.exists() else 'not built')); "
                         "log=p/'compile.log'; "
                         "print('\\n'.join(log.read_text(errors='replace').splitlines()[-45:]) "
                         "if log.exists() else 'No compile log yet')"])
    except (RuntimeError, ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"Remote build failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
