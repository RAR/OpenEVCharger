import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import (
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_PLUG,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_PROBLEM,
)

from . import openbhzd_tlv_ns, OpenbhzdTlv

CONF_OPENBHZD_TLV_ID = "openbhzd_tlv_id"

CONF_LINK_UP = "link_up"
CONF_VEHICLE_CONNECTED = "vehicle_connected"
CONF_CHARGING = "charging"
CONF_AC_PRESENT = "ac_present"
CONF_FAULT_ACTIVE = "fault_active"
CONF_CONTACTOR_CMD = "contactor_cmd"
CONF_RFID_PRESENT = "rfid_present"
CONF_RFID_LAST_ACCEPTED = "rfid_last_accepted"
CONF_RFID_SESSION_AUTHORIZED = "rfid_session_authorized"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_OPENBHZD_TLV_ID): cv.use_id(OpenbhzdTlv),
        cv.Optional(CONF_LINK_UP): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY,
        ),
        cv.Optional(CONF_VEHICLE_CONNECTED): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_PLUG,
        ),
        cv.Optional(CONF_CHARGING): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_POWER,
        ),
        cv.Optional(CONF_AC_PRESENT): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_POWER,
        ),
        cv.Optional(CONF_FAULT_ACTIVE): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_PROBLEM,
        ),
        cv.Optional(CONF_CONTACTOR_CMD): binary_sensor.binary_sensor_schema(),
        cv.Optional(CONF_RFID_PRESENT): binary_sensor.binary_sensor_schema(),
        cv.Optional(CONF_RFID_LAST_ACCEPTED): binary_sensor.binary_sensor_schema(),
        cv.Optional(CONF_RFID_SESSION_AUTHORIZED): binary_sensor.binary_sensor_schema(),
    }
)

_SETTERS = {
    CONF_LINK_UP: "set_link_up_bsensor",
    CONF_VEHICLE_CONNECTED: "set_vehicle_connected_bsensor",
    CONF_CHARGING: "set_charging_bsensor",
    CONF_AC_PRESENT: "set_ac_present_bsensor",
    CONF_FAULT_ACTIVE: "set_fault_active_bsensor",
    CONF_CONTACTOR_CMD: "set_contactor_cmd_bsensor",
    CONF_RFID_PRESENT: "set_rfid_present_bsensor",
    CONF_RFID_LAST_ACCEPTED: "set_rfid_last_accepted_bsensor",
    CONF_RFID_SESSION_AUTHORIZED: "set_session_authorized_bsensor",
}


async def to_code(config):
    parent = await cg.get_variable(config[CONF_OPENBHZD_TLV_ID])
    for key, setter in _SETTERS.items():
        if conf := config.get(key):
            b = await binary_sensor.new_binary_sensor(conf)
            cg.add(getattr(parent, setter)(b))
