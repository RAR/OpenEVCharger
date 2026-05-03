#include "openbhzd_tlv.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cstdio>
#include <cstring>

namespace esphome {
namespace openbhzd_tlv {

static const char *const TAG = "openbhzd_tlv";

// SOF, header, CRC sizes (spec § 5).
static constexpr uint8_t SOF0 = 0xA5;
static constexpr uint8_t SOF1 = 0x5A;
static constexpr size_t HDR_LEN = 6;       // 2 SOF + 2 LEN + CMD + SEQ
static constexpr size_t CRC_LEN = 2;
static constexpr size_t PAYLOAD_MAX = 56;
static constexpr size_t FRAME_MAX = HDR_LEN + PAYLOAD_MAX + CRC_LEN;

void OpenbhzdTlv::setup() {
  rx_buf_.reserve(128);
  // Trigger an initial state pull as soon as we're up. The MCU emits an
  // unsolicited BOOT_COMPLETE plus a STATE_REPORT shortly after its own
  // boot, but the BK might come up second so we ask explicitly.
  last_poll_ms_ = millis();
  send_get_build_info();
  send_get_state();
  send_get_lifetime_kwh();
}

void OpenbhzdTlv::loop() {
  while (this->available()) {
    uint8_t b;
    if (!this->read_byte(&b)) break;
    process_byte_(b);
  }

  uint32_t now = millis();
  if (now - last_poll_ms_ >= poll_interval_ms_) {
    last_poll_ms_ = now;
    send_get_state();
    // Refresh lifetime kWh much less often — it's a flash read on the MCU.
    static uint8_t lifetime_div = 0;
    if (++lifetime_div >= 12) {  // ~once per minute at 5 s poll
      lifetime_div = 0;
      send_get_lifetime_kwh();
    }
  }

  // Drive the link_up binary sensor on a timer even with no traffic.
  bool up = link_up();
  if (up != last_link_up_) {
    last_link_up_ = up;
#ifdef USE_BINARY_SENSOR
    if (link_up_bsensor_) link_up_bsensor_->publish_state(up);
#endif
  }
}

void OpenbhzdTlv::dump_config() {
  ESP_LOGCONFIG(TAG, "OpenBHZD TLV link:");
  ESP_LOGCONFIG(TAG, "  Poll interval: %u ms", (unsigned) poll_interval_ms_);
  ESP_LOGCONFIG(TAG, "  Link timeout:  %u ms", (unsigned) link_timeout_ms_);
  this->check_uart_settings(115200);
}

bool OpenbhzdTlv::link_up() const {
  if (last_state_ms_ == 0) return false;
  return (millis() - last_state_ms_) < link_timeout_ms_;
}

uint32_t OpenbhzdTlv::state_age_ms() const {
  if (last_state_ms_ == 0) return 0;
  return millis() - last_state_ms_;
}

uint8_t OpenbhzdTlv::next_seq_() {
  uint8_t s = seq_++;
  if (seq_ == 0) seq_ = 1;  // 0 is reserved for unsolicited events
  return s;
}

void OpenbhzdTlv::process_byte_(uint8_t b) {
  // Resync if we're not yet aligned on SOF0/SOF1.
  if (rx_buf_.empty()) {
    if (b != SOF0) return;
    rx_buf_.push_back(b);
    return;
  }
  if (rx_buf_.size() == 1) {
    if (b != SOF1) {
      rx_buf_.clear();
      if (b == SOF0) rx_buf_.push_back(b);  // could be the next frame's SOF0
      return;
    }
    rx_buf_.push_back(b);
    return;
  }

  if (rx_buf_.size() >= FRAME_MAX) {
    ESP_LOGW(TAG, "rx buffer overrun, resyncing");
    rx_buf_.clear();
    return;
  }

  rx_buf_.push_back(b);

  // We have at least 4 bytes — check LEN once it's there.
  if (rx_buf_.size() == 4) {
    uint16_t len = rx_buf_[2] | (uint16_t(rx_buf_[3]) << 8);
    if (len < 2 || len > PAYLOAD_MAX + 2) {
      ESP_LOGW(TAG, "bad LEN=%u, resyncing", len);
      rx_buf_.clear();
      return;
    }
  }

  try_parse_();
}

void OpenbhzdTlv::try_parse_() {
  if (rx_buf_.size() < HDR_LEN + CRC_LEN) return;
  uint16_t len = rx_buf_[2] | (uint16_t(rx_buf_[3]) << 8);
  size_t total = 4 + len + CRC_LEN;
  if (rx_buf_.size() < total) return;

  uint16_t got = (uint16_t(rx_buf_[4 + len]) << 8) | rx_buf_[4 + len + 1];
  uint16_t want = crc16_ccitt_(&rx_buf_[2], 2 + len);
  if (got != want) {
    ESP_LOGW(TAG, "CRC mismatch (got=0x%04x want=0x%04x), dropping byte",
             got, want);
    rx_buf_.erase(rx_buf_.begin());
    // Re-resync: scan forward to next SOF0.
    while (!rx_buf_.empty() && rx_buf_[0] != SOF0) rx_buf_.erase(rx_buf_.begin());
    return;
  }

  uint8_t cmd = rx_buf_[4];
  uint8_t seq = rx_buf_[5];
  size_t plen = len - 2;
  std::vector<uint8_t> payload(rx_buf_.begin() + 6, rx_buf_.begin() + 6 + plen);
  rx_buf_.erase(rx_buf_.begin(), rx_buf_.begin() + total);

  dispatch_frame_(cmd, seq, payload.data(), plen);
}

void OpenbhzdTlv::dispatch_frame_(uint8_t cmd, uint8_t seq,
                                  const uint8_t *p, size_t plen) {
  ESP_LOGV(TAG, "<- cmd=0x%02x seq=%u plen=%u", cmd, seq, (unsigned) plen);

  switch (cmd) {
    case EVT_PING_ACK:
      ESP_LOGD(TAG, "PING_ACK seq=%u", seq);
      last_state_ms_ = millis();  // any reply counts as "alive"
      break;

    case EVT_STATE_CHANGED: {
      // Lightweight unsolicited event from safety_task: 1 byte =
      // new j1772 state. Pull the full snapshot on next tick so the
      // STATE_REPORT path drives all the entity publishes.
      uint8_t s = (plen >= 1) ? p[0] : 0;
      ESP_LOGD(TAG, "STATE_CHANGED j1772=%s", j1772_state_name(s));
      send_get_state();
      last_state_ms_ = millis();
      break;
    }

    case EVT_STATE_REPORT: {
      // Layout matches src/core/system_state.h openbhzd_state
      // (packed, 30 bytes). Response to GET_STATE.
      if (plen < 30) {
        ESP_LOGW(TAG, "STATE_REPORT short (plen=%u)", (unsigned) plen);
        break;
      }
      StateReport &s = state_;
      s.j1772_state = p[0];
      s.evse_state = p[1];
      s.advertised_amps = p[2];
      s.contactor_cmd = p[3];
      s.cp_high_mv = int16_t(p[4] | (uint16_t(p[5]) << 8));
      s.cp_low_mv = int16_t(p[6] | (uint16_t(p[7]) << 8));
      s.active_amps_x10 = uint16_t(p[8] | (uint16_t(p[9]) << 8));
      s.ntc1_dC = uint16_t(p[10] | (uint16_t(p[11]) << 8));
      s.ntc2_dC = uint16_t(p[12] | (uint16_t(p[13]) << 8));
      s.cc_max_amps = uint16_t(p[14] | (uint16_t(p[15]) << 8));
      s.ac_present = p[16];
      s.pad = p[17];
      s.fault_active_bits = uint32_t(p[18]) | (uint32_t(p[19]) << 8) |
                            (uint32_t(p[20]) << 16) | (uint32_t(p[21]) << 24);
      s.first_fault_id = uint32_t(p[22]) | (uint32_t(p[23]) << 8) |
                         (uint32_t(p[24]) << 16) | (uint32_t(p[25]) << 24);
      s.session_mwh = uint32_t(p[26]) | (uint32_t(p[27]) << 8) |
                      (uint32_t(p[28]) << 16) | (uint32_t(p[29]) << 24);
      s.valid = true;
      first_fault_name_ = fault_name(s.first_fault_id);
      // Count set bits for fault_count diagnostic.
      uint32_t bits = s.fault_active_bits;
      uint32_t cnt = 0;
      while (bits) { cnt += (bits & 1u); bits >>= 1; }
      fault_count_ = cnt;
      last_state_ms_ = millis();
      publish_state_();
      break;
    }

    case EVT_BUILD_INFO: {
      // ASCII "version|sha", null-terminated.
      build_info_.assign(reinterpret_cast<const char *>(p),
                         strnlen(reinterpret_cast<const char *>(p), plen));
      ESP_LOGD(TAG, "BUILD_INFO: %s", build_info_.c_str());
#ifdef USE_TEXT_SENSOR
      if (build_info_tsensor_) build_info_tsensor_->publish_state(build_info_);
#endif
      last_state_ms_ = millis();
      break;
    }

    case EVT_LIFETIME_KWH: {
      if (plen < 4) break;
      lifetime_mwh_ = uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                      (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
      ESP_LOGD(TAG, "LIFETIME = %u mWh (%.3f kWh)",
               (unsigned) lifetime_mwh_, lifetime_mwh_ / 1000000.0f);
#ifdef USE_SENSOR
      if (lifetime_kwh_sensor_)
        lifetime_kwh_sensor_->publish_state(lifetime_mwh_ / 1000000.0f);
#endif
      last_state_ms_ = millis();
      break;
    }

    case EVT_BOOT_COMPLETE: {
      if (plen >= 8) {
        bool pass = p[0] != 0;
        uint32_t lf = uint32_t(p[4]) | (uint32_t(p[5]) << 8) |
                      (uint32_t(p[6]) << 16) | (uint32_t(p[7]) << 24);
        ESP_LOGI(TAG, "MCU boot complete: self_test=%s last_fault=%s",
                 pass ? "PASS" : "FAIL", fault_name(lf));
      } else {
        ESP_LOGI(TAG, "MCU boot complete");
      }
      // Re-pull state on every (re)boot — old cached state is stale.
      send_get_state();
      send_get_build_info();
      send_get_lifetime_kwh();
      break;
    }

    case EVT_FAULT_RAISED: {
      if (plen < 8) break;
      uint32_t fid = uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                     (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
      ESP_LOGW(TAG, "FAULT_RAISED: %s (id=%u)", fault_name(fid), (unsigned) fid);
      // STATE_REPORT will follow — main publish path runs from there.
      send_get_state();
      break;
    }

    case EVT_FAULT_CLEARED: {
      if (plen < 4) break;
      uint32_t fid = uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                     (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
      ESP_LOGI(TAG, "FAULT_CLEARED: %s (id=%u)", fault_name(fid), (unsigned) fid);
      send_get_state();
      break;
    }

    case EVT_SESSION_BEGAN:
      ESP_LOGI(TAG, "session began");
      send_get_state();
      break;

    case EVT_SESSION_ENDED: {
      uint32_t mwh = 0;
      if (plen >= 4) {
        mwh = uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
              (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
      }
      ESP_LOGI(TAG, "session ended: %.3f kWh", mwh / 1000000.0f);
      send_get_lifetime_kwh();
      break;
    }

    case EVT_FAULT_LOG_ENTRY:
      if (plen >= 12) {
        uint32_t ts = uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                      (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        uint16_t bc = uint16_t(p[4]) | (uint16_t(p[5]) << 8);
        uint16_t fid = uint16_t(p[6]) | (uint16_t(p[7]) << 8);
        ESP_LOGD(TAG, "fault_log[ts=%u boot=%u fid=%u (%s)]",
                 (unsigned) ts, (unsigned) bc, (unsigned) fid, fault_name(fid));
      }
      break;

    case EVT_FAULT_LOG_END:
      if (plen >= 2) {
        ESP_LOGD(TAG, "fault_log_end emitted=%u/%u", p[0], p[1]);
      }
      break;

    default:
      ESP_LOGD(TAG, "unhandled cmd=0x%02x seq=%u plen=%u", cmd, seq, (unsigned) plen);
      break;
  }
}

void OpenbhzdTlv::publish_state_() {
  const StateReport &s = state_;

#ifdef USE_SENSOR
  if (cp_high_mv_sensor_) cp_high_mv_sensor_->publish_state(s.cp_high_mv);
  if (cp_low_mv_sensor_) cp_low_mv_sensor_->publish_state(s.cp_low_mv);
  if (advertised_amps_sensor_) advertised_amps_sensor_->publish_state(s.advertised_amps);
  if (active_amps_sensor_) active_amps_sensor_->publish_state(s.active_amps_x10 / 10.0f);
  if (session_kwh_sensor_) session_kwh_sensor_->publish_state(s.session_mwh / 1000000.0f);
  // ntc1/ntc2_dC = deci-Celsius. Bench has no NTCs populated → comes through as 0.
  if (ntc1_temp_sensor_) ntc1_temp_sensor_->publish_state(s.ntc1_dC / 10.0f);
  if (ntc2_temp_sensor_) ntc2_temp_sensor_->publish_state(s.ntc2_dC / 10.0f);
  if (evse_state_code_sensor_) evse_state_code_sensor_->publish_state(s.evse_state);
  if (j1772_state_code_sensor_) j1772_state_code_sensor_->publish_state(s.j1772_state);
  if (fault_count_sensor_) fault_count_sensor_->publish_state(fault_count_);
#endif

#ifdef USE_BINARY_SENSOR
  if (vehicle_connected_bsensor_) {
    vehicle_connected_bsensor_->publish_state(
        s.j1772_state == 2 || s.j1772_state == 3 || s.j1772_state == 4);
  }
  if (charging_bsensor_) {
    charging_bsensor_->publish_state(s.j1772_state == 3 && s.contactor_cmd != 0);
  }
  if (ac_present_bsensor_) ac_present_bsensor_->publish_state(s.ac_present != 0);
  if (fault_active_bsensor_) fault_active_bsensor_->publish_state(fault_count_ != 0);
  if (contactor_cmd_bsensor_) contactor_cmd_bsensor_->publish_state(s.contactor_cmd != 0);
#endif

#ifdef USE_TEXT_SENSOR
  if (evse_state_tsensor_) evse_state_tsensor_->publish_state(evse_state_name(s.evse_state));
  if (j1772_state_tsensor_) j1772_state_tsensor_->publish_state(j1772_state_name(s.j1772_state));
  if (first_fault_tsensor_) first_fault_tsensor_->publish_state(first_fault_name_);
#endif

#ifdef USE_NUMBER
  for (auto *n : numbers_) n->publish_from_state();
#endif
}

// --- TX -----------------------------------------------------------------

void OpenbhzdTlv::send_frame_(uint8_t cmd, uint8_t seq,
                              const void *payload, size_t plen) {
  if (plen > PAYLOAD_MAX) {
    ESP_LOGE(TAG, "payload too long (%u > %u)", (unsigned) plen, (unsigned) PAYLOAD_MAX);
    return;
  }
  uint8_t buf[FRAME_MAX];
  buf[0] = SOF0;
  buf[1] = SOF1;
  uint16_t len = uint16_t(plen + 2);  // CMD+SEQ+payload
  buf[2] = uint8_t(len & 0xFF);
  buf[3] = uint8_t(len >> 8);
  buf[4] = cmd;
  buf[5] = seq;
  if (plen) std::memcpy(&buf[6], payload, plen);
  uint16_t crc = crc16_ccitt_(&buf[2], 2 + len);
  buf[6 + plen] = uint8_t(crc >> 8);
  buf[6 + plen + 1] = uint8_t(crc & 0xFF);
  size_t total = 6 + plen + 2;
  this->write_array(buf, total);
}

uint8_t OpenbhzdTlv::send_ping() {
  uint8_t s = next_seq_();
  send_frame_(CMD_PING, s, nullptr, 0);
  return s;
}

uint8_t OpenbhzdTlv::send_get_state() {
  uint8_t s = next_seq_();
  send_frame_(CMD_GET_STATE, s, nullptr, 0);
  return s;
}

uint8_t OpenbhzdTlv::send_get_build_info() {
  uint8_t s = next_seq_();
  send_frame_(CMD_GET_BUILD_INFO, s, nullptr, 0);
  return s;
}

uint8_t OpenbhzdTlv::send_set_advertised_amps(uint8_t amps) {
  uint8_t s = next_seq_();
  send_frame_(CMD_SET_ADVERTISED_AMPS, s, &amps, 1);
  // Refresh state so HA reflects the new value once persisted.
  send_get_state();
  return s;
}

uint8_t OpenbhzdTlv::send_request_stop() {
  uint8_t s = next_seq_();
  uint8_t z = 0;
  send_frame_(CMD_REQUEST_STOP, s, &z, 1);
  send_get_state();
  return s;
}

uint8_t OpenbhzdTlv::send_request_start_resume() {
  uint8_t s = next_seq_();
  send_frame_(CMD_REQUEST_START_RESUME, s, nullptr, 0);
  send_get_state();
  return s;
}

uint8_t OpenbhzdTlv::send_clear_fault(uint32_t fid) {
  uint8_t s = next_seq_();
  uint8_t buf[4] = {uint8_t(fid), uint8_t(fid >> 8), uint8_t(fid >> 16), uint8_t(fid >> 24)};
  send_frame_(CMD_CLEAR_FAULT, s, buf, 4);
  return s;
}

uint8_t OpenbhzdTlv::send_get_fault_log(uint8_t max_count) {
  uint8_t s = next_seq_();
  send_frame_(CMD_GET_FAULT_LOG, s, &max_count, 1);
  return s;
}

uint8_t OpenbhzdTlv::send_get_lifetime_kwh() {
  uint8_t s = next_seq_();
  send_frame_(CMD_GET_LIFETIME_KWH, s, nullptr, 0);
  return s;
}

uint8_t OpenbhzdTlv::send_buzzer_beep(uint16_t ms) {
  uint8_t s = next_seq_();
  uint8_t buf[2] = {uint8_t(ms), uint8_t(ms >> 8)};
  send_frame_(CMD_BUZZER_BEEP, s, buf, 2);
  return s;
}

uint8_t OpenbhzdTlv::send_set_led_override(uint8_t mode, uint8_t r, uint8_t g, uint8_t b) {
  uint8_t s = next_seq_();
  uint8_t buf[4] = {mode, r, g, b};
  send_frame_(CMD_SET_LED_OVERRIDE, s, buf, 4);
  return s;
}

// --- Helpers ------------------------------------------------------------

uint16_t OpenbhzdTlv::crc16_ccitt_(const uint8_t *p, size_t n) {
  // CRC16-CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no xor-out.
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; ++i) {
    crc ^= uint16_t(p[i]) << 8;
    for (int b = 0; b < 8; ++b) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

const char *OpenbhzdTlv::evse_state_name(uint8_t s) {
  switch (s) {
    case 0: return "BOOT";
    case 1: return "SELF_TEST";
    case 2: return "READY";
    case 3: return "CHARGING";
    case 4: return "USER_PAUSED";
    case 5: return "COOLING_DOWN";
    case 6: return "FAULT";
    default: return "?";
  }
}

const char *OpenbhzdTlv::j1772_state_name(uint8_t s) {
  // j1772_ctx_t numeric values (src/core/j1772.h): INVALID=0, A=1..F=6.
  switch (s) {
    case 0: return "INVALID";
    case 1: return "A";
    case 2: return "B";
    case 3: return "C";
    case 4: return "D";
    case 5: return "E";
    case 6: return "F";
    default: return "?";
  }
}

const char *OpenbhzdTlv::fault_name(uint32_t id) {
  // Mirrors src/core/fault.h enum order.
  switch (id) {
    case 0: return "NONE";
    case 1: return "GFCI";
    case 2: return "RELAY_WELD";
    case 3: return "RELAY_STUCK_OPEN";
    case 4: return "PE_CONTINUITY";
    case 5: return "CP_NO_PILOT";
    case 6: return "DIODE_CHECK";
    case 7: return "BOOT_SELF_TEST";
    case 8: return "GFCI_SELF_TEST";
    case 9: return "RELAY_WELD_AT_BOOT";
    case 10: return "RELAY_OPEN_AT_BOOT";
    case 11: return "ADC_OUT_OF_RANGE";
    case 12: return "HARD_OVER_CURRENT";
    case 13: return "CRASH_LOOP_SAFE_FAIL";
    case 16: return "OVER_TEMP";
    case 17: return "SOFT_OVER_CURRENT";
    case 18: return "CC_OUT_OF_RANGE";
    case 19: return "AC_ABSENT";
    case 20: return "CP_REGRESSION";
    default: return "UNKNOWN";
  }
}

// --- Number / Button ----------------------------------------------------

#ifdef USE_NUMBER
void OpenbhzdTlvNumber::setup() {
  if (parent_) publish_from_state();
}

void OpenbhzdTlvNumber::publish_from_state() {
  if (!parent_) return;
  switch (kind_) {
    case NumberKind::ADVERTISED_AMPS: {
      auto v = float(parent_->state().advertised_amps);
      if (v != this->state) this->publish_state(v);
      break;
    }
  }
}

void OpenbhzdTlvNumber::control(float value) {
  if (!parent_) return;
  switch (kind_) {
    case NumberKind::ADVERTISED_AMPS: {
      uint8_t a = (value < 0) ? 0 : (value > 255 ? 255 : uint8_t(value));
      ESP_LOGD(TAG, "set advertised amps -> %u", a);
      parent_->send_set_advertised_amps(a);
      this->publish_state(value);  // optimistic — confirmed by next STATE_REPORT
      break;
    }
  }
}
#endif

#ifdef USE_BUTTON
void OpenbhzdTlvButton::press_action() {
  if (!parent_) return;
  switch (action_) {
    case ButtonAction::PING: parent_->send_ping(); break;
    case ButtonAction::REQUEST_STOP: parent_->send_request_stop(); break;
    case ButtonAction::REQUEST_START_RESUME: parent_->send_request_start_resume(); break;
    case ButtonAction::CLEAR_FAULT_ALL: parent_->send_clear_fault(0); break;
    case ButtonAction::GET_STATE: parent_->send_get_state(); break;
    case ButtonAction::GET_BUILD_INFO: parent_->send_get_build_info(); break;
    case ButtonAction::GET_FAULT_LOG: parent_->send_get_fault_log(16); break;
    case ButtonAction::GET_LIFETIME_KWH: parent_->send_get_lifetime_kwh(); break;
    case ButtonAction::BUZZER_BEEP: parent_->send_buzzer_beep(buzzer_ms_); break;
  }
}
#endif

}  // namespace openbhzd_tlv
}  // namespace esphome
