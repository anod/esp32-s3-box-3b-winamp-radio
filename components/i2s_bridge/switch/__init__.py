"""ESPHome switch platform: I2S bridge to WROOM-32D A2DP board."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import switch
from esphome.const import CONF_ID
from esphome.components.i2s_bridge import i2s_bridge_ns

I2SBridge = i2s_bridge_ns.class_("I2SBridge", switch.Switch, cg.Component)

CONF_BCLK_PIN = "bclk_pin"
CONF_LRCK_PIN = "lrck_pin"
CONF_DOUT_PIN = "dout_pin"
CONF_PA_PIN = "pa_pin"

CONFIG_SCHEMA = cv.All(
    switch.switch_schema(I2SBridge)
    .extend(
        {
            cv.Required(CONF_BCLK_PIN): pins.internal_gpio_output_pin_number,
            cv.Required(CONF_LRCK_PIN): pins.internal_gpio_output_pin_number,
            cv.Required(CONF_DOUT_PIN): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_PA_PIN, default=-1): cv.Any(-1, pins.internal_gpio_output_pin_number),
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    cv.only_with_framework("esp-idf"),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await switch.register_switch(var, config)

    cg.add(var.set_bclk_pin(config[CONF_BCLK_PIN]))
    cg.add(var.set_lrck_pin(config[CONF_LRCK_PIN]))
    cg.add(var.set_dout_pin(config[CONF_DOUT_PIN]))
    cg.add(var.set_pa_pin(config[CONF_PA_PIN]))
