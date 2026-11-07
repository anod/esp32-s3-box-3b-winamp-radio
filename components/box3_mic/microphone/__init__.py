"""ESPHome microphone platform: box3_mic.

Reads the ES7210 ADC from the I2S0 RX channel owned by internet_radio.
Output is 16 kHz, mono, 16-bit, as required by voice_assistant.
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import audio, microphone
from esphome.const import CONF_CHANNEL, CONF_ID
from esphome.components.box3_mic import box3_mic_ns

CODEOWNERS = ["@alexg"]
DEPENDENCIES = ["media_player", "i2c"]

CONF_RADIO_ID = "radio_id"
CONF_WAKE_MODE = "wake_mode"

Box3Mic = box3_mic_ns.class_("Box3Mic", microphone.Microphone, cg.Component)
InternetRadio = cg.esphome_ns.namespace("internet_radio").class_("InternetRadio")


def _set_stream_limits(config):
    audio.set_stream_limits(
        min_bits_per_sample=16,
        max_bits_per_sample=16,
        min_channels=1,
        max_channels=1,
        min_sample_rate=16000,
        max_sample_rate=16000,
    )(config)
    return config


CONFIG_SCHEMA = cv.All(
    microphone.MICROPHONE_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(Box3Mic),
            cv.Required(CONF_RADIO_ID): cv.use_id(InternetRadio),
            # Wake mode keeps listening while the radio plays (for micro_wake_word)
            cv.Optional(CONF_WAKE_MODE, default=False): cv.boolean,
            # ES7210 puts MIC1 in the left slot and MIC2 in the right slot
            cv.Optional(CONF_CHANNEL, default="left"): cv.one_of(
                "left", "right", lower=True
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _set_stream_limits,
    cv.only_on_esp32,
    cv.only_with_framework("esp-idf"),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await microphone.register_microphone(var, config)

    radio = await cg.get_variable(config[CONF_RADIO_ID])
    cg.add(var.set_radio(radio))
    cg.add(var.set_use_right_channel(config[CONF_CHANNEL] == "right"))
    cg.add(var.set_wake_mode(config[CONF_WAKE_MODE]))
