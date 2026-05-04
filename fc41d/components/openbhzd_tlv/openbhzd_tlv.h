#pragma once

// openbhzd_tlv — ESPHome side of the OpenBHZD MCU TLV protocol.
//
// Transport: 115200 8N1 over a UART. On the FC41D (BK7231N) board the
// MCU's UART4 (PC12 TX / PD2 RX) is wired to the BK's UART1 (P10 RX /
// P11 TX). On a desktop dev rig, any USB-UART works.
//
// Frame format and command IDs match ../../../src/proto/tlv.h and
// ../../../src/proto/commands.h. STATE_REPORT payload layout matches
// ../../../src/core/system_state.h openbhzd_state (packed, 30 bytes).
// Keep these in sync if either side changes.

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"

#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace openbhzd_tlv {

// FC41D → MCU requests
static constexpr uint8_t CMD_PING = 0x01;
static constexpr uint8_t CMD_GET_STATE = 0x02;
static constexpr uint8_t CMD_SET_ADVERTISED_AMPS = 0x03;
static constexpr uint8_t CMD_REQUEST_STOP = 0x04;
static constexpr uint8_t CMD_REQUEST_START_RESUME = 0x05;
static constexpr uint8_t CMD_CLEAR_FAULT = 0x06;
static constexpr uint8_t CMD_GET_FAULT_LOG = 0x07;
static constexpr uint8_t CMD_GET_LIFETIME_KWH = 0x08;
static constexpr uint8_t CMD_WRITE_CALIBRATION = 0x09;
static constexpr uint8_t CMD_SET_LED_OVERRIDE = 0x0A;
static constexpr uint8_t CMD_BUZZER_BEEP = 0x0B;
static constexpr uint8_t CMD_GET_BUILD_INFO = 0x0C;
static constexpr uint8_t CMD_GET_DEVICE_ID = 0x0D;
static constexpr uint8_t CMD_WRITE_BL0939_CAL = 0x0E;

// MCU → FC41D events / responses (bit 7 set)
static constexpr uint8_t EVT_STATE_CHANGED = 0x80;
static constexpr uint8_t EVT_PING_ACK = 0x81;
static constexpr uint8_t EVT_STATE_REPORT = 0x82;
static constexpr uint8_t EVT_FAULT_RAISED = 0x83;
static constexpr uint8_t EVT_FAULT_CLEARED = 0x84;
static constexpr uint8_t EVT_SESSION_BEGAN = 0x85;
static constexpr uint8_t EVT_SESSION_ENDED = 0x86;
static constexpr uint8_t EVT_BOOT_COMPLETE = 0x87;
static constexpr uint8_t EVT_FAULT_LOG_ENTRY = 0x88;
static constexpr uint8_t EVT_FAULT_LOG_END = 0x89;
static constexpr uint8_t EVT_LIFETIME_KWH = 0x8A;
static constexpr uint8_t EVT_BUILD_INFO = 0x8C;
static constexpr uint8_t EVT_DEVICE_ID = 0x8D;
static constexpr uint8_t EVT_RFID_SWIPE = 0x8E;

// Mirrors src/core/system_state.h. Keep in sync if the MCU schema changes.
struct StateReport {
  uint8_t j1772_state{0};
  uint8_t evse_state{0};
  uint8_t advertised_amps{0};
  uint8_t contactor_cmd{0};
  int16_t cp_high_mv{0};
  int16_t cp_low_mv{0};
  uint16_t active_amps_x10{0};
  uint16_t ntc1_dC{0};
  uint16_t ntc2_dC{0};
  uint16_t cc_max_amps{0};
  uint8_t ac_present{0};
  uint8_t pad{0};
  uint32_t fault_active_bits{0};
  uint32_t first_fault_id{0};
  uint32_t session_mwh{0};
  // Channel-role correction 2026-05-03: PA2 is the gun-cable NTC,
  // not mains voltage; PA3 is the wall-plug NTC; PB0 is NOT a
  // thermistor (AC-presence sense — see MCU pin_map.h). Field name
  // ac_adc_raw was the early misnomer; semantic role is gun NTC.
  uint16_t gun_ntc_adc_raw{0};  // PA2 ADC rank 0 — gun-cable NTC raw
  uint16_t ntc1_adc_raw{0};     // PA3 — wall-plug NTC raw
  uint16_t ntc2_adc_raw{0};     // PB0 — non-thermistor; raw exposed for diag
  // BL0939 metering chip — raw 24-bit register reads. Engineering-unit
  // conversion (V/A/W) requires per-chassis calibration scales which
  // currently live in MCU boot_config; until those are populated we
  // surface raw counts only. v_rms/ia_rms/ib_rms are unsigned, a_watt
  // is sign-extended (signed power flow).
  uint32_t bl0939_v_rms{0};
  uint32_t bl0939_ia_rms{0};
  uint32_t bl0939_ib_rms{0};
  int32_t bl0939_a_watt{0};
  uint16_t bl0939_freq_hz_x10{0};
  bool bl0939_valid{false};
  bool valid{false};
};

