import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import CONF_ID as CONF_ID

from .. import (
    CONF_MIOT_ID,
    CONF_MIOT_PIID,
    CONF_MIOT_POLL,
    CONF_MIOT_SIID,
    Miot,
    miot_ns,
)

DEPENDENCIES = ["miot"]

MiotTextSensor = miot_ns.class_("MiotTextSensor", text_sensor.TextSensor, cg.Component)

CONFIG_SCHEMA = (
    text_sensor.text_sensor_schema(MiotTextSensor)
    .extend(
        {
            cv.GenerateID(CONF_MIOT_ID): cv.use_id(Miot),
            cv.Required(CONF_MIOT_SIID): cv.uint32_t,
            cv.Required(CONF_MIOT_PIID): cv.uint32_t,
            cv.Optional(CONF_MIOT_POLL, default=True): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
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
