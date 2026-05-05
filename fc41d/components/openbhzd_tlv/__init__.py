"""openbhzd_tlv — ESPHome side of the OpenBHZD MCU TLV protocol.

Talks to the OpenBHZD safety-core MCU (GD32F205VG) over UART using the
binary TLV protocol from spec § 5. Pairs with a UART configured at
115200 8N1; on the FC41D BK7231N deployment that's UART1 (P10/P11),
wired to the MCU's UART4 (PC12 TX / PD2 RX).
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import uart
from esphome.const import CONF_ID, CONF_TRIGGER_ID

CODEOWNERS = ["@andrewrankin"]
DEPENDENCIES = ["uart"]

openbhzd_tlv_ns = cg.esphome_ns.namespace("openbhzd_tlv")
OpenbhzdTlv = openbhzd_tlv_ns.class_("OpenbhzdTlv", cg.Component, uart.UARTDevice)
RFIDAuthResultTrigger = openbhzd_tlv_ns.class_(
    "RFIDAuthResultTrigger", automation.Trigger.template(cg.std_string, cg.uint8)
)

CONF_POLL_INTERVAL = "poll_interval"
CONF_LINK_TIMEOUT = "link_timeout"
CONF_ON_RFID_AUTH_RESULT = "on_rfid_auth_result"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(OpenbhzdTlv),
            cv.Optional(
                CONF_POLL_INTERVAL, default="5s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_LINK_TIMEOUT, default="15s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_ON_RFID_AUTH_RESULT): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        RFIDAuthResultTrigger
                    ),
                }
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_poll_interval(config[CONF_POLL_INTERVAL]))
    cg.add(var.set_link_timeout(config[CONF_LINK_TIMEOUT]))

    for conf in config.get(CONF_ON_RFID_AUTH_RESULT, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        # x = std::string uid, result = uint8_t auth result code
        await automation.build_automation(
            trigger,
            [(cg.std_string, "x"), (cg.uint8, "result")],
            conf,
        )
