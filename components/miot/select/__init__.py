import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv
from esphome.const import CONF_OPTIONS

from .. import (
    CONF_MIOT_ID,
    CONF_MIOT_PIID,
    CONF_MIOT_POLL,
    CONF_MIOT_SIID,
    Miot,
    ensure_option_map,
    miot_ns,
)

DEPENDENCIES = ["miot"]

MiotSelect = miot_ns.class_("MiotSelect", select.Select, cg.Component)

CONFIG_SCHEMA = (
    select.select_schema(MiotSelect)
    .extend(
        {
            cv.GenerateID(CONF_MIOT_ID): cv.use_id(Miot),
            cv.Required(CONF_MIOT_SIID): cv.uint32_t,
            cv.Required(CONF_MIOT_PIID): cv.uint32_t,
            cv.Required(CONF_OPTIONS): ensure_option_map,
            cv.Optional(CONF_MIOT_POLL, default=True): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    options_map = config[CONF_OPTIONS]
    var = await select.new_select(config, options=list(options_map.values()))
    await cg.register_component(var, config)
    cg.add(var.set_select_mappings(list(options_map.keys())))
    parent = await cg.get_variable(config[CONF_MIOT_ID])
    cg.add(
        var.set_miot_config(
            parent,
            config[CONF_MIOT_SIID],
            config[CONF_MIOT_PIID],
            config[CONF_MIOT_POLL],
        )
    )
