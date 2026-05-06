#pragma once

// openevcharger_tlv — ESPHome side of the OpenEVCharger MCU TLV protocol.
//
// Transport: 115200 8N1 over a UART. On the FC41D (BK7231N) board the
// MCU's UART4 (PC12 TX / PD2 RX) is wired to the BK's UART1 (P10 RX /
// P11 TX). On a desktop dev rig, any USB-UART works.
//
// Frame format and command IDs match ../../../src/proto/tlv.h and
// ../../../src/proto/commands.h. STATE_REPORT payload layout matches
// ../../../src/core/system_state.h openevcharger_state (packed, 30 bytes).
// Keep these in sync if either side changes.

#include "esphome/core/automation.h"
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
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
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
namespace openevcharger_tlv {

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
static constexpr uint8_t CMD_RFID_LEARN_NEXT = 0x0F;
static constexpr uint8_t CMD_RFID_REMOVE_UID = 0x10;
static constexpr uint8_t CMD_RFID_CLEAR_LIST = 0x11;
static constexpr uint8_t CMD_RFID_GET_LIST = 0x12;
static constexpr uint8_t CMD_SET_REQUIRE_RFID_AUTH = 0x13;
static constexpr uint8_t CMD_GET_RFID_CONFIG = 0x14;
static constexpr uint8_t CMD_OTA_BEGIN  = 0x15;
static constexpr uint8_t CMD_OTA_CHUNK  = 0x16;
static constexpr uint8_t CMD_OTA_COMMIT = 0x17;
static constexpr uint8_t CMD_OTA_ABORT  = 0x18;
static constexpr uint8_t CMD_SET_TIME   = 0x19;
static constexpr uint8_t CMD_GET_TIME   = 0x1A;
static constexpr uint8_t CMD_RESTART    = 0x1B;
static constexpr uint8_t CMD_SIMULATE_REPLUG = 0x1C;
static constexpr uint8_t CMD_RUN_GFCI_CAL_TEST = 0x1E;  // 0x1D reserved (was relay actuate, now N/A)

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
static constexpr uint8_t EVT_RFID_AUTH_RESULT = 0x8F;
static constexpr uint8_t EVT_RFID_LIST_ENTRY = 0x90;
static constexpr uint8_t EVT_RFID_LIST_END = 0x91;
static constexpr uint8_t EVT_RFID_CONFIG = 0x92;
static constexpr uint8_t EVT_OTA_BEGIN_ACK = 0x93;
static constexpr uint8_t EVT_OTA_CHUNK_ACK = 0x94;
static constexpr uint8_t EVT_OTA_COMMITTED = 0x95;
static constexpr uint8_t EVT_OTA_ABORTED   = 0x96;
static constexpr uint8_t EVT_TIME          = 0x97;

// OTA status codes (mirror src/proto/commands.h).
static constexpr uint8_t OTA_STATUS_OK              = 0;
static constexpr uint8_t OTA_STATUS_SESSION_INVALID = 1;
static constexpr uint8_t OTA_STATUS_OFFSET_MISMATCH = 2;
static constexpr uint8_t OTA_STATUS_WRITE_ERROR     = 3;
static constexpr uint8_t OTA_STATUS_OVERSIZE        = 4;
static constexpr uint8_t OTA_STATUS_TOO_LARGE       = 5;
static constexpr uint8_t OTA_STATUS_ERASE_FAIL      = 6;
static constexpr uint8_t OTA_STATUS_INVALID_PAYLOAD = 7;
static constexpr uint8_t OTA_STATUS_NO_SESSION      = 8;
static constexpr uint8_t OTA_STATUS_CRC_MISMATCH    = 9;
static constexpr uint8_t OTA_STATUS_PERSIST_FAIL    = 10;

// Max chunk data bytes per CMD_OTA_CHUNK (matches MCU OTA_CHUNK_MAX_DATA).
static constexpr size_t OTA_CHUNK_MAX_DATA = 48;
// Hard cap mirrors MCU's FLASH_OTA_APPLY_MAX_SIZE.
static constexpr size_t OTA_PUSH_MAX_IMAGE_BYTES = 64u * 1024u;

// RFID auth-result codes (mirror src/proto/commands.h).
static constexpr uint8_t RFID_AUTH_RESULT_LEARNED = 0;
static constexpr uint8_t RFID_AUTH_RESULT_START = 1;
static constexpr uint8_t RFID_AUTH_RESULT_STOP = 2;
static constexpr uint8_t RFID_AUTH_RESULT_MATCHED_NOOP = 3;
static constexpr uint8_t RFID_AUTH_RESULT_REJECTED = 4;
static constexpr uint8_t RFID_AUTH_RESULT_LIST_FULL = 5;

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

class OpenevchargerTlvButton;
class OpenevchargerTlvNumber;

class OpenevchargerTlv : public Component, public uart::UARTDevice {
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
                                int16_t pa_uw_per_raw);
  // Convenience: ship the four per-chassis scales already loaded from
  // YAML over to the MCU. Clamps to int16 range. Lets HA "Push BL0939
  // Calibration" button persist the YAML-configured values without
  // re-flashing the MCU.
  uint8_t send_write_bl0939_cal_from_yaml();
  uint8_t send_rfid_learn_next();
  uint8_t send_rfid_clear_list();
  uint8_t send_rfid_get_list();
  uint8_t send_rfid_remove_uid(uint32_t uid);
  uint8_t send_set_require_rfid_auth(bool enable);
  uint8_t send_get_rfid_config();

