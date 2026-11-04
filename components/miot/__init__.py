import esphome.codegen as cg
from esphome.components import ota, uart
import esphome.config_validation as cv
from esphome.const import CONF_ID

DEPENDENCIES = ["uart"]
DOMAIN = "miot"

miot_ns = cg.esphome_ns.namespace("miot")
Miot = miot_ns.class_("Miot", cg.Component, uart.UARTDevice)

CONF_MIOT_ID = "miot_id"
CONF_MIOT_HEARTBEAT_SIID = "miot_heartbeat_siid"
CONF_MIOT_HEARTBEAT_PIID = "miot_heartbeat_piid"
CONF_MIOT_OTA_NET_INDICATOR = "miot_ota_net_indicator"
CONF_MIOT_SIID = "miot_siid"
CONF_MIOT_PIID = "miot_piid"
CONF_MIOT_EIID = "miot_eiid"
CONF_MIOT_POLL = "miot_poll"
CONF_MIOT_TRUE = "miot_true"
MIOT_TRUE_DEFAULT = "true"
CONF_MIOT_FALSE = "miot_false"
MIOT_FALSE_DEFAULT = "false"

# The MIoT serial protocol is fixed to 115200 8N1.
MIOT_BAUD_RATE = 115200


def ensure_option_map(value):
    """Validate a mapping of MIoT property values to option names."""
    cv.check_not_templatable(value)
    option = cv.All(cv.int_range(0, 2**32 - 1))
    mapping = cv.All(cv.string_strict)
    value = cv.Schema({option: mapping})(value)

    names = list(value.values())
    if len(names) != len(set(names)):
        raise cv.Invalid("Option names must be unique.")

    return value


def validate_heartbeat(config):
    if (CONF_MIOT_HEARTBEAT_SIID in config) != (CONF_MIOT_HEARTBEAT_PIID in config):
        raise cv.Invalid(
            "either none or both of miot_heartbeat_siid and miot_heartbeat_piid are required"
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.require_esphome_version(2026, 10, 0),
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(Miot),
            cv.Optional(CONF_MIOT_HEARTBEAT_SIID): cv.uint32_t,
            cv.Optional(CONF_MIOT_HEARTBEAT_PIID): cv.uint32_t,
            cv.Optional(CONF_MIOT_OTA_NET_INDICATOR, default="updating"): cv.string,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA),
    validate_heartbeat,
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    DOMAIN, require_tx=True, require_rx=True, baud_rate=MIOT_BAUD_RATE
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    if CONF_MIOT_HEARTBEAT_SIID in config:
        cg.add(
            var.set_heartbeat_config(
                config[CONF_MIOT_HEARTBEAT_SIID], config[CONF_MIOT_HEARTBEAT_PIID]
            )
        )
    cg.add(var.set_ota_net_indicator(config[CONF_MIOT_OTA_NET_INDICATOR]))

    ota.request_ota_state_listeners()
