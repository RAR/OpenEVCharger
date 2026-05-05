import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button

from . import openevcharger_tlv_ns, OpenevchargerTlv

CONF_OPENEVCHARGER_TLV_ID = "openbhzd_tlv_id"

CONF_PING = "ping"
CONF_REQUEST_STOP = "request_stop"
CONF_REQUEST_START_RESUME = "request_start_resume"
CONF_CLEAR_FAULT_ALL = "clear_fault_all"
CONF_REFRESH_STATE = "refresh_state"
CONF_REFRESH_BUILD_INFO = "refresh_build_info"
CONF_REFRESH_FAULT_LOG = "refresh_fault_log"
CONF_REFRESH_LIFETIME_KWH = "refresh_lifetime_kwh"
CONF_BUZZER_BEEP = "buzzer_beep"
CONF_BUZZER_MS = "buzzer_ms"
CONF_PUSH_BL0939_CAL = "push_bl0939_cal"
CONF_RFID_LEARN_NEXT = "rfid_learn_next"
CONF_RFID_CLEAR_LIST = "rfid_clear_list"
CONF_RFID_GET_LIST = "rfid_get_list"
CONF_OTA_ABORT = "ota_abort"
CONF_RESTART = "restart"

OpenevchargerTlvButton = openevcharger_tlv_ns.class_("OpenevchargerTlvButton", button.Button)
ButtonAction = openevcharger_tlv_ns.enum("ButtonAction", is_class=True)

# Map YAML key → ButtonAction enum member.
_FIELDS = {
    CONF_PING: "PING",
    CONF_REQUEST_STOP: "REQUEST_STOP",
    CONF_REQUEST_START_RESUME: "REQUEST_START_RESUME",
    CONF_CLEAR_FAULT_ALL: "CLEAR_FAULT_ALL",
    CONF_REFRESH_STATE: "GET_STATE",
    CONF_REFRESH_BUILD_INFO: "GET_BUILD_INFO",
    CONF_REFRESH_FAULT_LOG: "GET_FAULT_LOG",
    CONF_REFRESH_LIFETIME_KWH: "GET_LIFETIME_KWH",
    CONF_BUZZER_BEEP: "BUZZER_BEEP",
    CONF_PUSH_BL0939_CAL: "PUSH_BL0939_CAL",
    CONF_RFID_LEARN_NEXT: "RFID_LEARN_NEXT",
    CONF_RFID_CLEAR_LIST: "RFID_CLEAR_LIST",
    CONF_RFID_GET_LIST: "RFID_GET_LIST",
    CONF_OTA_ABORT: "OTA_ABORT",
    CONF_RESTART: "RESTART",
}

# buzzer_beep takes an optional `buzzer_ms`.
_BUZZER_SCHEMA = button.button_schema(OpenevchargerTlvButton).extend(
    {cv.Optional(CONF_BUZZER_MS, default=50): cv.int_range(min=1, max=500)}
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_OPENEVCHARGER_TLV_ID): cv.use_id(OpenevchargerTlv),
        **{
            cv.Optional(key): _BUZZER_SCHEMA
            if key == CONF_BUZZER_BEEP
            else button.button_schema(OpenevchargerTlvButton)
            for key in _FIELDS
        },
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_OPENEVCHARGER_TLV_ID])
    for key, action_name in _FIELDS.items():
        if conf := config.get(key):
            b = await button.new_button(conf)
            cg.add(b.set_parent(parent))
            cg.add(b.set_action(getattr(ButtonAction, action_name)))
            if key == CONF_BUZZER_BEEP:
                cg.add(b.set_buzzer_ms(conf[CONF_BUZZER_MS]))
            cg.add(parent.register_button(b))
