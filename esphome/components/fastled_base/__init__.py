import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light
from esphome.const import CONF_OUTPUT_ID, CONF_NUM_LEDS, CONF_RGB_ORDER, CONF_MAX_REFRESH_RATE
from esphome.core import coroutine

fastled_base_ns = cg.esphome_ns.namespace('fastled_base')
FastLEDLightOutput = fastled_base_ns.class_('FastLEDLightOutput', light.AddressableLight)

RGB_ORDERS = [
    'RGB',
    'RBG',
    'GRB',
    'GBR',
    'BRG',
    'BGR',
]

BASE_SCHEMA = light.ADDRESSABLE_LIGHT_SCHEMA.extend({
    cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(FastLEDLightOutput),

    cv.Required(CONF_NUM_LEDS): cv.positive_not_null_int,
    cv.Optional(CONF_RGB_ORDER): cv.one_of(*RGB_ORDERS, upper=True),
    cv.Optional(CONF_MAX_REFRESH_RATE): cv.positive_time_period_microseconds,
}).extend(cv.COMPONENT_SCHEMA)


@coroutine
def new_fastled_light(config):
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    yield cg.register_component(var, config)

    if CONF_MAX_REFRESH_RATE in config:
        cg.add(var.set_max_refresh_rate(config[CONF_MAX_REFRESH_RATE]))

    yield light.register_light(var, config)
    cg.add_library('FastLED', '3.2.0')
    yield var