class OpenbhzdTlvButton;
class OpenbhzdTlvNumber;

class OpenbhzdTlv : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  // AFTER_WIFI (not BUS) so our setup() runs once WiFi has been put
  // into STA mode by the wifi component. WiFi.setMacAddress() only
  // triggers the radio re-init that propagates the new MAC down to
  // the BK SDK if WiFi mode is non-NULL when it's called.
  float get_setup_priority() const override {
    return setup_priority::AFTER_WIFI;
  }

  void set_poll_interval(uint32_t ms) { poll_interval_ms_ = ms; }
  void set_link_timeout(uint32_t ms) { link_timeout_ms_ = ms; }

  // Outbound commands (HA → MCU). seq is auto-incremented.
  uint8_t send_ping();
  uint8_t send_get_state();
  uint8_t send_get_build_info();
  uint8_t send_get_device_id();
  uint8_t send_set_advertised_amps(uint8_t amps);
  uint8_t send_request_stop();
  uint8_t send_request_start_resume();
  uint8_t send_clear_fault(uint32_t fault_id);
  uint8_t send_get_fault_log(uint8_t max_count);
  uint8_t send_get_lifetime_kwh();
  uint8_t send_buzzer_beep(uint16_t ms);
  uint8_t send_set_led_override(uint8_t mode, uint8_t r, uint8_t g, uint8_t b);
  uint8_t send_write_bl0939_cal(int16_t v_uv_per_raw,
                                int16_t ia_ua_per_raw,
                                int16_t ib_ua_per_raw,
                                int16_t pa_mw_per_raw);
  // Convenience: ship the four per-chassis scales already loaded from
  // YAML over to the MCU. Clamps to int16 range. Lets HA "Push BL0939
  // Calibration" button persist the YAML-configured values without
  // re-flashing the MCU.
  uint8_t send_write_bl0939_cal_from_yaml();

  // Cached state.
  const StateReport &state() const { return state_; }
  bool link_up() const;
  uint32_t lifetime_mwh() const { return lifetime_mwh_; }
  const std::string &build_info() const { return build_info_; }
  const std::string &first_fault_name() const { return first_fault_name_; }
  const std::string &last_rfid_uid_str() const { return last_rfid_uid_str_; }
  uint32_t last_rfid_uid_u32() const { return last_rfid_uid_u32_; }
  bool last_rfid_present() const { return last_rfid_present_; }
  uint32_t state_age_ms() const;

