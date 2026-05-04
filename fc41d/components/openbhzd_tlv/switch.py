import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch

from . import openbhzd_tlv_ns, OpenbhzdTlv

CONF_OPENBHZD_TLV_ID = "openbhzd_tlv_id"
CONF_REQUIRE_RFID_AUTH = "require_rfid_auth"

OpenbhzdTlvSwitch = openbhzd_tlv_ns.class_(
    "OpenbhzdTlvSwitch", switch.Switch, cg.Component
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_OPENBHZD_TLV_ID): cv.use_id(OpenbhzdTlv),
        cv.Optional(CONF_REQUIRE_RFID_AUTH): switch.switch_schema(OpenbhzdTlvSwitch),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_OPENBHZD_TLV_ID])
    if conf := config.get(CONF_REQUIRE_RFID_AUTH):
        sw = await switch.new_switch(conf)
        await cg.register_component(sw, conf)
        cg.add(sw.set_parent(parent))
        cg.add(parent.set_require_rfid_auth_switch(sw))
