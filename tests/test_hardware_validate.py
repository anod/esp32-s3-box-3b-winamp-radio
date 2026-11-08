import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch
import json

spec = importlib.util.spec_from_file_location(
    "hardware_validate", Path(__file__).resolve().parents[1] / "tools/hardware_validate.py"
)
hardware = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hardware)


class EvidenceTests(unittest.TestCase):
    def test_memory_and_pcm_are_separate_evidence(self):
        result = hardware.summarize([
            "\x1b[0;32mRuntime diag: internal=12000 min=9000 largest=4096 psram=8000000 "
            "loop_us=3400 gap_ms=96 control=0 reset=3 announce=0\x1b[0m",
            "Runtime diag: internal=14000 min=9000 largest=8192 psram=7000000 "
            "loop_us=1200 gap_ms=80 control=0 reset=3",
            "Audio diag: cb=40 bytes=184320 i2s0_to=0 i2s0_err=0 "
            "bridge_to=0 bridge_err=0 bridge_short=0 i2s0_short=0",
        ])
        self.assertEqual(result["internal_free_min"], 12000)
        self.assertEqual(result["internal_largest_min"], 4096)
        self.assertEqual(result["radio_loop_max_us"], 3400)
        self.assertEqual(result["pcm_bytes"], 184320)
        self.assertEqual(result["write_counters"], [0] * 6)

    def test_state_is_not_pcm_evidence(self):
        result = hardware.summarize(["STATE PLAYING"])
        self.assertEqual(result["pcm_bytes"], 0)
        self.assertEqual(result["runtime_samples"], 0)
        self.assertIsNone(result["internal_free_min"])

    def test_decoder_error_exact_vendor_message(self):
        result = hardware.summarize(["ESP_AAC_DEC: There is no enough memory for AAC buffer"])
        self.assertEqual(result["decoder_memory_errors"], 1)

    def test_actual_written_pcm_is_reported(self):
        result = hardware.summarize([
            "Audio diag: cb=4 bytes=18432 i2s0_to=0 i2s0_err=0 bridge_to=0 "
            "bridge_err=0 bridge_short=0 i2s0_short=0 sr=44100 bt=on written=13824"
        ])
        self.assertEqual(result["i2s0_written_bytes"], 13824)
        self.assertNotEqual(result["pcm_bytes"], result["i2s0_written_bytes"])

    def test_provenance_requires_build_and_upload_identity(self):
        def remote(args, command):
            self.assertIn("uploaded-sha256", command[2])
            self.assertIn("uploaded-artifact-sha256", command[2])
            print(json.dumps({"source_sha256": command[-1], "compilation_time": "matched"}))
        with patch.object(hardware, "guest", side_effect=remote):
            self.assertEqual(hardware.provenance()["compilation_time"], "matched")

    def test_full_log_counters_and_expected_events(self):
        lines = [
            "[E][internet_radio:194]: Player control timed out after 10s; audio quarantined",
            "[W][internet_radio:182]: Playback startup timed out: no PCM within 20s",
            "[W][internet_radio:602]: HTTP stream cancelled at read (epoch 4 superseded by 5)",
            "[I][internet_radio:1147]: Player control: generation=9 op=0 ms=212 result=0 stack=900",
            "[I][internet_radio:1147]: Player control: generation=8 op=1 ms=35 result=0 stack=900",
        ]
        result = hardware.summarize(lines)
        self.assertEqual(result["control_timeouts"], 1)
        self.assertEqual(result["http_cancellations"], 1)
        self.assertEqual(result["control_max_ms"], {"STOP": 212, "RUN": 35})
        self.assertEqual(result["error_lines"], 1)
        expected = {name: 0 for name in hardware.EXPECTED_EVENTS}
        expected["startup_timeouts"] = 1
        problems = hardware.final_problems(result, expected)
        self.assertEqual(problems, ["control_timeouts=1 (expected 0)"])
        expected["control_timeouts"] = 1
        self.assertEqual(hardware.final_problems(result, expected), [])
        expected["startup_timeouts"] = 2
        self.assertEqual(hardware.final_problems(result, expected), ["startup_timeouts=1 (expected 2)"])

    def audio(self, nbytes, ann=0, peak=500, sr=44100):
        return (f"Audio diag: cb=40 bytes={nbytes} i2s0_to=0 i2s0_err=0 bridge_to=0 bridge_err=0 "
                f"bridge_short=0 i2s0_short=0 sr={sr} bt=on written={nbytes} ann={ann} peak={peak}")

    runtime = ("Runtime diag: internal=60000 min=50000 largest=30000 psram=16000000 "
               "loop_us=900 gap_ms=40 control=0 reset=3 announce=0 rx=0 voice=0")

    def test_sustained_pcm_is_judged_by_rate_not_line_count(self):
        good = hardware.summarize([self.runtime, self.audio(176400)] * 5)
        self.assertEqual(hardware.sustained_problems(good, 5), [])
        straddled = hardware.summarize([self.runtime, self.audio(73728)] + [self.runtime, self.audio(176400)] * 4)
        self.assertEqual(hardware.sustained_problems(straddled, 5), [])
        dribble = hardware.summarize([self.runtime, self.audio(4608)] * 5)
        self.assertTrue(any("rate" in x for x in hardware.sustained_problems(dribble, 5)))
        long_window = hardware.summarize([self.runtime, self.audio(176400)] * 57)
        self.assertEqual(hardware.sustained_problems(long_window, 60), [])
        sparse = hardware.summarize([self.runtime, self.audio(176400)] * 20)
        self.assertTrue(any("telemetry" in x for x in hardware.sustained_problems(sparse, 60)))
        gap = hardware.summarize([self.runtime, self.audio(176400), self.runtime, self.runtime,
                                  self.runtime, self.audio(176400)])
        self.assertTrue(any("present" in x for x in hardware.sustained_problems(gap, 4)))

    def test_announcement_and_radio_pcm_are_separated(self):
        result = hardware.summarize([self.audio(176400, ann=176400, peak=700),
                                     self.audio(176400, peak=0)])
        self.assertEqual(result["announcement_bytes"], 176400)
        self.assertEqual(result["radio_bytes"], 176400)
        self.assertEqual(result["announcement_seconds"], 1)
        self.assertEqual(result["announcement_peak_min"], 700)
        self.assertEqual(result["radio_peak_max"], 0)

    def test_slow_server_must_be_closed_once_by_device(self):
        good = ("0.0 READY lifetime=420\n5.0 REQ 192.168.1.107 /tone.mp3\n"
                "40.0 REQ 192.168.1.107 /slow.mp3\n"
                "61.0 SLOW_END 192.168.1.107 sent=21 reason=client-closed:BrokenPipeError\n")
        self.assertEqual(hardware.slow_server_problems(good, 28), [])
        reconnect = good + "62.0 REQ 192.168.1.107 /slow.mp3\n"
        self.assertTrue(hardware.slow_server_problems(reconnect, 28))
        expired = good.replace("client-closed:BrokenPipeError", "server-expired")
        self.assertTrue(hardware.slow_server_problems(expired, 28))
        self.assertTrue(hardware.slow_server_problems(good, 10))
        twice = good + ("90.0 REQ 192.168.1.107 /slow.mp3\n"
                        "112.0 SLOW_END 192.168.1.107 sent=22 reason=client-closed:BrokenPipeError\n")
        self.assertEqual(hardware.slow_server_problems(twice, [28, 25]), [])
        self.assertTrue(hardware.slow_server_problems(twice, [28]))
        self.assertTrue(hardware.slow_server_problems(twice, [28, 15]))

    def test_keepalive_404_must_be_closed_promptly_by_device(self):
        good = ("1.0 REQ 192.168.1.107 /missing.mp3\n1.0 KEEPALIVE_404 192.168.1.107 /missing.mp3\n"
                "1.4 KEEPALIVE_END 192.168.1.107 /missing.mp3 held=0.4 reason=client-closed\n")
        ends, problems = hardware.keepalive_problems(good)
        self.assertEqual(problems, [])
        self.assertEqual(ends[0]["held_s"], 0.4)
        self.assertTrue(hardware.keepalive_problems(good.replace("client-closed", "server-timeout"))[1])
        reset = good.replace("client-closed", "client-reset:ConnectionResetError")
        self.assertEqual(hardware.keepalive_problems(reset)[1], [])
        self.assertTrue(hardware.keepalive_problems(good.replace("held=0.4", "held=40.0"))[1])
        leaked = ("1.0 KEEPALIVE_404 192.168.1.107 /missing.mp3\n"
                  "90.0 KEEPALIVE_OPEN 192.168.1.107 /missing.mp3 held=89.0\n")
        self.assertEqual(len(hardware.keepalive_problems(leaked)[1]), 2)

    def test_http_close_counters_are_cumulative_last_value(self):
        def runtime(cancel, stale):
            return ("Runtime diag: internal=70000 min=50000 largest=31744 psram=16000000 loop_us=4000 "
                    f"gap_ms=100 control=0 reset=3 announce=0 rx=1 voice=0 "
                    f"http_close=cancel:{cancel},{cancel} stale:{stale},0")
        result = hardware.summarize([runtime(1, 0), runtime(2, 1)])
        self.assertEqual(result["http_close"], [2, 2, 1, 0])
        settled = hardware.summarize([runtime(1, 0) + " gmf_settle=4,12,0",
                                      "[D][esp-idf:000][radio_control]: E (1) ESP_GMF_TASK: Run timeout,[TSK_0x1,0x1]"])
        self.assertEqual(settled["gmf_settle"], [4, 12, 0])
        self.assertEqual(settled["gmf_run_timeouts"], 1)
        self.assertTrue(any("GMF run timeouts" in x for x in hardware.final_problems(
            {**settled, "reset_reasons": [3]}, {})))
        self.assertEqual(result["internal_free_min"], 70000)
        stopped = hardware.summarize([runtime(1, 0) + " gmf_settle=4,12,0 gmf_stop=2,0"])
        self.assertEqual(stopped["gmf_stop"], [2, 0])
        self.assertEqual(hardware.final_problems({**stopped, "reset_reasons": [3]}, {}), [])
        unconfirmed = hardware.summarize([runtime(1, 0) + " gmf_settle=4,12,0 gmf_stop=2,1"])
        self.assertTrue(any("not confirmed" in x for x in hardware.final_problems(
            {**unconfirmed, "reset_reasons": [3]}, {})))

    def test_kind_drift_detects_psram_step_in_first_cycle(self):
        transitions = [{"kind": "aac", "psram_free_min": v} for v in
                       [16222500, 16200000, 16200010, 16200020, 16200000, 16200030]]
        self.assertLess(hardware.kind_drift(transitions, "psram_free_min")["aac"], -8192)
        self.assertGreater(hardware.kind_drift(transitions[1:], "psram_free_min")["aac"], -8192)

    def test_warmup_step_is_reported_separately(self):
        transitions = [{"kind": k, "internal_free_max": v} for k, v in
                       [("a", 60000), ("b", 61000), ("a", 53000), ("b", 54000), ("a", 53100), ("b", 54000)]]
        self.assertEqual(hardware.warmup_step(transitions, 2), {"a": -6950, "b": -7000})

    def test_clock_scale_uses_device_uptime_as_reference(self):
        # Dev host monotonic ran ~10.4% fast: 1769.6 host s for 1603 device s.
        samples = [(94627.5 + h * 1.104, 4337 + int(h)) for h in range(0, 1603, 5)]
        self.assertAlmostEqual(hardware.clock_scale(samples), 1 / 1.104, places=3)
        self.assertIsNone(hardware.clock_scale(samples[:3]))
        self.assertIsNone(hardware.clock_scale([]))

    def test_heap_trend_reports_drift(self):
        samples = [(t, 60000 - t, 50000, 30000) for t in range(0, 3600, 60)]
        trend = hardware.heap_trend(samples)
        self.assertLess(trend["last_third_free_min"], trend["first_third_free_min"])
        self.assertEqual(trend["free_slope_bytes_per_hour"], -3600)

    def test_kind_drift_uses_steady_ceiling_not_noisy_minima(self):
        # Soak run 5 AAC dwells: minima swung 58-71 KB, ceilings stayed ~72.8-73.0 KB.
        maxima = [73068, 72924, 72944, 72948, 72976, 72968, 72952, 72796, 72816, 72776]
        minima = [66860, 66384, 68852, 67360, 68688, 63964, 71016, 58472, 62220, 60428]
        transitions = [{"kind": "aac", "internal_free_max": hi, "internal_free_min": lo}
                       for hi, lo in zip(maxima, minima)]
        transitions.append({"kind": "mp3", "internal_free_max": 70000})
        drift = hardware.kind_drift(transitions)
        self.assertEqual(drift, {"aac": -148})
        self.assertLess(hardware.kind_drift(transitions, "internal_free_min")["aac"], -4096)

    def test_worker_done_after_uses_main_loop_control_flag(self):
        def runtime(control):
            return ("[I][internet_radio:322]: Runtime diag: internal=70000 min=50000 largest=31744 "
                    f"psram=16000000 loop_us=4000 gap_ms=100 control={control} reset=3")
        lines = [runtime(0), "[W][internet_radio:182]: Playback startup timed out: no PCM within 20s",
                 runtime(1), runtime(1), runtime(0)]
        self.assertEqual(hardware.worker_done_after(lines, 1), 4)
        self.assertIsNone(hardware.worker_done_after(lines[:4], 1))

    def test_kind_drift_detects_leak(self):
        transitions = [{"kind": "mp3", "internal_free_max": 70000 - 1000 * n} for n in range(9)]
        self.assertEqual(hardware.kind_drift(transitions), {"mp3": -6000})
