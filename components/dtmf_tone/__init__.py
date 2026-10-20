import inspect

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import CONF_ID

CODEOWNERS = ["@pit3k"]
MULTI_CONF = True

ns = cg.esphome_ns.namespace("dtmf_tone")
DtmfTone = ns.class_("DtmfTone", cg.Component)
PlayAction = ns.class_("PlayAction", automation.Action)
StopAction = ns.class_("StopAction", automation.Action)

CONF_PIN_LOW = "pin_low"
CONF_PIN_HIGH = "pin_high"
CONF_CARRIER = "carrier_frequency"
CONF_SAMPLE_RATE = "sample_rate"
CONF_AMPLITUDE = "amplitude"
CONF_HIGH = "high_tone_level"
CONF_SEQUENCE = "sequence"
CONF_TONE_LENGTH = "tone_length"
CONF_GAP = "inter_digit_gap"


def gpio_num(value):
    if isinstance(value, str):
        text = value.strip().upper()
        if text.startswith("GPIO"):
            value = text[4:]
    return cv.int_range(min=0, max=21)(value)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(DtmfTone),
        cv.Required(CONF_PIN_LOW): gpio_num,
        cv.Optional(CONF_PIN_HIGH): gpio_num,
        cv.Optional(CONF_CARRIER, default=200000): cv.int_range(min=20000, max=300000),
        cv.Optional(CONF_SAMPLE_RATE, default=32000): cv.int_range(min=8000, max=48000),
        cv.Optional(CONF_AMPLITUDE, default="100%"): cv.percentage,
        cv.Optional(CONF_HIGH, default="80%"): cv.percentage,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    # ESPHome excludes unused IDF drivers. driver/gptimer.h and driver/ledc.h
    # live in these components and are not linked unless requested.
    try:
        from esphome.components.esp32 import include_builtin_idf_component
    except ImportError:
        include_builtin_idf_component = None
    if include_builtin_idf_component is not None:
        include_builtin_idf_component("esp_driver_gptimer")
        include_builtin_idf_component("esp_driver_ledc")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_pin_low(config[CONF_PIN_LOW]))
    if CONF_PIN_HIGH in config:
        cg.add(var.set_pin_high(config[CONF_PIN_HIGH]))
    else:
        cg.add(var.set_pin_high(-1))
    cg.add(var.set_carrier_frequency(config[CONF_CARRIER]))
    cg.add(var.set_sample_rate(config[CONF_SAMPLE_RATE]))
    cg.add(var.set_amplitude_pct(int(round(config[CONF_AMPLITUDE] * 100))))
    cg.add(var.set_high_pct(int(round(config[CONF_HIGH] * 100))))


PLAY_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(DtmfTone),
        cv.Required(CONF_SEQUENCE): cv.templatable(cv.string),
        cv.Optional(CONF_TONE_LENGTH, default="200ms"): cv.templatable(
            cv.positive_time_period_milliseconds
        ),
        cv.Optional(CONF_GAP, default="80ms"): cv.templatable(cv.positive_time_period_milliseconds),
    }
)

STOP_SCHEMA = automation.maybe_simple_id(
    {
        cv.GenerateID(): cv.use_id(DtmfTone),
    }
)


def _register_action(name, action_type, schema):
    kwargs = {}
    if "synchronous" in inspect.signature(automation.register_action).parameters:
        kwargs["synchronous"] = True
    return automation.register_action(name, action_type, schema, **kwargs)


@_register_action("dtmf_tone.play", PlayAction, PLAY_SCHEMA)
async def play_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_sequence(await cg.templatable(config[CONF_SEQUENCE], args, cg.std_string)))
    cg.add(var.set_tone_length(await cg.templatable(config[CONF_TONE_LENGTH], args, cg.uint32)))
    cg.add(var.set_inter_digit_gap(await cg.templatable(config[CONF_GAP], args, cg.uint32)))
    return var


@_register_action("dtmf_tone.stop", StopAction, STOP_SCHEMA)
async def stop_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