  // Push the current HA wall-clock to the MCU. Caller must check the
  // time component's is_valid() before calling — sending 0 is allowed
  // but means "clear". Returns the seq used.
  uint8_t send_set_time(uint32_t unix_seconds);
  uint8_t send_get_time();
  uint8_t send_restart();
  uint8_t send_simulate_replug();
  uint8_t send_run_gfci_cal_test();
  // Cached MCU clock (last value reported via EVT_TIME, with the
  // FC41D millis at receipt — caller can extrapolate live by adding
  // the millis-since delta).
  uint32_t mcu_unix_seconds() const { return mcu_unix_seconds_; }
  uint32_t mcu_unix_seconds_recv_ms() const { return mcu_unix_seconds_recv_ms_; }
  bool     mcu_time_is_set() const { return mcu_time_is_set_; }

  // --- OTA push (FC41D → MCU TLV upload) -----------------------------
  // start_ota_push() copies the image into an internal buffer, picks a
  // session_id, sends CMD_OTA_BEGIN, and from there the loop()-driven
  // state machine streams CMD_OTA_CHUNKs in lockstep with the MCU's
  // EVT_OTA_CHUNK_ACK responses. On the final ACK it issues
  // CMD_OTA_COMMIT and the MCU verifies CRC32 + arms its pending-OTA
  // flag. The MCU reboots into the new image only after a separate
  // user-triggered reset (see openevcharger.yaml's "Reboot MCU After OTA"
  // button in step 6).
  //
  // Returns true if the push was queued, false if size is out of range
  // or another push is already in progress.
  bool start_ota_push(const uint8_t *data, size_t len);
  // Fetch a binary over plain HTTP and hand it to start_ota_push.
  // url must be of the form http://host[:port]/path  (HTTPS not
  // supported — BK7231N's TLS isn't in this build). Pass HA's
  // /local/<file> URL after dropping the .bin into /config/www/.
  // Returns true if the push was queued.
  bool fetch_and_push_ota(const std::string &url);
  void abort_ota_push();
  bool ota_push_active() const { return ota_state_ != OtaState::IDLE &&
                                        ota_state_ != OtaState::DONE  &&
                                        ota_state_ != OtaState::FAILED; }
  uint8_t ota_progress_pct() const;
  const char *ota_state_name() const;
  uint8_t ota_last_status() const { return ota_last_status_; }
  // Low-level send. Returns the seq used. data may be null for COMMIT/ABORT.
  uint8_t send_ota_begin(uint32_t image_size, uint32_t image_crc32,
                         uint32_t session_id);
  uint8_t send_ota_chunk(uint32_t session_id, uint32_t offset,
                         const uint8_t *data, uint8_t data_len);
  uint8_t send_ota_commit(uint32_t session_id);
  uint8_t send_ota_abort(uint32_t session_id);

