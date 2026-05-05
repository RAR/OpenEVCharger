import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_FREQUENCY,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_VOLTAGE,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_AMPERE,
    UNIT_CELSIUS,
    UNIT_HERTZ,
    UNIT_KILOWATT_HOURS,
    UNIT_VOLT,
    UNIT_WATT,
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
CONF_GUN_NTC_TEMP = "gun_ntc_temp"
CONF_NTC1_TEMP = "ntc1_temp"
CONF_NTC2_TEMP = "ntc2_temp"
CONF_EVSE_STATE_CODE = "evse_state_code"
CONF_J1772_STATE_CODE = "j1772_state_code"
CONF_FAULT_COUNT = "fault_count"
CONF_GUN_NTC_ADC_RAW = "gun_ntc_adc_raw"
CONF_NTC1_ADC_RAW = "ntc1_adc_raw"
CONF_NTC2_ADC_RAW = "ntc2_adc_raw"
CONF_BL0939_V_RMS_RAW = "bl0939_v_rms_raw"
CONF_BL0939_IA_RMS_RAW = "bl0939_ia_rms_raw"
CONF_BL0939_IB_RMS_RAW = "bl0939_ib_rms_raw"
CONF_BL0939_A_WATT_RAW = "bl0939_a_watt_raw"
CONF_MAINS_VOLTAGE = "mains_voltage"
CONF_MAINS_CURRENT_A = "mains_current_a"
CONF_MAINS_CURRENT_B = "mains_current_b"
CONF_ACTIVE_POWER = "active_power"
CONF_MAINS_FREQUENCY = "mains_frequency"
CONF_LAST_RFID_UID = "last_rfid_uid"
CONF_RFID_AUTHLIST_COUNT = "rfid_authlist_count"
CONF_OTA_PROGRESS = "ota_progress"
CONF_BL0939_V_UV_PER_RAW = "bl0939_v_uv_per_raw"
CONF_BL0939_IA_UA_PER_RAW = "bl0939_ia_ua_per_raw"
CONF_BL0939_IB_UA_PER_RAW = "bl0939_ib_ua_per_raw"
CONF_BL0939_PA_MW_PER_RAW = "bl0939_pa_mw_per_raw"

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
        cv.Optional(CONF_GUN_NTC_TEMP): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_TEMPERATURE,
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
        cv.Optional(CONF_GUN_NTC_ADC_RAW): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_NTC1_ADC_RAW): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_NTC2_ADC_RAW): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_BL0939_V_RMS_RAW): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_BL0939_IA_RMS_RAW): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_BL0939_IB_RMS_RAW): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_BL0939_A_WATT_RAW): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_MAINS_VOLTAGE): sensor.sensor_schema(
            unit_of_measurement=UNIT_VOLT,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_VOLTAGE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_MAINS_CURRENT_A): _AMP_SCHEMA,
        cv.Optional(CONF_MAINS_CURRENT_B): _AMP_SCHEMA,
        cv.Optional(CONF_ACTIVE_POWER): sensor.sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_MAINS_FREQUENCY): sensor.sensor_schema(
            unit_of_measurement=UNIT_HERTZ,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_FREQUENCY,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        # Card UID as a u32 number — same value the stock fw sees on
        # ldr.w r1, [r0, #6]. Updated only on a card-present edge so
        # the entity stays at the most-recent UID after the card is
        # lifted off the reader.
        cv.Optional(CONF_LAST_RFID_UID): sensor.sensor_schema(
            accuracy_decimals=0,
        ),
        cv.Optional(CONF_RFID_AUTHLIST_COUNT): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        # OTA push progress percent — only meaningful while a push is
        # active. Stays at the last value otherwise.
        cv.Optional(CONF_OTA_PROGRESS): sensor.sensor_schema(
            unit_of_measurement="%",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        # Per-chassis BL0939 raw → engineering scales. Default 0 means
        # skip the engineering-unit publish (raw counts still post).
        cv.Optional(CONF_BL0939_V_UV_PER_RAW, default=0): cv.int_,
        cv.Optional(CONF_BL0939_IA_UA_PER_RAW, default=0): cv.int_,
        cv.Optional(CONF_BL0939_IB_UA_PER_RAW, default=0): cv.int_,
        cv.Optional(CONF_BL0939_PA_MW_PER_RAW, default=0): cv.int_,
    }
)

_SETTERS = {
    CONF_CP_HIGH_MV: "set_cp_high_mv_sensor",
    CONF_CP_LOW_MV: "set_cp_low_mv_sensor",
    CONF_ADVERTISED_AMPS: "set_advertised_amps_sensor",
    CONF_ACTIVE_AMPS: "set_active_amps_sensor",
    CONF_LIFETIME_KWH: "set_lifetime_kwh_sensor",
    CONF_SESSION_KWH: "set_session_kwh_sensor",
    CONF_GUN_NTC_TEMP: "set_gun_ntc_temp_sensor",
    CONF_NTC1_TEMP: "set_ntc1_temp_sensor",
    CONF_NTC2_TEMP: "set_ntc2_temp_sensor",
    CONF_EVSE_STATE_CODE: "set_evse_state_code_sensor",
    CONF_J1772_STATE_CODE: "set_j1772_state_code_sensor",
    CONF_FAULT_COUNT: "set_fault_count_sensor",
    CONF_GUN_NTC_ADC_RAW: "set_gun_ntc_adc_raw_sensor",
    CONF_NTC1_ADC_RAW: "set_ntc1_adc_raw_sensor",
    CONF_NTC2_ADC_RAW: "set_ntc2_adc_raw_sensor",
    CONF_BL0939_V_RMS_RAW: "set_bl0939_v_rms_raw_sensor",
    CONF_BL0939_IA_RMS_RAW: "set_bl0939_ia_rms_raw_sensor",
    CONF_BL0939_IB_RMS_RAW: "set_bl0939_ib_rms_raw_sensor",
    CONF_BL0939_A_WATT_RAW: "set_bl0939_a_watt_raw_sensor",
    CONF_MAINS_VOLTAGE: "set_mains_voltage_sensor",
    CONF_MAINS_CURRENT_A: "set_mains_current_a_sensor",
    CONF_MAINS_CURRENT_B: "set_mains_current_b_sensor",
    CONF_ACTIVE_POWER: "set_active_power_sensor",
    CONF_MAINS_FREQUENCY: "set_mains_frequency_sensor",
    CONF_LAST_RFID_UID: "set_last_rfid_uid_sensor",
    CONF_RFID_AUTHLIST_COUNT: "set_rfid_authlist_count_sensor",
    CONF_OTA_PROGRESS: "set_ota_progress_sensor",
}

_SCALE_SETTERS = {
    CONF_BL0939_V_UV_PER_RAW: "set_bl0939_v_uv_per_raw",
    CONF_BL0939_IA_UA_PER_RAW: "set_bl0939_ia_ua_per_raw",
    CONF_BL0939_IB_UA_PER_RAW: "set_bl0939_ib_ua_per_raw",
    CONF_BL0939_PA_MW_PER_RAW: "set_bl0939_pa_mw_per_raw",
}


async def to_code(config):
    parent = await cg.get_variable(config[CONF_OPENBHZD_TLV_ID])
    for key, setter in _SETTERS.items():
        if conf := config.get(key):
            s = await sensor.new_sensor(conf)
            cg.add(getattr(parent, setter)(s))
    for key, setter in _SCALE_SETTERS.items():
        cg.add(getattr(parent, setter)(config[key]))
