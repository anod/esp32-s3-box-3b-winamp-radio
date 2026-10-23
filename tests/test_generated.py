"""Level 2/3 tests: generated C++ (`esphome compile --only-generate`) and,
when a built firmware exists, the linked ELF.

Run: python3 -m unittest discover -s tests -v
"""
import glob
import os
import re
import shutil
import subprocess
import unittest

from test_config import ROOT, MAIN_YAML, local_yaml

BUILD = os.path.join(ROOT, ".esphome", "build", "esp32radio")
MAIN_CPP = os.path.join(BUILD, "src", "main.cpp")
ELF = os.path.join(BUILD, ".pioenvs", "esp32radio", "firmware.elf")
SDKCONFIG = os.path.join(BUILD, "sdkconfig.esp32radio")
FLASH_BUDGET = 8126464  # app partition from the build log
MIN_FREE_FLASH_PCT = 10


def toolchain(tool):
    hits = glob.glob(os.path.expanduser(f"~/.platformio/packages/toolchain-xtensa-esp-elf/bin/*-{tool}"))
    return hits[0] if hits else None


@unittest.skipUnless(shutil.which("esphome"), "esphome not installed")
class GeneratedCodeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = os.path.join(ROOT, "_tmp_test_generated.yaml")
        with open(MAIN_YAML) as f, open(tmp, "w") as out:
            out.write(local_yaml(f.read()))
        try:
            p = subprocess.run(["esphome", "compile", "--only-generate", tmp],
                               cwd=ROOT, capture_output=True, text=True)
        finally:
            os.remove(tmp)
        assert p.returncode == 0, p.stdout[-2000:] + p.stderr[-2000:]
        # The generated file is named after the node, not the temp YAML
        with open(MAIN_CPP) as f:
            cls.cpp = f.read()

    def test_mic_wired_to_radio(self):
        self.assertIn("box3_microphone->set_radio(radio);", self.cpp)
        self.assertIn("box3_microphone->set_use_right_channel(false);", self.cpp)

    def test_din_pin(self):
        self.assertIn("radio->set_i2s_din_pin(16);", self.cpp)

    def test_radio_session_hooks_called(self):
        # on_end, on_error and the interval safety net all end the session
        self.assertGreaterEqual(self.cpp.count("id(radio).voice_session_end();"), 3)

    def test_display_states_set(self):
        for st in (0, 1, 2, 3, 4, 5):
            self.assertIn(f"id(display).set_voice_state({st});", self.cpp)

    def test_voice_text_sensors_published(self):
        for sensor in ("voice_state_text", "voice_heard_text", "voice_reply_text"):
            self.assertIn(f"id({sensor}).publish_state(", self.cpp)

    def test_component_priority_order(self):
        order = [int(n) for n in re.findall(r"register_component_\((?:radio|bt_speaker|display|box3_microphone|va), (\d+)\)", self.cpp)]
        names = re.findall(r"register_component_\((radio|bt_speaker|display|box3_microphone|va), \d+\)", self.cpp)
        self.assertEqual(names, ["radio", "bt_speaker", "display", "box3_microphone", "va"])
        self.assertEqual(order, sorted(order))

    def test_es7210_present_and_no_i2s_audio(self):
        self.assertIn("es7210_adc", self.cpp)
        self.assertNotIn("i2s_audio::", self.cpp)

    def test_voice_assistant_uses_media_player(self):
        self.assertRegex(self.cpp, r"va->set_media_player\(radio\)")
        self.assertRegex(self.cpp, r"va->set_microphone_source\(")

    def test_crash_diagnostics_wired_and_internal(self):
        for setter, sensor in (
            ("free", "diag_heap_free"), ("min_free", "diag_heap_min"),
            ("block", "diag_heap_block"), ("psram", "diag_psram_free"),
            ("loop_time", "diag_loop_time"), ("reset_reason", "diag_reset_reason"),
        ):
            setter_call = f"radio_debug->set_{setter}_sensor({sensor});"
            self.assertTrue(setter_call in self.cpp, setter_call)
        for sensor in ("diag_heap_free", "diag_heap_min", "diag_heap_block",
                       "diag_psram_free", "diag_loop_time", "diag_reset_reason", "diag_uptime"):
            internal = re.search(
                rf"App\.register_(?:text_)?sensor\({sensor},[^\n]*// internal\b", self.cpp
            )
            self.assertIsNotNone(internal, f"{sensor} must stay internal")
        self.assertTrue("radio_debug->set_update_interval(5000);" in self.cpp)
        self.assertTrue('ESP_LOGD("radio_health", "Health: uptime=' in self.cpp)


@unittest.skipUnless(os.path.exists(ELF) and toolchain("gcc-nm"), "no built firmware.elf / toolchain")
class FirmwareImageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.syms = subprocess.run([toolchain("gcc-nm"), "-C", ELF], capture_output=True, text=True).stdout

    def test_icy_patch_linked(self):
        self.assertRegex(self.syms, r" D g_icy_patch_version")
        self.assertIn("g_icy_metaint", self.syms)

    def test_spectrum_override_linked(self):
        self.assertRegex(self.syms, r" T feed_fft_samples\(")

    def test_voice_code_linked(self):
        for sym in ("InternetRadio::voice_capture_begin()", "InternetRadio::voice_session_end()",
                    "InternetRadio::restore_after_voice_()", "Box3Mic::run_capture_()"):
            self.assertIn(sym, self.syms)

    def test_flash_headroom(self):
        size = os.path.getsize(os.path.join(os.path.dirname(ELF), "firmware.bin"))
        free_pct = 100 * (FLASH_BUDGET - size) / FLASH_BUDGET
        self.assertGreater(free_pct, MIN_FREE_FLASH_PCT, f"only {free_pct:.1f}% flash free")

    @unittest.skipUnless(os.path.exists(SDKCONFIG), "no sdkconfig")
    def test_sdkconfig_workarounds(self):
        with open(SDKCONFIG) as f:
            cfg = f.read()
        self.assertIn("# CONFIG_ESP_AUDIO_SIMPLE_PLAYER_HTTP_EN is not set", cfg)
        self.assertIn("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y", cfg)
        self.assertIn("# CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN is not set", cfg)


if __name__ == "__main__":
    unittest.main()
