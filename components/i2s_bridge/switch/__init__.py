"""ESPHome switch platform: I2S bridge to WROOM-32D A2DP board."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_ID
from esphome.components.i2s_bridge import i2s_bridge_ns

I2SBridge = i2s_bridge_ns.class_("I2SBridge", switch.Switch, cg.Component)

CONF_BCLK_PIN = "bclk_pin"
CONF_LRCK_PIN = "lrck_pin"
CONF_DOUT_PIN = "dout_pin"
CONF_PA_PIN = "pa_pin"

CONFIG_SCHEMA = (
    switch.switch_schema(I2SBridge)
    .extend(
        {
            cv.Required(CONF_BCLK_PIN): cv.int_range(min=0, max=48),
            cv.Required(CONF_LRCK_PIN): cv.int_range(min=0, max=48),
            cv.Required(CONF_DOUT_PIN): cv.int_range(min=0, max=48),
            cv.Optional(CONF_PA_PIN, default=-1): cv.int_range(min=-1, max=48),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await switch.register_switch(var, config)

    cg.add(var.set_bclk_pin(config[CONF_BCLK_PIN]))
    cg.add(var.set_lrck_pin(config[CONF_LRCK_PIN]))
    cg.add(var.set_dout_pin(config[CONF_DOUT_PIN]))
    cg.add(var.set_pa_pin(config[CONF_PA_PIN]))
