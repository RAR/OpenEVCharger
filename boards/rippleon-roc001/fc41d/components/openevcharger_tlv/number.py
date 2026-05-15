import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import (
    DEVICE_CLASS_CURRENT,
    UNIT_AMPERE,
)

from . import openevcharger_tlv_ns, OpenevchargerTlv

CONF_OPENEVCHARGER_TLV_ID = "openevcharger_tlv_id"
CONF_ADVERTISED_AMPS = "advertised_amps"

OpenevchargerTlvNumber = openevcharger_tlv_ns.class_(
    "OpenevchargerTlvNumber", number.Number, cg.Component
)
NumberKind = openevcharger_tlv_ns.enum("NumberKind", is_class=True)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_OPENEVCHARGER_TLV_ID): cv.use_id(OpenevchargerTlv),
        cv.Optional(CONF_ADVERTISED_AMPS): number.number_schema(
            OpenevchargerTlvNumber,
            unit_of_measurement=UNIT_AMPERE,
            device_class=DEVICE_CLASS_CURRENT,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_OPENEVCHARGER_TLV_ID])
    if conf := config.get(CONF_ADVERTISED_AMPS):
        # 6 A = J1772 minimum advertised; 48 A = OpenEVCharger MCU hw cap
        # (COMMS_HW_AMPS_MAX). DIP1 may further floor/ceil this.
        n = await number.new_number(conf, min_value=6, max_value=48, step=1)
        await cg.register_component(n, conf)
        cg.add(n.set_parent(parent))
        cg.add(n.set_kind(NumberKind.ADVERTISED_AMPS))
        cg.add(parent.register_number(n))
