"""Config-validation tests: run `esphome config` against the real YAML (with
local components) and against deliberately broken variants.

Run: python3 -m unittest discover -s tests -v   (needs esphome + secrets.yaml)
"""
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAIN_YAML = os.path.join(ROOT, "esp32radio.yaml")


def local_yaml(text):
    text = re.sub(
        r"  - source:\n      type: git\n.*?ref: main\n",
        "  - source:\n      type: local\n      path: components\n",
        text,
        flags=re.S,
    )
    return text


def run_config(text):
    path = os.path.join(ROOT, "_tmp_test_config.yaml")
    with open(path, "w") as f:
        f.write(local_yaml(text))
    try:
        p = subprocess.run(
            ["esphome", "config", path], cwd=ROOT, capture_output=True, text=True
        )
        return p.returncode, p.stdout + p.stderr
    finally:
        os.remove(path)


@unittest.skipUnless(shutil.which("esphome"), "esphome not installed")
class ConfigTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(MAIN_YAML) as f:
            cls.base = f.read()

    def test_main_yaml_valid(self):
        rc, out = run_config(self.base)
        self.assertEqual(rc, 0, out[-2000:])

    def test_voice_pieces_present(self):
        for needle in ("platform: box3_mic", "voice_assistant:", "platform: es7210",
                       "i2s_din_pin: 16", "Voice Assistant State", "Voice Heard",
                       "Voice Reply", "timeout: 30s", "delay: 300ms"):
            self.assertIn(needle, self.base)

    def test_bad_channel_rejected(self):
        rc, out = run_config(self.base.replace("channel: left", "channel: middle"))
        self.assertNotEqual(rc, 0)
        self.assertIn("channel", out)

    def test_mic_requires_radio_id(self):
        rc, out = run_config(self.base.replace("    radio_id: radio\n    channel", "    channel"))
        self.assertNotEqual(rc, 0)
        self.assertIn("radio_id", out)

    def test_voice_assistant_unknown_microphone(self):
        rc, out = run_config(self.base.replace("microphone: box3_microphone", "microphone: ghost_mic"))
        self.assertNotEqual(rc, 0)
        self.assertIn("ghost_mic", out)

    def test_din_pin_range(self):
        rc, out = run_config(self.base.replace("i2s_din_pin: 16", "i2s_din_pin: 99"))
        self.assertNotEqual(rc, 0)

    def test_invalid_s3_gpio_rejected(self):
        rc, out = run_config(self.base.replace("i2s_dout_pin: 15", "i2s_dout_pin: 25"))
        self.assertNotEqual(rc, 0)
        self.assertIn("25", out)

    def test_arduino_framework_rejected(self):
        text = self.base.replace("type: esp-idf\n    version: 6.0.1", "type: arduino")
        rc, out = run_config(text)
        self.assertNotEqual(rc, 0)

    def test_standard_station_commands(self):
        self.assertIn("media_player.next: radio", self.base)
        self.assertIn("media_player.previous: radio", self.base)

    def test_audio_pin_conflict_rejected(self):
        rc, out = run_config(self.base.replace("    bclk_pin: 10", "    bclk_pin: 17"))
        self.assertNotEqual(rc, 0)
        self.assertIn("Pin 17 is used in multiple places", out)

    def test_shared_amplifier_requires_explicit_permission(self):
        rc, out = run_config(self.base.replace("      allow_other_uses: true\n", ""))
        self.assertNotEqual(rc, 0)
        self.assertIn("Pin 46 is used in multiple places", out)

    def test_unknown_radio_reference(self):
        rc, out = run_config(self.base.replace("    radio_id: radio\n    channel", "    radio_id: nope\n    channel"))
        self.assertNotEqual(rc, 0)

    def test_old_mute_switch_removed(self):
        self.assertNotIn("MEDIA_PLAYER_COMMAND_MUTE", self.base)


if __name__ == "__main__":
    unittest.main()
