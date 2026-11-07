import base64
import importlib.util
import io
from pathlib import Path
import contextlib
import shutil
import uuid
import unittest
from unittest.mock import patch
from types import SimpleNamespace
import zipfile

spec = importlib.util.spec_from_file_location(
    "ha_build", Path(__file__).resolve().parents[1] / "tools" / "ha_build.py"
)
ha_build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ha_build)


@contextlib.contextmanager
def fixture_directory():
    path = ha_build.ROOT / ".esphome" / "ha-build-tests" / uuid.uuid4().hex
    path.mkdir(parents=True)
    try:
        yield str(path)
    finally:
        shutil.rmtree(path)


class SnapshotTests(unittest.TestCase):
    def test_snapshot_uses_current_sources_and_excludes_secrets(self):
        with fixture_directory() as directory:
            root = Path(directory)
            (root / "esp32radio.yaml").write_text(
                "external_components:\n  - source:\n      type: git\n"
                "      url: https://github.com/example/radio.git\n      ref: main\n"
            )
            (root / "secrets.yaml").write_text("do-not-transfer")
            (root / "components").mkdir()
            component = root / "components" / "radio.cpp"
            component.write_text("uncommitted")
            digest, payload = ha_build.snapshot(root)
            with zipfile.ZipFile(io.BytesIO(base64.b64decode(payload))) as archive:
                self.assertEqual(set(archive.namelist()), {"esp32radio.yaml", "components/radio.cpp"})
                self.assertIn(b"type: local", archive.read("esp32radio.yaml"))
                self.assertEqual(archive.read("components/radio.cpp"), b"uncommitted")
            self.assertEqual(digest, ha_build.snapshot(root)[0])
            component.write_text("new uncommitted change")
            self.assertNotEqual(digest, ha_build.snapshot(root)[0])

    def test_unknown_source_layout_rejected(self):
        with fixture_directory() as directory:
            root = Path(directory)
            (root / "esp32radio.yaml").write_text("external_components: []")
            with self.assertRaisesRegex(ValueError, "ambiguous"):
                ha_build.snapshot(root)

    def test_source_symlink_rejected(self):
        with fixture_directory() as directory:
            root = Path(directory)
            (root / "esp32radio.yaml").write_text(
                "  - source:\n      type: git\n      url: https://example.com\n      ref: main\n"
            )
            (root / "components").mkdir()
            (root / "components" / "secret").symlink_to(root / "esp32radio.yaml")
            with self.assertRaisesRegex(ValueError, "symlink"):
                ha_build.snapshot(root)


class GuestTests(unittest.TestCase):
    def test_verification_sends_only_tests_for_current_source(self):
        args = SimpleNamespace(host="proxmox", vm=100, container="esphome")
        with patch.object(ha_build, "guest") as remote:
            ha_build.verify_remote(args)
        command, payload = remote.call_args.args[1:]
        self.assertEqual(command[:2], ["python3", "-c"])
        self.assertEqual(command[-1], ha_build.snapshot(ha_build.ROOT)[0])
        compile(command[2], "<remote verification>", "exec")
        with zipfile.ZipFile(io.BytesIO(base64.b64decode(payload))) as archive:
            self.assertEqual(set(archive.namelist()), {"test_config.py", "test_generated.py"})
        self.assertIn("result.wasSuccessful() and not result.skipped", command[2])

    def test_verification_refuses_stale_build(self):
        args = SimpleNamespace(host="proxmox", vm=100, container="esphome")
        with patch.object(ha_build, "guest") as remote:
            ha_build.verify_remote(args)
        code = remote.call_args.args[1][2]
        with fixture_directory() as directory:
            base = Path(directory)
            (base / "current-sha256").write_text("current")
            (base / "built-sha256").write_text("stale")
            code = code.replace("/config/esphome/radio-development", directory)
            namespace = {}
            with patch("sys.argv", ["verify", "current"]):
                try:
                    with self.assertRaisesRegex(RuntimeError, "compile first"):
                        exec(compile(code, "<remote verification>", "exec"), namespace)
                finally:
                    namespace["lock"].close()

    def test_guest_polling_retains_original_pid(self):
        args = SimpleNamespace(host="proxmox", vm=100, container="esphome")
        with patch.object(ha_build, "ssh_json", side_effect=[
            {"pid": 123},
            {"exited": 0},
            {"exited": 1, "exitcode": 0},
        ]) as remote, patch.object(ha_build.time, "sleep"):
            ha_build.guest(args, ["esphome", "version"])
        for call in remote.call_args_list[1:]:
            self.assertEqual(call.args[1], ["qm", "guest", "exec-status", "100", "123"])

    def test_guest_failure_is_reported(self):
        args = SimpleNamespace(host="proxmox", vm=100, container="esphome")
        with patch.object(ha_build, "ssh_json", return_value={
            "exited": 1, "exitcode": 1, "err-data": "compile failed",
        }):
            with self.assertRaisesRegex(RuntimeError, "compile failed"):
                ha_build.guest(args, ["esphome", "compile"])

    def test_upload_is_guarded_by_source_and_artifact_digest(self):
        args = SimpleNamespace(host="proxmox", vm=100, container="esphome", device="192.168.1.107")
        with patch.object(ha_build, "guest") as remote:
            ha_build.upload_remote(args)
        command = remote.call_args.args[1]
        self.assertEqual(command[-2:], [ha_build.snapshot(ha_build.ROOT)[0], args.device])
        self.assertIn("built-artifact-sha256", command[2])
        self.assertIn("Source/build mismatch; refusing OTA", command[2])
        self.assertIn("Artifact changed after compilation; refusing OTA", command[2])
        compile(command[2], "<guarded upload>", "exec")