#ifdef USE_SENSOR
  void set_cp_high_mv_sensor(sensor::Sensor *s) { cp_high_mv_sensor_ = s; }
  void set_cp_low_mv_sensor(sensor::Sensor *s) { cp_low_mv_sensor_ = s; }
  void set_advertised_amps_sensor(sensor::Sensor *s) { advertised_amps_sensor_ = s; }
  void set_active_amps_sensor(sensor::Sensor *s) { active_amps_sensor_ = s; }
  void set_lifetime_kwh_sensor(sensor::Sensor *s) { lifetime_kwh_sensor_ = s; }
  void set_session_kwh_sensor(sensor::Sensor *s) { session_kwh_sensor_ = s; }
  void set_ntc1_temp_sensor(sensor::Sensor *s) { ntc1_temp_sensor_ = s; }
  void set_ntc2_temp_sensor(sensor::Sensor *s) { ntc2_temp_sensor_ = s; }
  void set_evse_state_code_sensor(sensor::Sensor *s) { evse_state_code_sensor_ = s; }
  void set_j1772_state_code_sensor(sensor::Sensor *s) { j1772_state_code_sensor_ = s; }
  void set_fault_count_sensor(sensor::Sensor *s) { fault_count_sensor_ = s; }
  void set_gun_ntc_adc_raw_sensor(sensor::Sensor *s) { gun_ntc_adc_raw_sensor_ = s; }
  void set_gun_ntc_temp_sensor(sensor::Sensor *s) { gun_ntc_temp_sensor_ = s; }
  void set_ntc1_adc_raw_sensor(sensor::Sensor *s) { ntc1_adc_raw_sensor_ = s; }
  void set_ntc2_adc_raw_sensor(sensor::Sensor *s) { ntc2_adc_raw_sensor_ = s; }
  void set_bl0939_v_rms_raw_sensor(sensor::Sensor *s) { bl0939_v_rms_raw_sensor_ = s; }
  void set_bl0939_ia_rms_raw_sensor(sensor::Sensor *s) { bl0939_ia_rms_raw_sensor_ = s; }
  void set_bl0939_ib_rms_raw_sensor(sensor::Sensor *s) { bl0939_ib_rms_raw_sensor_ = s; }
  void set_bl0939_a_watt_raw_sensor(sensor::Sensor *s) { bl0939_a_watt_raw_sensor_ = s; }
  void set_mains_voltage_sensor(sensor::Sensor *s) { mains_voltage_sensor_ = s; }
  void set_mains_current_a_sensor(sensor::Sensor *s) { mains_current_a_sensor_ = s; }
  void set_mains_current_b_sensor(sensor::Sensor *s) { mains_current_b_sensor_ = s; }
  void set_active_power_sensor(sensor::Sensor *s) { active_power_sensor_ = s; }
  void set_mains_frequency_sensor(sensor::Sensor *s) { mains_frequency_sensor_ = s; }
  // Per-chassis BL0939 raw → engineering-unit scales. Pulled from YAML
  // substitutions; default 0 = no conversion (raw-only mode).
  void set_bl0939_v_uv_per_raw(int32_t s) { bl0939_v_uv_per_raw_ = s; }
  void set_bl0939_ia_ua_per_raw(int32_t s) { bl0939_ia_ua_per_raw_ = s; }
  void set_bl0939_ib_ua_per_raw(int32_t s) { bl0939_ib_ua_per_raw_ = s; }
  void set_bl0939_pa_mw_per_raw(int32_t s) { bl0939_pa_mw_per_raw_ = s; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_link_up_bsensor(binary_sensor::BinarySensor *s) { link_up_bsensor_ = s; }
  void set_vehicle_connected_bsensor(binary_sensor::BinarySensor *s) {
    vehicle_connected_bsensor_ = s;
  }
  void set_charging_bsensor(binary_sensor::BinarySensor *s) { charging_bsensor_ = s; }
  void set_ac_present_bsensor(binary_sensor::BinarySensor *s) { ac_present_bsensor_ = s; }
  void set_fault_active_bsensor(binary_sensor::BinarySensor *s) { fault_active_bsensor_ = s; }
  void set_contactor_cmd_bsensor(binary_sensor::BinarySensor *s) {
    contactor_cmd_bsensor_ = s;
  }
#endif
#ifdef USE_TEXT_SENSOR
  void set_evse_state_text_sensor(text_sensor::TextSensor *s) { evse_state_tsensor_ = s; }
  void set_j1772_state_text_sensor(text_sensor::TextSensor *s) { j1772_state_tsensor_ = s; }
  void set_first_fault_text_sensor(text_sensor::TextSensor *s) { first_fault_tsensor_ = s; }
  void set_build_info_text_sensor(text_sensor::TextSensor *s) { build_info_tsensor_ = s; }
  void set_last_rfid_uid_text_sensor(text_sensor::TextSensor *s) { last_rfid_uid_tsensor_ = s; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_rfid_present_bsensor(binary_sensor::BinarySensor *s) { rfid_present_bsensor_ = s; }
#endif

  // Sub-entity registry — number/button platforms call these in to_code().
#ifdef USE_NUMBER
  void register_number(OpenbhzdTlvNumber *n) { numbers_.push_back(n); }
#endif
#ifdef USE_BUTTON
  void register_button(OpenbhzdTlvButton *b) { buttons_.push_back(b); }
#endif

  static const char *evse_state_name(uint8_t s);
  static const char *j1772_state_name(uint8_t s);
  static const char *fault_name(uint32_t id);

 protected:
  void process_byte_(uint8_t b);
  void try_parse_();
  void dispatch_frame_(uint8_t cmd, uint8_t seq, const uint8_t *payload, size_t plen);

  void publish_state_();
  void send_frame_(uint8_t cmd, uint8_t seq, const void *payload, size_t plen);
  uint8_t next_seq_();

  static uint16_t crc16_ccitt_(const uint8_t *p, size_t n);

  uint32_t poll_interval_ms_{5000};
  uint32_t link_timeout_ms_{15000};
  uint32_t last_poll_ms_{0};
  uint32_t last_state_ms_{0};
  uint8_t seq_{1};

  std::vector<uint8_t> rx_buf_;
  StateReport state_;
  uint32_t lifetime_mwh_{0};
  std::string build_info_;
  std::string first_fault_name_;
  uint32_t fault_count_{0};
  bool last_link_up_{false};
  bool mac_overridden_{false};
  bool mac_status_logged_{false};
  bool mac_set_rc_{false};
  uint8_t mac_before_[6]{};
  uint8_t mac_after_[6]{};

  void apply_mac_override_(const uint8_t mac[6]);
  void maybe_log_mac_status_();

#ifdef USE_SENSOR
  sensor::Sensor *cp_high_mv_sensor_{nullptr};
  sensor::Sensor *cp_low_mv_sensor_{nullptr};
  sensor::Sensor *advertised_amps_sensor_{nullptr};
  sensor::Sensor *active_amps_sensor_{nullptr};
  sensor::Sensor *lifetime_kwh_sensor_{nullptr};
  sensor::Sensor *session_kwh_sensor_{nullptr};
  sensor::Sensor *ntc1_temp_sensor_{nullptr};
  sensor::Sensor *ntc2_temp_sensor_{nullptr};
  sensor::Sensor *evse_state_code_sensor_{nullptr};
  sensor::Sensor *j1772_state_code_sensor_{nullptr};
  sensor::Sensor *fault_count_sensor_{nullptr};
  sensor::Sensor *gun_ntc_adc_raw_sensor_{nullptr};
  sensor::Sensor *gun_ntc_temp_sensor_{nullptr};
  sensor::Sensor *ntc1_adc_raw_sensor_{nullptr};
  sensor::Sensor *ntc2_adc_raw_sensor_{nullptr};
  sensor::Sensor *bl0939_v_rms_raw_sensor_{nullptr};
  sensor::Sensor *bl0939_ia_rms_raw_sensor_{nullptr};
  sensor::Sensor *bl0939_ib_rms_raw_sensor_{nullptr};
  sensor::Sensor *bl0939_a_watt_raw_sensor_{nullptr};
  sensor::Sensor *mains_voltage_sensor_{nullptr};
  sensor::Sensor *mains_current_a_sensor_{nullptr};
  sensor::Sensor *mains_current_b_sensor_{nullptr};
  sensor::Sensor *active_power_sensor_{nullptr};
  sensor::Sensor *mains_frequency_sensor_{nullptr};
  int32_t bl0939_v_uv_per_raw_{0};
  int32_t bl0939_ia_ua_per_raw_{0};
  int32_t bl0939_ib_ua_per_raw_{0};
  int32_t bl0939_pa_mw_per_raw_{0};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *link_up_bsensor_{nullptr};
  binary_sensor::BinarySensor *vehicle_connected_bsensor_{nullptr};
  binary_sensor::BinarySensor *charging_bsensor_{nullptr};
  binary_sensor::BinarySensor *ac_present_bsensor_{nullptr};
  binary_sensor::BinarySensor *fault_active_bsensor_{nullptr};
  binary_sensor::BinarySensor *contactor_cmd_bsensor_{nullptr};
  binary_sensor::BinarySensor *rfid_present_bsensor_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *evse_state_tsensor_{nullptr};
  text_sensor::TextSensor *j1772_state_tsensor_{nullptr};
  text_sensor::TextSensor *first_fault_tsensor_{nullptr};
  text_sensor::TextSensor *build_info_tsensor_{nullptr};
  text_sensor::TextSensor *last_rfid_uid_tsensor_{nullptr};
#endif
  std::string last_rfid_uid_str_;
  uint32_t    last_rfid_uid_u32_{0};
  bool        last_rfid_present_{false};
#ifdef USE_NUMBER
  std::vector<OpenbhzdTlvNumber *> numbers_;
#endif
#ifdef USE_BUTTON
  std::vector<OpenbhzdTlvButton *> buttons_;
#endif
};

// Number kinds — currently only advertised_amps. Kept as enum to make adding
// future writable parameters (e.g. cp_anchor calibration) drop-in.
enum class NumberKind : uint8_t {
  ADVERTISED_AMPS,
};

// Button actions. These are one-shots that call the parent's send_*().
enum class ButtonAction : uint8_t {
  PING,
  REQUEST_STOP,
  REQUEST_START_RESUME,
  CLEAR_FAULT_ALL,
  GET_STATE,
  GET_BUILD_INFO,
  GET_FAULT_LOG,
  GET_LIFETIME_KWH,
  BUZZER_BEEP,
  PUSH_BL0939_CAL,
};

#ifdef USE_NUMBER
class OpenbhzdTlvNumber : public number::Number, public Component {
 public:
  void set_parent(OpenbhzdTlv *p) { parent_ = p; }
  void set_kind(NumberKind k) { kind_ = k; }
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  void publish_from_state();

 protected:
  void control(float value) override;
  OpenbhzdTlv *parent_{nullptr};
  NumberKind kind_{NumberKind::ADVERTISED_AMPS};
};
#endif

#ifdef USE_BUTTON
class OpenbhzdTlvButton : public button::Button {
 public:
  void set_parent(OpenbhzdTlv *p) { parent_ = p; }
  void set_action(ButtonAction a) { action_ = a; }
  void set_buzzer_ms(uint16_t ms) { buzzer_ms_ = ms; }

 protected:
  void press_action() override;
  OpenbhzdTlv *parent_{nullptr};
  ButtonAction action_{ButtonAction::PING};
  uint16_t buzzer_ms_{50};
};
#endif

}  // namespace openbhzd_tlv
}  // namespace esphome
