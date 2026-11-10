"""Per-path PCM gain for native ESPHome speakers."""

import esphome.codegen as cg

audio_gain_ns = cg.esphome_ns.namespace("audio_gain")
