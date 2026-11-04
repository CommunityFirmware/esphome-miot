import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv

from .. import (
    CONF_MIOT_FALSE,
    CONF_MIOT_ID,
    CONF_MIOT_PIID,
    CONF_MIOT_POLL,
    CONF_MIOT_SIID,
    CONF_MIOT_TRUE,
    MIOT_FALSE_DEFAULT,
    MIOT_TRUE_DEFAULT,
    Miot,
    miot_ns,
)

DEPENDENCIES = ["miot"]

MiotBinarySensor = miot_ns.class_(
    "MiotBinarySensor", binary_sensor.BinarySensor, cg.Component
)

CONFIG_SCHEMA = (
    binary_sensor.binary_sensor_schema(MiotBinarySensor)
    .extend(
        {
            cv.GenerateID(CONF_MIOT_ID): cv.use_id(Miot),
            cv.Required(CONF_MIOT_SIID): cv.uint32_t,
            cv.Required(CONF_MIOT_PIID): cv.uint32_t,
            cv.Optional(CONF_MIOT_POLL, default=True): cv.boolean,
            cv.Optional(CONF_MIOT_TRUE, default=MIOT_TRUE_DEFAULT): cv.string,
            cv.Optional(CONF_MIOT_FALSE, default=MIOT_FALSE_DEFAULT): cv.string,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
    await cg.register_component(var, config)

    parent = await cg.get_variable(config[CONF_MIOT_ID])
    cg.add(
        var.set_miot_config(
            parent,
            config[CONF_MIOT_SIID],
            config[CONF_MIOT_PIID],
            config[CONF_MIOT_POLL],
        )
    )
    cg.add(var.set_miot_bool_values(config[CONF_MIOT_TRUE], config[CONF_MIOT_FALSE]))
