import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select

from . import openevcharger_tlv_ns, OpenevchargerTlv

CONF_OPENEVCHARGER_TLV_ID = "openevcharger_tlv_id"
CONF_GFCI_FAULT_POLICY = "gfci_fault_policy"

# Option order IS the wire value: index 0 = FAULT, 1 = WARN, 2 = IGNORE.
# Must stay in sync with GFCI_POLICY_* in openevcharger_tlv.h and
# proto/commands.h.
GFCI_POLICY_OPTIONS = ["fault", "warn", "ignore"]

OpenevchargerTlvSelect = openevcharger_tlv_ns.class_(
    "OpenevchargerTlvSelect", select.Select, cg.Component
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_OPENEVCHARGER_TLV_ID): cv.use_id(OpenevchargerTlv),
        cv.Optional(CONF_GFCI_FAULT_POLICY): select.select_schema(
            OpenevchargerTlvSelect
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_OPENEVCHARGER_TLV_ID])
    if conf := config.get(CONF_GFCI_FAULT_POLICY):
        sel = await select.new_select(conf, options=GFCI_POLICY_OPTIONS)
        await cg.register_component(sel, conf)
        cg.add(sel.set_parent(parent))
        cg.add(parent.set_gfci_policy_select(sel))
