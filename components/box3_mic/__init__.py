"""ESPHome external component: ESP32-S3-BOX-3 microphone (shares I2S0 with the radio)."""
import esphome.codegen as cg

box3_mic_ns = cg.esphome_ns.namespace("box3_mic")
