import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch

from . import openevcharger_tlv_ns, OpenevchargerTlv

CONF_OPENEVCHARGER_TLV_ID = "openevcharger_tlv_id"
CONF_REQUIRE_RFID_AUTH = "require_rfid_auth"

OpenevchargerTlvSwitch = openevcharger_tlv_ns.class_(
    "OpenevchargerTlvSwitch", switch.Switch, cg.Component
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_OPENEVCHARGER_TLV_ID): cv.use_id(OpenevchargerTlv),
        cv.Optional(CONF_REQUIRE_RFID_AUTH): switch.switch_schema(OpenevchargerTlvSwitch),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_OPENEVCHARGER_TLV_ID])
    if conf := config.get(CONF_REQUIRE_RFID_AUTH):
        sw = await switch.new_switch(conf)
        await cg.register_component(sw, conf)
        cg.add(sw.set_parent(parent))
        cg.add(parent.set_require_rfid_auth_switch(sw))