  // Automation hook: fires for every EVT_RFID_AUTH_RESULT the MCU
  // emits, with the colon-formatted UID string + the raw result code
  // (RFID_AUTH_RESULT_*). Lets YAML wire arbitrary side effects —
  // e.g. driving an OCPP transaction lifecycle via the ocpp component
  // — without baking that knowledge into openevcharger_tlv itself.
  void add_on_rfid_auth_result_callback(
      std::function<void(std::string, uint8_t)> &&cb) {
    rfid_auth_result_callbacks_.push_back(std::move(cb));
  }

  // Cached state.
  const StateReport &state() const { return state_; }
  bool link_up() const;
  uint32_t lifetime_mwh() const { return lifetime_mwh_; }
  const std::string &build_info() const { return build_info_; }
  const std::string &first_fault_name() const { return first_fault_name_; }
  const std::string &last_rfid_uid_str() const { return last_rfid_uid_str_; }
  uint32_t last_rfid_uid_u32() const { return last_rfid_uid_u32_; }
  bool last_rfid_present() const { return last_rfid_present_; }
  uint8_t rfid_authlist_count() const { return rfid_authlist_count_; }
  uint8_t last_rfid_auth_result() const { return last_rfid_auth_result_; }
  uint32_t last_rfid_auth_uid() const { return last_rfid_auth_uid_; }
  bool require_rfid_auth() const { return require_rfid_auth_; }
  bool session_authorized() const { return session_authorized_; }
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
  void set_last_rfid_uid_sensor(sensor::Sensor *s) { last_rfid_uid_sensor_ = s; }
  void set_rfid_authlist_count_sensor(sensor::Sensor *s) { rfid_authlist_count_sensor_ = s; }
  void set_ota_progress_sensor(sensor::Sensor *s) { ota_progress_sensor_ = s; }
  // Per-chassis BL0939 raw → engineering-unit scales. Pulled from YAML
  // substitutions; default 0 = no conversion (raw-only mode).
  void set_bl0939_v_uv_per_raw(int32_t s) { bl0939_v_uv_per_raw_ = s; }
  void set_bl0939_ia_ua_per_raw(int32_t s) { bl0939_ia_ua_per_raw_ = s; }
  void set_bl0939_ib_ua_per_raw(int32_t s) { bl0939_ib_ua_per_raw_ = s; }
  void set_bl0939_pa_uw_per_raw(int32_t s) { bl0939_pa_uw_per_raw_ = s; }
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
  void set_rfid_last_accepted_bsensor(binary_sensor::BinarySensor *s) { rfid_last_accepted_bsensor_ = s; }
  void set_session_authorized_bsensor(binary_sensor::BinarySensor *s) { session_authorized_bsensor_ = s; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_last_auth_result_text_sensor(text_sensor::TextSensor *s) { last_auth_result_tsensor_ = s; }
  void set_last_rejected_uid_text_sensor(text_sensor::TextSensor *s) { last_rejected_uid_tsensor_ = s; }
#endif

  // Sub-entity registry — number/button platforms call these in to_code().
#ifdef USE_NUMBER
  void register_number(OpenevchargerTlvNumber *n) { numbers_.push_back(n); }
#endif
#ifdef USE_BUTTON
  void register_button(OpenevchargerTlvButton *b) { buttons_.push_back(b); }
#endif
#ifdef USE_SWITCH
  void set_require_rfid_auth_switch(class OpenevchargerTlvSwitch *s) { require_rfid_auth_switch_ = s; }
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
  sensor::Sensor *last_rfid_uid_sensor_{nullptr};
  sensor::Sensor *rfid_authlist_count_sensor_{nullptr};
  sensor::Sensor *ota_progress_sensor_{nullptr};
  int32_t bl0939_v_uv_per_raw_{0};
  int32_t bl0939_ia_ua_per_raw_{0};
  int32_t bl0939_ib_ua_per_raw_{0};
  int32_t bl0939_pa_uw_per_raw_{0};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *link_up_bsensor_{nullptr};
  binary_sensor::BinarySensor *vehicle_connected_bsensor_{nullptr};
  binary_sensor::BinarySensor *charging_bsensor_{nullptr};
  binary_sensor::BinarySensor *ac_present_bsensor_{nullptr};
  binary_sensor::BinarySensor *fault_active_bsensor_{nullptr};
  binary_sensor::BinarySensor *contactor_cmd_bsensor_{nullptr};
  binary_sensor::BinarySensor *rfid_present_bsensor_{nullptr};
  binary_sensor::BinarySensor *rfid_last_accepted_bsensor_{nullptr};
  binary_sensor::BinarySensor *session_authorized_bsensor_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *evse_state_tsensor_{nullptr};
  text_sensor::TextSensor *j1772_state_tsensor_{nullptr};
  text_sensor::TextSensor *first_fault_tsensor_{nullptr};
  text_sensor::TextSensor *build_info_tsensor_{nullptr};
  text_sensor::TextSensor *last_rfid_uid_tsensor_{nullptr};
  text_sensor::TextSensor *last_auth_result_tsensor_{nullptr};
  text_sensor::TextSensor *last_rejected_uid_tsensor_{nullptr};
#endif
  std::string last_rfid_uid_str_;
  uint32_t    last_rfid_uid_u32_{0};
  bool        last_rfid_present_{false};
  uint8_t     rfid_authlist_count_{0};
  uint8_t     last_rfid_auth_result_{0xFF};
  uint32_t    last_rfid_auth_uid_{0};
  std::string last_rejected_uid_str_;
  bool        require_rfid_auth_{false};
  bool        session_authorized_{false};
  std::vector<std::function<void(std::string, uint8_t)>>
      rfid_auth_result_callbacks_;
#ifdef USE_SWITCH
  class OpenevchargerTlvSwitch *require_rfid_auth_switch_{nullptr};
#endif

  // --- OTA push state machine ----------------------------------------
  // Driven by loop() — fires next CMD_OTA_CHUNK on every CHUNK_ACK with
  // matching offset. ota_buf_ holds the entire image (size-capped at
  // OTA_PUSH_MAX_IMAGE_BYTES, default 64 KB; lives on the FC41D heap
  // for the duration of the upload only — released on DONE/FAILED).
  enum class OtaState : uint8_t {
    IDLE,
    AWAIT_BEGIN,
    AWAIT_CHUNK,
    AWAIT_COMMIT,
    DONE,
    FAILED,
  };
  void ota_loop_tick_();
  void ota_send_next_chunk_();
  void ota_finish_(OtaState end_state, const char *why);
  void ota_publish_progress_();
  // Common path for both `start_ota_push(data,len)` and the fetch-
  // and-push variant. Assumes ota_buf_ has just been populated with
  // ota_buf_.size() bytes of image data; sends BEGIN with that
  // payload and arms the state machine. Returns true on success.
  bool start_ota_push_from_buf_();

  // Cached clock state from EVT_TIME. mcu_unix_seconds_recv_ms_ is the
  // FC41D millis() at receipt, used to extrapolate "MCU clock now"
  // without a fresh GET_TIME round-trip.
  uint32_t mcu_unix_seconds_{0};
  uint32_t mcu_unix_seconds_recv_ms_{0};
  bool     mcu_time_is_set_{false};

  // Rate-limit guard for send_set_time(). HA fires both
  // on_client_connected and on_time_sync within ms of a reconnect;
  // we squelch the duplicate so the MCU log + W25Q stay quiet.
  uint32_t last_set_time_ms_{0};
  uint32_t last_set_time_unix_{0};
  uint8_t  last_set_time_seq_{0};

  std::vector<uint8_t> ota_buf_;
  uint32_t ota_session_id_{0};
  uint32_t ota_image_crc32_{0};
  uint32_t ota_total_bytes_{0};
  uint32_t ota_next_offset_{0};
  uint32_t ota_last_io_ms_{0};
  uint32_t ota_op_timeout_ms_{2000};   // per-ack deadline
  OtaState ota_state_{OtaState::IDLE};
  uint8_t  ota_last_status_{0xFF};
  uint8_t  ota_seq_begin_{0};
  uint8_t  ota_seq_chunk_{0};
  uint8_t  ota_seq_commit_{0};
  uint8_t  ota_progress_pct_cache_{0xFF};
#ifdef USE_NUMBER
  std::vector<OpenevchargerTlvNumber *> numbers_;
#endif
#ifdef USE_BUTTON
  std::vector<OpenevchargerTlvButton *> buttons_;
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
  RFID_LEARN_NEXT,
  RFID_CLEAR_LIST,
  RFID_GET_LIST,
  OTA_ABORT,
  RESTART,
  SIMULATE_REPLUG,
  RUN_GFCI_CAL_TEST,
};

#ifdef USE_NUMBER
class OpenevchargerTlvNumber : public number::Number, public Component {
 public:
  void set_parent(OpenevchargerTlv *p) { parent_ = p; }
  void set_kind(NumberKind k) { kind_ = k; }
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  void publish_from_state();

