import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_VOLTAGE,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_AMPERE,
    UNIT_CELSIUS,
    UNIT_KILOWATT_HOURS,
    UNIT_VOLT,
)

from . import openbhzd_tlv_ns, OpenbhzdTlv

CONF_OPENBHZD_TLV_ID = "openbhzd_tlv_id"

# CP voltages are reported in mV; expose as raw mV for clarity (not V) since
# the J1772 PWM swings ±12 V and "12000 mV" is the natural unit.
CONF_CP_HIGH_MV = "cp_high_mv"
CONF_CP_LOW_MV = "cp_low_mv"
CONF_ADVERTISED_AMPS = "advertised_amps"
CONF_ACTIVE_AMPS = "active_amps"
CONF_LIFETIME_KWH = "lifetime_kwh"
CONF_SESSION_KWH = "session_kwh"
CONF_NTC1_TEMP = "ntc1_temp"
CONF_NTC2_TEMP = "ntc2_temp"
CONF_EVSE_STATE_CODE = "evse_state_code"
CONF_J1772_STATE_CODE = "j1772_state_code"
CONF_FAULT_COUNT = "fault_count"

UNIT_MILLIVOLT = "mV"

_AMP_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_AMPERE,
    accuracy_decimals=1,
    device_class=DEVICE_CLASS_CURRENT,
    state_class=STATE_CLASS_MEASUREMENT,
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_OPENBHZD_TLV_ID): cv.use_id(OpenbhzdTlv),
        cv.Optional(CONF_CP_HIGH_MV): sensor.sensor_schema(
            unit_of_measurement=UNIT_MILLIVOLT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_VOLTAGE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_CP_LOW_MV): sensor.sensor_schema(
            unit_of_measurement=UNIT_MILLIVOLT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_VOLTAGE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_ADVERTISED_AMPS): _AMP_SCHEMA,
        cv.Optional(CONF_ACTIVE_AMPS): _AMP_SCHEMA,
        cv.Optional(CONF_LIFETIME_KWH): sensor.sensor_schema(
            unit_of_measurement=UNIT_KILOWATT_HOURS,
            accuracy_decimals=3,
            device_class=DEVICE_CLASS_ENERGY,
            state_class=STATE_CLASS_TOTAL_INCREASING,
        ),
        cv.Optional(CONF_SESSION_KWH): sensor.sensor_schema(
            unit_of_measurement=UNIT_KILOWATT_HOURS,
            accuracy_decimals=3,
            device_class=DEVICE_CLASS_ENERGY,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_NTC1_TEMP): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_NTC2_TEMP): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_EVSE_STATE_CODE): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_J1772_STATE_CODE): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_FAULT_COUNT): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    }
)

_SETTERS = {
    CONF_CP_HIGH_MV: "set_cp_high_mv_sensor",
    CONF_CP_LOW_MV: "set_cp_low_mv_sensor",
    CONF_ADVERTISED_AMPS: "set_advertised_amps_sensor",
    CONF_ACTIVE_AMPS: "set_active_amps_sensor",
    CONF_LIFETIME_KWH: "set_lifetime_kwh_sensor",
    CONF_SESSION_KWH: "set_session_kwh_sensor",
    CONF_NTC1_TEMP: "set_ntc1_temp_sensor",
    CONF_NTC2_TEMP: "set_ntc2_temp_sensor",
    CONF_EVSE_STATE_CODE: "set_evse_state_code_sensor",
    CONF_J1772_STATE_CODE: "set_j1772_state_code_sensor",
    CONF_FAULT_COUNT: "set_fault_count_sensor",
}


async def to_code(config):
    parent = await cg.get_variable(config[CONF_OPENBHZD_TLV_ID])
    for key, setter in _SETTERS.items():
        if conf := config.get(key):
            s = await sensor.new_sensor(conf)
            cg.add(getattr(parent, setter)(s))
