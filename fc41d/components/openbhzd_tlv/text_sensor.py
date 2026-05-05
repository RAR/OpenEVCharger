import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor

from . import openevcharger_tlv_ns, OpenevchargerTlv

CONF_OPENEVCHARGER_TLV_ID = "openbhzd_tlv_id"

CONF_EVSE_STATE = "evse_state"
CONF_J1772_STATE = "j1772_state"
CONF_FIRST_FAULT = "first_fault"
CONF_BUILD_INFO = "build_info"
CONF_LAST_RFID_UID = "last_rfid_uid"
CONF_LAST_AUTH_RESULT = "last_auth_result"
CONF_LAST_REJECTED_UID = "last_rejected_uid"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_OPENEVCHARGER_TLV_ID): cv.use_id(OpenevchargerTlv),
        cv.Optional(CONF_EVSE_STATE): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_J1772_STATE): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_FIRST_FAULT): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_BUILD_INFO): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_LAST_RFID_UID): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_LAST_AUTH_RESULT): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_LAST_REJECTED_UID): text_sensor.text_sensor_schema(),
    }
)

_SETTERS = {
    CONF_EVSE_STATE: "set_evse_state_text_sensor",
    CONF_J1772_STATE: "set_j1772_state_text_sensor",
    CONF_FIRST_FAULT: "set_first_fault_text_sensor",
    CONF_BUILD_INFO: "set_build_info_text_sensor",
    CONF_LAST_RFID_UID: "set_last_rfid_uid_text_sensor",
    CONF_LAST_AUTH_RESULT: "set_last_auth_result_text_sensor",
    CONF_LAST_REJECTED_UID: "set_last_rejected_uid_text_sensor",
}


async def to_code(config):
    parent = await cg.get_variable(config[CONF_OPENEVCHARGER_TLV_ID])
    for key, setter in _SETTERS.items():
        if conf := config.get(key):
            t = await text_sensor.new_text_sensor(conf)
            cg.add(getattr(parent, setter)(t))
