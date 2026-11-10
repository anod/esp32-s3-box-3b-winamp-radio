"""Probe schema/codegen tests; set ESPHOME_NATIVE_BETA to the isolated beta binary."""

import base64
import contextlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
ESPHOME = os.environ.get("ESPHOME_NATIVE_BETA")
CONFIG = ROOT / "experiments/native-audio.yaml"

with patch.object(sys, "path", [str(ROOT / "tools"), *sys.path]):
    spec = importlib.util.spec_from_file_location("ha_native_build", ROOT / "tools/ha_native_build.py")
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)


@contextlib.contextmanager
def probe_config(text, base_text=None, extra_files=None):
    base = ROOT / ".esphome" / "native-audio-tests"
    base.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=base) as directory:
        path = Path(directory) / "probe.yaml"
        def local_config(source):
            return source.replace(
                "  min_version: 2026.11.0b2",
                f"  min_version: 2026.11.0b2\n  build_path: {directory}/build",
            ).replace("path: ../components", f"path: {ROOT / 'components'}")
        path.write_text(local_config(text))
        if base_text is not None:
            (path.parent / "native-audio.yaml").write_text(local_config(base_text))
        for name, content in (extra_files or {}).items():
            (path.parent / name).write_text(local_config(content))
        (path.parent / "secrets.yaml").write_text(
            'wifi_ssid: test\nwifi_password: test-password\n'
            f'esp32radio_api_key: "{base64.b64encode(bytes(range(32))).decode()}"\n'
            'esp32radio_ota_password: test-password\n'
        )
        yield path


