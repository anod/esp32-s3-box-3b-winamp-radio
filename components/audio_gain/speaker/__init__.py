import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import audio, speaker
from esphome.components.audio_gain import audio_gain_ns
from esphome.const import (
    CONF_AUDIO_DAC,
    CONF_BITS_PER_SAMPLE,
    CONF_ID,
    CONF_NUM_CHANNELS,
    CONF_OUTPUT_SPEAKER,
    CONF_SAMPLE_RATE,
)
from esphome.core.entity_helpers import inherit_property_from

DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["audio"]

GainSpeaker = audio_gain_ns.class_("GainSpeaker", cg.Component, speaker.Speaker)
CONF_FIXED_GAIN = "fixed_gain"
CONF_TAP_SPEAKER = "tap_speaker"


def validate_output(config):
    for prop in (CONF_SAMPLE_RATE, CONF_NUM_CHANNELS):
        inherit_property_from(prop, CONF_OUTPUT_SPEAKER)(config)
    audio.final_validate_audio_schema(
        "audio_gain",
        audio_device=CONF_OUTPUT_SPEAKER,
        bits_per_sample=16,
        channels=config.get(CONF_NUM_CHANNELS),
        sample_rate=config.get(CONF_SAMPLE_RATE),
    )(config)
    return config


def stream_limits(config):
    if CONF_TAP_SPEAKER in config:
        if CONF_FIXED_GAIN in config:
            raise cv.Invalid("Fixed voice output must not have a Bluetooth PCM tap")
        if config[CONF_TAP_SPEAKER] == config[CONF_OUTPUT_SPEAKER]:
            raise cv.Invalid("PCM tap must use a separate output speaker")
    audio.set_stream_limits(min_bits_per_sample=16, max_bits_per_sample=16)(config)
    return config


CONFIG_SCHEMA = cv.All(
    speaker.SPEAKER_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(GainSpeaker),
            cv.Required(CONF_OUTPUT_SPEAKER): cv.use_id(speaker.Speaker),
            cv.Optional(CONF_BITS_PER_SAMPLE, default=16): cv.one_of(16, int=True),
            cv.Optional(CONF_FIXED_GAIN): cv.percentage,
            cv.Optional(CONF_TAP_SPEAKER): cv.use_id(speaker.Speaker),
            cv.Optional(CONF_AUDIO_DAC): cv.invalid("The shared DAC belongs only on the native output speaker"),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    stream_limits,
)

FINAL_VALIDATE_SCHEMA = validate_output


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await speaker.register_speaker(var, config)
    output = await cg.get_variable(config[CONF_OUTPUT_SPEAKER])
    cg.add(var.set_output_speaker(output))
    if CONF_TAP_SPEAKER in config:
        cg.add(var.set_tap_speaker(await cg.get_variable(config[CONF_TAP_SPEAKER])))
    if CONF_FIXED_GAIN in config:
        cg.add(var.set_fixed_gain(config[CONF_FIXED_GAIN]))