 protected:
  void control(float value) override;
  OpenevchargerTlv *parent_{nullptr};
  NumberKind kind_{NumberKind::ADVERTISED_AMPS};
};
#endif

#ifdef USE_SWITCH
class OpenevchargerTlvSwitch : public switch_::Switch, public Component {
 public:
  void set_parent(OpenevchargerTlv *p) { parent_ = p; }
  void setup() override {}
  float get_setup_priority() const override { return setup_priority::DATA; }
  // Pushed by EVT_RFID_CONFIG dispatch — bypasses control() so the
  // confirmed-from-MCU state lands cleanly without echoing back over UART.
  void publish_from_mcu(bool state) {
    if (this->state != state) this->publish_state(state);
  }

 protected:
  void write_state(bool state) override {
    if (parent_) parent_->send_set_require_rfid_auth(state);
    // Optimistic — confirmed by next EVT_RFID_CONFIG.
    this->publish_state(state);
  }
  OpenevchargerTlv *parent_{nullptr};
};
#endif

#ifdef USE_BUTTON
class OpenevchargerTlvButton : public button::Button {
 public:
  void set_parent(OpenevchargerTlv *p) { parent_ = p; }
  void set_action(ButtonAction a) { action_ = a; }
  void set_buzzer_ms(uint16_t ms) { buzzer_ms_ = ms; }

 protected:
  void press_action() override;
  OpenevchargerTlv *parent_{nullptr};
  ButtonAction action_{ButtonAction::PING};
  uint16_t buzzer_ms_{50};
};
#endif

// YAML automation trigger for EVT_RFID_AUTH_RESULT. Variables:
//   x = colon-formatted UID string ("B3:EA:04:88")
//   result = RFID_AUTH_RESULT_* code (0=LEARNED, 1=START, 2=STOP,
//            3=MATCHED_NOOP, 4=REJECTED, 5=LIST_FULL)
class RFIDAuthResultTrigger : public Trigger<std::string, uint8_t> {
 public:
  explicit RFIDAuthResultTrigger(OpenevchargerTlv *parent) {
    parent->add_on_rfid_auth_result_callback(
        [this](std::string uid, uint8_t result) {
          this->trigger(uid, result);
        });
  }
};

}  // namespace openevcharger_tlv
}  // namespace esphome