@unittest.skipUnless(ESPHOME, "set ESPHOME_NATIVE_BETA to an isolated 2026.11.0b2 binary")
class NativeAudioConfigTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        version = subprocess.check_output([ESPHOME, "version"], text=True).strip()
        if version != "Version: 2026.11.0b2":
            raise RuntimeError(f"Expected isolated 2026.11.0b2, got {version}")
        cls.text = CONFIG.read_text()

    def validate(self, text):
        with probe_config(text) as path:
            result = subprocess.run(
                [ESPHOME, "config", str(path)], capture_output=True, text=True,
                env={**os.environ, "NO_COLOR": "1"}, timeout=60,
            )
        return result.returncode, result.stdout + result.stderr

    def test_graph_accepted(self):
        code, output = self.validate(self.text)
        self.assertEqual(code, 0, output)

    def test_duplex_rejects_mismatched_mic_sample_rate(self):
        old = "    i2s_din_pin: 16\n    adc_type: external\n    sample_rate: 48000"
        self.assertEqual(self.text.count(old), 1)
        code, output = self.validate(self.text.replace(old, old.replace("48000", "16000")))
        self.assertNotEqual(code, 0)
        self.assertIn("sample_rate", output)

    def test_rejects_shared_source_across_pipelines(self):
        code, output = self.validate(self.text.replace(
            "sources: [announcement_http]", "sources: [media_http]",
        ))
        self.assertNotEqual(code, 0)
        self.assertIn("cannot be shared between pipelines", output)

    def test_keep_alive_not_supported_on_shared_bus(self):
        old = "    i2s_dout_pin: 15\n    dac_type: external"
        self.assertEqual(self.text.count(old), 1)
        code, output = self.validate(self.text.replace(old, old + "\n    keep_alive: true"))
        self.assertNotEqual(code, 0)
        self.assertIn("keep_alive", output)

    def test_gain_cannot_own_shared_dac(self):
        code, output = self.validate(self.text.replace(
            "    id: radio_gain\n", "    id: radio_gain\n    audio_dac: codec_out\n",
        ))
        self.assertNotEqual(code, 0)
        self.assertIn("shared DAC belongs only", output)

    def test_fixed_gain_above_unity_rejected(self):
        code, output = self.validate(self.text.replace("fixed_gain: 31.640625%", "fixed_gain: 120%"))
        self.assertNotEqual(code, 0)
        self.assertIn("fixed_gain", output)

    def test_fixed_voice_cannot_be_tapped_to_bluetooth(self):
        code, output = self.validate(self.text.replace(
            "    id: voice_gain\n", "    id: voice_gain\n    tap_speaker: radio_resampler\n",
        ))
        self.assertNotEqual(code, 0)
        self.assertIn("voice output must not", output)

    def test_generated_graph_has_both_resamplers_and_duplex(self):
        with probe_config(self.text) as path:
            result = subprocess.run(
                [ESPHOME, "compile", "--only-generate", str(path)],
                capture_output=True, text=True, timeout=120,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            main = (path.parent / "build/src/main.cpp").read_text()
            defines = (path.parent / "build/src/esphome/core/defines.h").read_text()
            sdkconfig = (path.parent / "build/sdkconfig.radio-native-beta-probe.esphomeinternal").read_text()
        for expected in (
            "duplex->set_audio_in(native_mic)",
            "duplex->set_audio_out(native_speaker)",
            "mic_16k->set_target_sample_rate(16000)",
            "radio_resampler->set_output_speaker(radio_stream)",
            "voice_resampler->set_output_speaker(voice_stream)",
            "radio_gain->set_output_speaker(radio_resampler)",
            "voice_gain->set_output_speaker(voice_resampler)",
            "voice_gain->set_fixed_gain(0.31640625",
            "new(media_http) gmf_audio::GmfMediaSource()",
            "new(announcement_http) gmf_audio::GmfMediaSource()",
            "add_audio_output_callback",
        ):
            self.assertTrue(expected in main, f"Missing generated statement: {expected}")
        self.assertIn("#define USE_I2S_AUDIO_FULL_DUPLEX", defines)
        self.assertIn("#define USE_GMF_MEDIA_SOURCE", defines)
        self.assertIn("CONFIG_MBEDTLS_HARDWARE_SHA=n", sdkconfig)
        self.assertNotIn("audio_http::AudioHttpSource", main)
        self.assertNotIn("internet_radio", main)
        self.assertNotIn("box3_mic", main)

    def test_isolated_bluetooth_variant_codegen(self):
        text = (ROOT / "experiments/native-audio-bt.yaml").read_text()
        with probe_config(text, self.text) as path:
            result = subprocess.run(
                [ESPHOME, "compile", "--only-generate", str(path)],
                capture_output=True, text=True, timeout=120,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            main = (path.parent / "build/src/main.cpp").read_text()
        for expected in (
            "radio_gain->set_tap_speaker(bridge_resampler)",
            "bridge_resampler->set_output_speaker(bridge_speaker)",
            "bridge_speaker->set_parent(bridge_bus)",
            "bridge_bus->set_port(1)",
        ):
            self.assertIn(expected, main)
        self.assertNotIn("voice_gain->set_tap_speaker", main)

    def test_native_voice_codegen(self):
        self.check_voice_codegen(bluetooth=False)

    def test_native_voice_with_bluetooth_codegen(self):
        self.check_voice_codegen(bluetooth=True)

    def check_voice_codegen(self, bluetooth):
        _, payload = builder.snapshot(ROOT, bluetooth=bluetooth, voice=True)
        files = json.loads(base64.b64decode(payload))
        with probe_config(files["native-audio.yaml"], extra_files={
            name: content.replace("path: components", f"path: {ROOT / 'components'}")
            for name, content in files.items()
            if name in ("native-audio-base.yaml", "native-audio-routing.yaml")
        }) as path:
            result = subprocess.run(
                [ESPHOME, "compile", "--only-generate", str(path)],
                capture_output=True, text=True, timeout=120,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            main = (path.parent / "build/src/main.cpp").read_text()
        for expected in (
            "probe_assistant->set_media_player(probe_player)",
            "probe_assistant->set_microphone_source(",
            "probe_wake->set_microphone_source(",
            "microphone::MicrophoneSource(mic_16k, 16, 4, false)",
            "id(radio_gain).set_ducked(true)",
            "id(radio_gain).set_ducked(false)",
            "Session end timed out",
            "id(voice_gain).has_buffered_data()",
        ):
            self.assertIn(expected, main)
        self.assertEqual(main.count("microphone::MicrophoneSource(mic_16k, 16, 4, false)"), 2)
        self.assertEqual("bridge_bus->set_port(1)" in main, bluetooth)
        self.assertNotIn("internet_radio::", main)
        self.assertNotIn("box3_mic::", main)


class NativeBuildTests(unittest.TestCase):
    def test_remote_voice_build_uses_requested_variant_and_linked_gates(self):
        args = SimpleNamespace(host="proxmox", vm=100, container="esphome",
                               bluetooth=True, voice=True)
        with patch.object(builder, "guest") as remote:
            builder.compile_remote(args)
        command, payload = remote.call_args.args[1:]
        self.assertEqual((command[-1], payload), builder.snapshot(ROOT, bluetooth=True, voice=True))
        code = command[2]
        self.assertIn("Generated code did not enable native wake/assistant", code)
        self.assertIn("Generated code did not enable isolated I2S1 routing", code)
        compile(code, "<remote beta voice build>", "exec")

    def test_voice_snapshots_preserve_optional_routing(self):
        for bluetooth in (False, True):
            with self.subTest(bluetooth=bluetooth):
                digest, payload = builder.snapshot(ROOT, bluetooth=bluetooth, voice=True)
                files = json.loads(base64.b64decode(payload))
                self.assertIn("!include native-audio-routing.yaml", files["native-audio.yaml"])
                self.assertIn("native-audio-routing.yaml", files)
                self.assertEqual("native-audio-base.yaml" in files, bluetooth)
                self.assertNotEqual(digest, builder.snapshot(ROOT, bluetooth=bluetooth)[0])

    def test_bluetooth_variant_has_distinct_snapshot(self):
        digest, payload = builder.snapshot(ROOT, bluetooth=True)
        files = json.loads(base64.b64decode(payload))
        self.assertIn("native-audio-base.yaml", files)
        self.assertIn("!include native-audio-base.yaml", files["native-audio.yaml"])
        self.assertNotEqual(digest, builder.snapshot(ROOT)[0])

    def test_snapshot_is_reproducible_and_contains_no_secrets(self):
        digest, payload = builder.snapshot(ROOT)
        files = json.loads(base64.b64decode(payload))
        self.assertTrue({"native-audio.yaml", "requirements.txt"} <= files.keys())
        self.assertIn("components/audio_gain/speaker/audio_gain.cpp", files)
        self.assertIn("components/gmf_audio/gmf_media_source.cpp", files)
        self.assertIn("components/audio_spectrum/audio_spectrum.cpp", files)
        self.assertEqual(files["requirements.txt"], "esphome==2026.11.0b2\n")
        self.assertEqual((digest, payload), builder.snapshot(ROOT))
        self.assertIn("!secret esp32radio_api_key", files["native-audio.yaml"])

    def test_snapshot_digest_covers_config_and_requirements(self):
        (ROOT / ".esphome").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=ROOT / ".esphome") as directory:
            root = Path(directory)
            (root / "experiments").mkdir()
            (root / "tools").mkdir()
            shutil.copytree(ROOT / "components/audio_gain", root / "components/audio_gain",
                            ignore=shutil.ignore_patterns("__pycache__"))
            shutil.copytree(ROOT / "components/gmf_audio", root / "components/gmf_audio",
                            ignore=shutil.ignore_patterns("__pycache__"))
            shutil.copytree(ROOT / "components/audio_spectrum", root / "components/audio_spectrum",
                            ignore=shutil.ignore_patterns("__pycache__"))
            shutil.copyfile(CONFIG, root / "experiments/native-audio.yaml")
            requirement = root / "tools/requirements-native-beta.txt"
            requirement.write_text("esphome==2026.11.0b2\n")
            before, _ = builder.snapshot(root)
            requirement.write_text("esphome==2026.11.0b3\n")
            after, _ = builder.snapshot(root)
            self.assertNotEqual(before, after)
            config = root / "experiments/native-audio.yaml"
            config.write_text(config.read_text() + "\n# changed\n")
            self.assertNotEqual(after, builder.snapshot(root)[0])

    def test_remote_build_is_isolated_and_never_uploads(self):
        args = SimpleNamespace(host="proxmox", vm=100, container="esphome")
        with patch.object(builder, "guest") as remote:
            builder.compile_remote(args)
        command, payload = remote.call_args.args[1:]
        self.assertEqual(command[-1], builder.snapshot(ROOT)[0])
        self.assertEqual(payload, builder.snapshot(ROOT)[1])
        code = command[2]
        compile(code, "<remote beta build>", "exec")
        self.assertIn('/data/radio-native-beta', code)
        self.assertIn('stable / "compile.lock"', code)
        self.assertIn('"uv", "venv"', code)
        self.assertIn('"--python", str(python)', code)
        self.assertIn("Stable add-on version changed during beta compile", code)
        self.assertIn("Probe linked with known-failing hardware SHA enabled", code)
        self.assertIn('"hardware_validated": False', code)
        self.assertNotIn('"upload"', code)
        self.assertNotIn('"run"', code)


if __name__ == "__main__":
    unittest.main()
