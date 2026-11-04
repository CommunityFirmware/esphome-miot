import esphome.codegen as cg
from esphome.components import fan
import esphome.config_validation as cv
from esphome.const import (
    CONF_DIRECTION,
    CONF_MAX_VALUE,
    CONF_MIN_VALUE,
    CONF_OPTIONS,
    CONF_OSCILLATING,
    CONF_PRESET_MODES,
    CONF_SPEED,
    CONF_STATE,
    CONF_STEP,
)

from .. import (
    CONF_MIOT_ID,
    CONF_MIOT_PIID,
    CONF_MIOT_SIID,
    Miot,
    ensure_option_map,
    miot_ns,
)

DEPENDENCIES = ["miot"]

MiotFan = miot_ns.class_("MiotFan", cg.Component, fan.Fan)

PROPERTY_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_MIOT_SIID): cv.uint32_t,
        cv.Required(CONF_MIOT_PIID): cv.uint32_t,
    }
)


def validate_speed(config):
    if config[CONF_MAX_VALUE] <= config[CONF_MIN_VALUE]:
        raise cv.Invalid("max_value must be greater than min_value")
    return config


SPEED_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(CONF_MIOT_SIID): cv.uint32_t,
            cv.Required(CONF_MIOT_PIID): cv.uint32_t,
            cv.Required(CONF_MIN_VALUE): cv.uint32_t,
            cv.Required(CONF_MAX_VALUE): cv.uint32_t,
            cv.Optional(CONF_STEP, default=1): cv.int_range(min=1),
        }
    ),
    validate_speed,
)

CONFIG_SCHEMA = (
    fan.fan_schema(MiotFan)
    .extend(
        {
            cv.GenerateID(CONF_MIOT_ID): cv.use_id(Miot),
            cv.Required(CONF_STATE): PROPERTY_SCHEMA,
            cv.Required(CONF_SPEED): SPEED_SCHEMA,
            cv.Optional(CONF_OSCILLATING): PROPERTY_SCHEMA,
            cv.Optional(CONF_DIRECTION): PROPERTY_SCHEMA,
            cv.Optional(CONF_PRESET_MODES): cv.Schema(
                {
                    cv.Required(CONF_MIOT_SIID): cv.uint32_t,
                    cv.Required(CONF_MIOT_PIID): cv.uint32_t,
                    cv.Required(CONF_OPTIONS): ensure_option_map,
                }
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_MIOT_ID])

    var = await fan.new_fan(config, parent)
    await cg.register_component(var, config)

    state = config[CONF_STATE]
    cg.add(var.set_state_config(state[CONF_MIOT_SIID], state[CONF_MIOT_PIID]))

    speed = config[CONF_SPEED]
    cg.add(
        var.set_speed_config(
            speed[CONF_MIOT_SIID],
            speed[CONF_MIOT_PIID],
            speed[CONF_MIN_VALUE],
            speed[CONF_MAX_VALUE],
            speed[CONF_STEP],
        )
    )

    if oscillating := config.get(CONF_OSCILLATING):
        cg.add(
            var.set_oscillating_config(
                oscillating[CONF_MIOT_SIID], oscillating[CONF_MIOT_PIID]
            )
        )
    if direction := config.get(CONF_DIRECTION):
        cg.add(
            var.set_direction_config(
                direction[CONF_MIOT_SIID], direction[CONF_MIOT_PIID]
            )
        )
    if preset_modes := config.get(CONF_PRESET_MODES):
        cg.add(
            var.set_preset_modes_config(
                preset_modes[CONF_MIOT_SIID], preset_modes[CONF_MIOT_PIID]
            )
        )
        options_map = preset_modes[CONF_OPTIONS]
        for option_value, option_name in options_map.items():
            cg.add(var.set_preset_mode_name(option_value, option_name))
