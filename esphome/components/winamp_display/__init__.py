import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.components.esp32 import include_builtin_idf_component

CODEOWNERS = ["@alexg"]
# No DEPENDENCIES — we use use_id() which handles cross-references at codegen time

winamp_display_ns = cg.esphome_ns.namespace("winamp_display")
WinampDisplay = winamp_display_ns.class_("WinampDisplay", cg.Component)

CONF_RADIO_ID = "radio_id"
CONF_BRIDGE_ID = "bridge_id"
CONF_BRIGHTNESS = "brightness"

# Import the classes we reference
internet_radio_ns = cg.esphome_ns.namespace("internet_radio")
InternetRadio = internet_radio_ns.class_("InternetRadio")

i2s_bridge_ns = cg.esphome_ns.namespace("i2s_bridge")
I2SBridge = i2s_bridge_ns.class_("I2SBridge")

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(WinampDisplay),
        cv.Required(CONF_RADIO_ID): cv.use_id(InternetRadio),
        cv.Required(CONF_BRIDGE_ID): cv.use_id(I2SBridge),
        cv.Optional(CONF_BRIGHTNESS, default=160): cv.int_range(min=1, max=255),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    radio = await cg.get_variable(config[CONF_RADIO_ID])
    cg.add(var.set_radio(radio))

    bridge = await cg.get_variable(config[CONF_BRIDGE_ID])
    cg.add(var.set_bridge(bridge))

    cg.add(var.set_brightness(config[CONF_BRIGHTNESS]))

    # LovyanGFX library
    cg.add_library("lovyan03/LovyanGFX", "1.2.19")

    # LovyanGFX Bus_Parallel16 needs esp_lcd IDF component (excluded by default)
    include_builtin_idf_component("esp_lcd")

    # Build flags for LovyanGFX autodetect
    cg.add_build_flag("-DLGFX_USE_V1")
    cg.add_build_flag("-DLGFX_AUTODETECT")
