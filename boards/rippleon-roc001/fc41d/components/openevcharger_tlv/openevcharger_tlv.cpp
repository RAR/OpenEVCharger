#include "openevcharger_tlv.h"
#include "ntc_lut.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

// LibreTiny's Arduino-style MAC setter, used by apply_mac_override_().
// Must be at file scope, not inside our namespace, or the compiler
// reparents arduino:: classes (Print, etc.) into our ns and chokes.
#ifdef USE_LIBRETINY
#include <WiFi.h>
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace esphome {
namespace openevcharger_tlv {

static const char *const TAG = "openevcharger_tlv";

// SOF, header, CRC sizes (spec § 5).
static constexpr uint8_t SOF0 = 0xA5;
static constexpr uint8_t SOF1 = 0x5A;
static constexpr size_t HDR_LEN = 6;       // 2 SOF + 2 LEN + CMD + SEQ
static constexpr size_t CRC_LEN = 2;
static constexpr size_t PAYLOAD_MAX = 56;
static constexpr size_t FRAME_MAX = HDR_LEN + PAYLOAD_MAX + CRC_LEN;

void OpenevchargerTlv::setup() {
  rx_buf_.reserve(128);

  // OEM ships every FC41D with the same MAC address. Fix it before
  // WiFi.begin() runs by pulling the GD32's UID96 over TLV (guaranteed
  // unique per die) and folding it into a locally-administered MAC.
  // setup_priority::BUS sorts us before WIFI, so a brief spin-wait
  // here lands the override before the radio comes up. Failure mode
  // is we keep the OEM MAC — collisions stay possible but the link
  // still works.
  send_get_device_id();
  uint32_t deadline = millis() + 500;
  while (millis() < deadline && !mac_overridden_) {
    while (this->available()) {
      uint8_t b;
      if (this->read_byte(&b)) process_byte_(b);
    }
    delayMicroseconds(200);
  }
  if (!mac_overridden_) {
    ESP_LOGW(TAG, "no DEVICE_ID from MCU within 500 ms — keeping OEM MAC");
  }

  // Trigger an initial state pull as soon as we're up. The MCU emits an
  // unsolicited BOOT_COMPLETE plus a STATE_REPORT shortly after its own
  // boot, but the BK might come up second so we ask explicitly.
  last_poll_ms_ = millis();
  send_get_build_info();
  send_get_state();
  send_get_lifetime_kwh();
  send_rfid_get_list();
  send_get_rfid_config();
  send_get_gfci_policy();
}

void OpenevchargerTlv::loop() {
  maybe_log_mac_status_();

  // Re-fire GET_DEVICE_ID every 2 s until the MCU answers. The
  // setup-time spin-wait can miss the reply if the MCU is busy or
  // the WiFi component's setup chewed up the UART RX window.
  if (!mac_overridden_) {
    static uint32_t last_devid_retry = 0;
    uint32_t now = millis();
    if (now - last_devid_retry > 2000) {
      last_devid_retry = now;
      send_get_device_id();
    }
  }

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

  // OTA push state machine — non-blocking; advances on each loop()
  // tick and on each EVT_OTA_*_ACK dispatched above.
  ota_loop_tick_();

  // Drive the link_up binary sensor on a timer even with no traffic.
  bool up = link_up();
  if (up != last_link_up_) {
    last_link_up_ = up;
#ifdef USE_BINARY_SENSOR
    if (link_up_bsensor_) link_up_bsensor_->publish_state(up);
#endif
  }
}

void OpenevchargerTlv::dump_config() {
  ESP_LOGCONFIG(TAG, "OpenEVCharger TLV link:");
  ESP_LOGCONFIG(TAG, "  Poll interval: %u ms", (unsigned) poll_interval_ms_);
  ESP_LOGCONFIG(TAG, "  Link timeout:  %u ms", (unsigned) link_timeout_ms_);
  this->check_uart_settings(115200);
}

bool OpenevchargerTlv::link_up() const {
  if (last_state_ms_ == 0) return false;
  return (millis() - last_state_ms_) < link_timeout_ms_;
}

uint32_t OpenevchargerTlv::state_age_ms() const {
  if (last_state_ms_ == 0) return 0;
  return millis() - last_state_ms_;
}

uint8_t OpenevchargerTlv::next_seq_() {
  uint8_t s = seq_++;
  if (seq_ == 0) seq_ = 1;  // 0 is reserved for unsolicited events
  return s;
}

void OpenevchargerTlv::process_byte_(uint8_t b) {
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

void OpenevchargerTlv::try_parse_() {
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

void OpenevchargerTlv::dispatch_frame_(uint8_t cmd, uint8_t seq,
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
      // Layout matches src/core/system_state.h openevcharger_state
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
      // PA2 ADC raw at offset 30 — actually GUN-cable NTC, not mains
      // voltage (early bring-up labelled it "AC", corrected 2026-05-03
      // when grounding the front-block NTC pin zeroed PA2). Older MCU
      // firmwares emit 30-byte payloads — leave the field 0 in that case.
      s.gun_ntc_adc_raw = (plen >= 32)
          ? uint16_t(p[30] | (uint16_t(p[31]) << 8))
          : 0;
      // NTC1 (PA3) at off 32 — wall-plug NTC; NTC2 (PB0) at off 34 —
      // not a thermistor (likely AC presence). Same back-compat:
      // old MCU firmwares with 32 B payload won't carry these.
      s.ntc1_adc_raw = (plen >= 34)
          ? uint16_t(p[32] | (uint16_t(p[33]) << 8))
          : 0;
      s.ntc2_adc_raw = (plen >= 36)
          ? uint16_t(p[34] | (uint16_t(p[35]) << 8))
          : 0;
      // BL0939 metering — V_RMS/IA_RMS/IB_RMS (u32) at off 36/40/44,
      // A_WATT (i32, sign-extended) at off 48, valid flag at off 52.
      // Older MCU firmwares (pre BL0939 integration) emit a 36 B
      // payload; in that case all fields stay 0.
      if (plen >= 56) {
        s.bl0939_v_rms = uint32_t(p[36]) | (uint32_t(p[37]) << 8) |
                         (uint32_t(p[38]) << 16) | (uint32_t(p[39]) << 24);
        s.bl0939_ia_rms = uint32_t(p[40]) | (uint32_t(p[41]) << 8) |
                          (uint32_t(p[42]) << 16) | (uint32_t(p[43]) << 24);
        s.bl0939_ib_rms = uint32_t(p[44]) | (uint32_t(p[45]) << 8) |
                          (uint32_t(p[46]) << 16) | (uint32_t(p[47]) << 24);
        s.bl0939_a_watt = int32_t(uint32_t(p[48]) | (uint32_t(p[49]) << 8) |
                                  (uint32_t(p[50]) << 16) | (uint32_t(p[51]) << 24));
        s.bl0939_valid = (p[52] != 0);
        // p[53] = pad. p[54..55] = freq_hz × 10 (u16 LE).
        s.bl0939_freq_hz_x10 = uint16_t(p[54] | (uint16_t(p[55]) << 8));
      } else {
        s.bl0939_v_rms = 0;
        s.bl0939_ia_rms = 0;
        s.bl0939_ib_rms = 0;
        s.bl0939_a_watt = 0;
        s.bl0939_valid = false;
        s.bl0939_freq_hz_x10 = 0;
      }
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

    case EVT_DEVICE_ID: {
      // 12-byte UID96 from GD32 0x1FFFF7E8. Fold to 6 bytes by XOR-ing
      // halves, then force a locally-administered unicast OUI byte.
      if (plen < 12) {
        ESP_LOGW(TAG, "DEVICE_ID short (plen=%u)", (unsigned) plen);
        break;
      }
      uint8_t mac[6];
      for (int i = 0; i < 6; ++i) mac[i] = p[i] ^ p[i + 6];
      mac[0] = (mac[0] & 0xFCu) | 0x02u;  // clear multicast, set local-admin
      apply_mac_override_(mac);
      mac_overridden_ = true;
      last_state_ms_ = millis();
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

    case EVT_RFID_AUTH_RESULT: {
      // Payload: u32 uid (LE) + u8 result.
      if (plen < 5) break;
      uint32_t uid = uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                     (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
      uint8_t result = p[4];
      last_rfid_auth_uid_ = uid;
      last_rfid_auth_result_ = result;
      const char *label = "?";
      bool accepted = false;
      bool rejected = false;
      switch (result) {
        case RFID_AUTH_RESULT_LEARNED:        label = "learned";        accepted = true; break;
        case RFID_AUTH_RESULT_START:          label = "start";          accepted = true; break;
        case RFID_AUTH_RESULT_STOP:           label = "stop";           accepted = true; break;
        case RFID_AUTH_RESULT_MATCHED_NOOP:   label = "matched-noop";   accepted = true; break;
        case RFID_AUTH_RESULT_REJECTED:       label = "rejected";       rejected = true; break;
        case RFID_AUTH_RESULT_LIST_FULL:      label = "list-full";      rejected = true; break;
      }
      char hex[12];
      snprintf(hex, sizeof(hex), "%02X:%02X:%02X:%02X",
               uint8_t(uid >> 24), uint8_t(uid >> 16),
               uint8_t(uid >> 8),  uint8_t(uid));
      ESP_LOGI(TAG, "RFID auth uid=%s -> %s", hex, label);
#ifdef USE_BINARY_SENSOR
      if (rfid_last_accepted_bsensor_) rfid_last_accepted_bsensor_->publish_state(accepted);
#endif
#ifdef USE_TEXT_SENSOR
      if (last_auth_result_tsensor_) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s %s", hex, label);
        last_auth_result_tsensor_->publish_state(buf);
      }
      if (rejected && last_rejected_uid_tsensor_) {
        last_rejected_uid_str_ = std::string(hex);
        last_rejected_uid_tsensor_->publish_state(last_rejected_uid_str_);
      }
#endif
      // Refresh authlist count after a learn — the count just bumped by 1.
      if (result == RFID_AUTH_RESULT_LEARNED) {
        send_rfid_get_list();
      }
      // Fan out to YAML automation hooks (e.g. ocpp component drives
      // start_transaction(idTag) / end_transaction_with_idtag here).
      for (auto &cb : rfid_auth_result_callbacks_) {
        cb(std::string(hex), result);
      }
      break;
    }

    case EVT_RFID_LIST_ENTRY: {
      // Payload: u8 idx + u8 count + u32 uid LE.
      if (plen < 6) break;
      uint8_t idx = p[0];
      uint8_t count = p[1];
      uint32_t uid = uint32_t(p[2]) | (uint32_t(p[3]) << 8) |
                     (uint32_t(p[4]) << 16) | (uint32_t(p[5]) << 24);
      ESP_LOGD(TAG, "rfid_list[%u/%u] uid=%08X", idx, count, (unsigned) uid);
      rfid_authlist_count_ = count;
      break;
    }

    case EVT_RFID_CONFIG: {
      // Payload: u8 require_rfid_auth + u8 session_authorized.
      if (plen < 2) break;
      bool req = (p[0] != 0);
      bool sa = (p[1] != 0);
      require_rfid_auth_ = req;
      session_authorized_ = sa;
      ESP_LOGI(TAG, "rfid_config: require_auth=%u session_authorized=%u",
               unsigned(req), unsigned(sa));
#ifdef USE_SWITCH
      if (require_rfid_auth_switch_) require_rfid_auth_switch_->publish_from_mcu(req);
#endif
#ifdef USE_BINARY_SENSOR
      if (session_authorized_bsensor_) session_authorized_bsensor_->publish_state(sa);
#endif
      break;
    }

    case EVT_GFCI_POLICY: {
      // Payload: u8 policy (GFCI_POLICY_*).
      if (plen < 1) break;
      gfci_policy_ = p[0];
      const char *name = (p[0] == GFCI_POLICY_FAULT) ? "fault"
                       : (p[0] == GFCI_POLICY_WARN)  ? "warn"
                       :                               "?";
      if (p[0] == GFCI_POLICY_FAULT) {
        ESP_LOGI(TAG, "gfci_policy: %s", name);
      } else {
        ESP_LOGW(TAG, "gfci_policy: %s — ground-fault interlock suppressed",
                 name);
      }
#ifdef USE_SELECT
      if (gfci_policy_select_) gfci_policy_select_->publish_from_mcu(p[0]);
#endif
      break;
    }

    case EVT_RFID_LIST_END: {
      if (plen >= 1) {
        rfid_authlist_count_ = p[0];
        ESP_LOGD(TAG, "rfid_list_end count=%u", p[0]);
#ifdef USE_SENSOR
        if (rfid_authlist_count_sensor_) {
          rfid_authlist_count_sensor_->publish_state(float(rfid_authlist_count_));
        }
#endif
      }
      break;
    }

    case EVT_RFID_SWIPE: {
      // Payload: u32 uid (LE) + u8 present.
      if (plen < 5) break;
      uint32_t uid = uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                     (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
      uint8_t present = p[4];
      last_rfid_present_ = (present != 0);
      // Only update the latched UID on a card-present edge — keep
      // showing the last-seen UID after the card is lifted off so HA
      // can read it any time. The "RFID Card Present" binary sensor
      // is the live indicator of whether a card is currently on the
      // reader.
      if (present) {
        last_rfid_uid_u32_ = uid;
        // MSB-first colon-separated form matches what's printed on
        // most Mifare cards and the OCPP idTag string convention.
        char hex[12];
        snprintf(hex, sizeof(hex), "%02X:%02X:%02X:%02X",
                 uint8_t(uid >> 24), uint8_t(uid >> 16),
                 uint8_t(uid >> 8),  uint8_t(uid));
        last_rfid_uid_str_ = std::string(hex);
      }
      ESP_LOGI(TAG, "RFID %s uid=%s",
               present ? "swipe" : "removed",
               last_rfid_uid_str_.c_str());
#ifdef USE_TEXT_SENSOR
      if (present && last_rfid_uid_tsensor_) {
        last_rfid_uid_tsensor_->publish_state(last_rfid_uid_str_);
      }
#endif
#ifdef USE_SENSOR
      if (present && last_rfid_uid_sensor_) {
        last_rfid_uid_sensor_->publish_state(float(last_rfid_uid_u32_));
      }
#endif
#ifdef USE_BINARY_SENSOR
      if (rfid_present_bsensor_) {
        rfid_present_bsensor_->publish_state(last_rfid_present_);
      }
#endif
      break;
    }

    case EVT_OTA_BEGIN_ACK: {
      // Payload (8 B): u32 session_id, u8 status, u8 chunk_size_max, u16 reserved.
      if (plen < 8) break;
      uint32_t sid = uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                     (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
      uint8_t  status = p[4];
      uint8_t  cs_max = p[5];
      ota_last_status_ = status;
      ESP_LOGI(TAG, "OTA BEGIN_ACK sid=0x%08x status=%u chunk_max=%u",
               unsigned(sid), unsigned(status), unsigned(cs_max));
      if (ota_state_ != OtaState::AWAIT_BEGIN || sid != ota_session_id_) {
        ESP_LOGW(TAG, "OTA BEGIN_ACK ignored (state=%s sid=%08x want=%08x)",
                 ota_state_name(), unsigned(sid), unsigned(ota_session_id_));
        break;
      }
      if (status != OTA_STATUS_OK) {
        ota_finish_(OtaState::FAILED, "BEGIN rejected");
        break;
      }
      ota_state_ = OtaState::AWAIT_CHUNK;
      ota_last_io_ms_ = millis();
      ota_send_next_chunk_();
      break;
    }

    case EVT_OTA_CHUNK_ACK: {
      // Payload (13 B): u32 session_id, u32 next_offset, u32 running_crc, u8 status.
      if (plen < 13) break;
      uint32_t sid = uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                     (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
      uint32_t next_off = uint32_t(p[4]) | (uint32_t(p[5]) << 8) |
                          (uint32_t(p[6]) << 16) | (uint32_t(p[7]) << 24);
      uint8_t  status   = p[12];
      ota_last_status_ = status;
      if (ota_state_ != OtaState::AWAIT_CHUNK || sid != ota_session_id_) {
        ESP_LOGW(TAG, "OTA CHUNK_ACK ignored (state=%s sid=%08x want=%08x)",
                 ota_state_name(), unsigned(sid), unsigned(ota_session_id_));
        break;
      }
      if (status != OTA_STATUS_OK) {
        // OFFSET_MISMATCH means the MCU's next_off is authoritative —
        // resync and try again. Anything else: bail.
        if (status == OTA_STATUS_OFFSET_MISMATCH) {
          ESP_LOGW(TAG, "OTA chunk offset resync: ours=%u mcu=%u",
                   unsigned(ota_next_offset_), unsigned(next_off));
          ota_next_offset_ = next_off;
          ota_last_io_ms_ = millis();
          ota_send_next_chunk_();
        } else {
          ota_finish_(OtaState::FAILED, "CHUNK rejected");
        }
        break;
      }
      ota_next_offset_ = next_off;
      ota_last_io_ms_  = millis();
      ota_publish_progress_();
      if (ota_next_offset_ >= ota_total_bytes_) {
        ota_state_       = OtaState::AWAIT_COMMIT;
        ota_seq_commit_  = send_ota_commit(ota_session_id_);
        ESP_LOGI(TAG, "OTA all chunks acked, sent COMMIT seq=%u",
                 unsigned(ota_seq_commit_));
      } else {
        ota_send_next_chunk_();
      }
      break;
    }

    case EVT_OTA_COMMITTED: {
      if (plen < 5) break;
      uint32_t sid = uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                     (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
      uint8_t status = p[4];
      ota_last_status_ = status;
      if (ota_state_ != OtaState::AWAIT_COMMIT || sid != ota_session_id_) break;
      if (status == OTA_STATUS_OK) {
        ESP_LOGI(TAG, "OTA committed: pending flag set on MCU; reboot to activate");
        ota_finish_(OtaState::DONE, "committed");
      } else {
        ota_finish_(OtaState::FAILED, "COMMIT rejected");
      }
      break;
    }

    case EVT_OTA_ABORTED: {
      if (plen < 4) break;
      uint32_t sid = uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                     (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
      ESP_LOGI(TAG, "OTA aborted by MCU sid=0x%08x", unsigned(sid));
      if (sid == ota_session_id_) ota_finish_(OtaState::FAILED, "aborted by MCU");
      break;
    }

    case EVT_TIME: {
      // Payload (5 B): u32 unix_seconds LE, u8 is_set.
      if (plen < 5) break;
      uint32_t u = uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                   (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
      bool set = (p[4] != 0);
      mcu_unix_seconds_         = u;
      mcu_unix_seconds_recv_ms_ = millis();
      mcu_time_is_set_          = set;
      ESP_LOGD(TAG, "EVT_TIME unix=%u set=%u", unsigned(u), unsigned(set));
      last_state_ms_ = millis();
      break;
    }

    case EVT_DIAG_ADC: {
      // Payload (5 B): u16 pe_adc_raw LE, u8 j1772, u8 evse, u8 ac_present.
      // Emitted ~5 s by safety_task while F10 detector tuning is open.
      if (plen < 5) break;
      uint16_t pe_raw = uint16_t(p[0] | (uint16_t(p[1]) << 8));
      ESP_LOGD(TAG, "EVT_DIAG_ADC pe_raw=%u js=%u evse=%u",
               unsigned(pe_raw), unsigned(p[2]), unsigned(p[3]));
#ifdef USE_SENSOR
      if (pe_adc_raw_sensor_) pe_adc_raw_sensor_->publish_state(pe_raw);
#endif
      break;
    }

    default:
      ESP_LOGD(TAG, "unhandled cmd=0x%02x seq=%u plen=%u", cmd, seq, (unsigned) plen);
      break;
  }
}

void OpenevchargerTlv::publish_state_() {
  const StateReport &s = state_;

#ifdef USE_SENSOR
  if (cp_high_mv_sensor_) cp_high_mv_sensor_->publish_state(s.cp_high_mv);
  if (cp_low_mv_sensor_) cp_low_mv_sensor_->publish_state(s.cp_low_mv);
  if (advertised_amps_sensor_) advertised_amps_sensor_->publish_state(s.advertised_amps);
  if (active_amps_sensor_) active_amps_sensor_->publish_state(s.active_amps_x10 / 10.0f);
  if (session_kwh_sensor_) session_kwh_sensor_->publish_state(s.session_mwh / 1000000.0f);
  // °C from raw via the 150-entry LUT extracted from stock fw V1.0.066
  // (see ntc_lut.h). Replaces the earlier β=3380 model — that model
  // was off by ~10 °C at 85 °C because the OEM thermistor's β is
  // closer to 3980. The LUT covers −30..+119 °C; out-of-range maps
  // to 120 °C (stock semantics: open or shorted thermistor = max-temp
  // fault). NTC1 (PA3) is the wall-plug NTC; gun_ntc (PA2) is the
  // J1772 cable / gun-handle NTC.
  if (ntc1_temp_sensor_) {
    ntc1_temp_sensor_->publish_state(ntc_raw_to_celsius(s.ntc1_adc_raw));
  }
  if (gun_ntc_temp_sensor_) {
    gun_ntc_temp_sensor_->publish_state(ntc_raw_to_celsius(s.gun_ntc_adc_raw));
  }
  // ntc2 (PB0) is NOT a thermistor — likely AC-presence sense (see
  // MCU pin_map.h). The °C entity is retained as a diagnostic so
  // we'd notice if a future hardware revision re-purposes PB0; the
  // displayed value from a non-NTC source is not meaningful.
  if (ntc2_temp_sensor_) {
    ntc2_temp_sensor_->publish_state(ntc_raw_to_celsius(s.ntc2_adc_raw));
  }
  if (evse_state_code_sensor_) evse_state_code_sensor_->publish_state(s.evse_state);
  if (j1772_state_code_sensor_) j1772_state_code_sensor_->publish_state(s.j1772_state);
  if (fault_count_sensor_) fault_count_sensor_->publish_state(fault_count_);
  if (gun_ntc_adc_raw_sensor_) gun_ntc_adc_raw_sensor_->publish_state(s.gun_ntc_adc_raw);
  if (ntc1_adc_raw_sensor_) ntc1_adc_raw_sensor_->publish_state(s.ntc1_adc_raw);
  if (ntc2_adc_raw_sensor_) ntc2_adc_raw_sensor_->publish_state(s.ntc2_adc_raw);

  // BL0939 metering — raw counts always; engineering units only when
  // a per-chassis scale has been wired up via YAML (default 0 = skip).
  if (bl0939_v_rms_raw_sensor_) bl0939_v_rms_raw_sensor_->publish_state(s.bl0939_v_rms);
  if (bl0939_ia_rms_raw_sensor_) bl0939_ia_rms_raw_sensor_->publish_state(s.bl0939_ia_rms);
  if (bl0939_ib_rms_raw_sensor_) bl0939_ib_rms_raw_sensor_->publish_state(s.bl0939_ib_rms);
  if (bl0939_a_watt_raw_sensor_) bl0939_a_watt_raw_sensor_->publish_state(s.bl0939_a_watt);
  if (s.bl0939_valid) {
    if (mains_voltage_sensor_ && bl0939_v_uv_per_raw_ != 0) {
      // raw × µV/raw → µV; / 1e6 → V
      double v = double(s.bl0939_v_rms) * double(bl0939_v_uv_per_raw_) / 1.0e6;
      mains_voltage_sensor_->publish_state(float(v));
    }
    if (mains_current_a_sensor_ && bl0939_ia_na_per_raw_ != 0) {
      // raw [count] × scale [nA/raw] / 1e9 = A. Cal v2 schema —
      // µA-resolution lacked precision at high current (F1 cal at 6/8/10
      // A extrapolated low at 44 A real-EV draw, 5.5% under).
      double a = double(s.bl0939_ia_rms) * double(bl0939_ia_na_per_raw_) / 1.0e9;
      mains_current_a_sensor_->publish_state(float(a));
    }
    if (mains_current_b_sensor_ && bl0939_ib_ua_per_raw_ != 0) {
      double a = double(s.bl0939_ib_rms) * double(bl0939_ib_ua_per_raw_) / 1.0e6;
      mains_current_b_sensor_->publish_state(float(a));
    }
    if (active_power_sensor_ && bl0939_pa_uw_per_raw_ != 0) {
      // pa_uw_per_raw is µW per raw count; signed to handle inverted-
      // sense PCBs (the ROC001 reads A_WATT negative because the CT
      // direction is opposite BL0939's expectation — bench-confirmed
      // 2026-05-06). Sign carries through the multiplication, so the
      // published watts value is sign-correct without an abs() here.
      double w = double(s.bl0939_a_watt) * double(bl0939_pa_uw_per_raw_) / 1.0e6;
      active_power_sensor_->publish_state(float(w));
    }
    // Mains frequency is reported directly by the BL0939 — no per-
    // chassis cal needed. 0 = no AC / read failed.
    if (mains_frequency_sensor_ && s.bl0939_freq_hz_x10 != 0) {
      mains_frequency_sensor_->publish_state(s.bl0939_freq_hz_x10 / 10.0f);
    }
  }
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

void OpenevchargerTlv::send_frame_(uint8_t cmd, uint8_t seq,
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

uint8_t OpenevchargerTlv::send_ping() {
  uint8_t s = next_seq_();
  send_frame_(CMD_PING, s, nullptr, 0);
  return s;
}

uint8_t OpenevchargerTlv::send_get_state() {
  uint8_t s = next_seq_();
  send_frame_(CMD_GET_STATE, s, nullptr, 0);
  return s;
}

uint8_t OpenevchargerTlv::send_get_build_info() {
  uint8_t s = next_seq_();
  send_frame_(CMD_GET_BUILD_INFO, s, nullptr, 0);
  return s;
}

uint8_t OpenevchargerTlv::send_get_device_id() {
  uint8_t s = next_seq_();
  send_frame_(CMD_GET_DEVICE_ID, s, nullptr, 0);
  return s;
}

void OpenevchargerTlv::apply_mac_override_(const uint8_t mac[6]) {
#ifdef USE_LIBRETINY
  WiFi.macAddress(mac_before_);
  // LibreTiny's WiFi.setMacAddress validates the unicast bit, copies
  // into system_mac, and bounces the radio mode if it's already up.
  // We run at AFTER_WIFI so the bounce fires and the BK SDK actually
  // re-reads system_mac on the next mode change.
  mac_set_rc_ = WiFi.setMacAddress(mac);
  WiFi.macAddress(mac_after_);
  // Don't log yet — the API client isn't attached this early in boot
  // so OTA `esphome run` log streams can't capture lines emitted from
  // setup(). loop() re-emits once the device has been up long enough
  // for the API to be live; mac_status_logged_ guards against repeats.
#else
  ESP_LOGD(TAG, "MAC override no-op — not building under LibreTiny");
#endif
}

void OpenevchargerTlv::maybe_log_mac_status_() {
  if (mac_status_logged_) return;
  // Wait ~10 s after boot so the API client / OTA log stream is live.
  if (millis() < 10000) return;

#ifdef USE_LIBRETINY
  uint8_t live[6] = {0};
  WiFi.macAddress(live);
  if (mac_overridden_) {
    ESP_LOGI(TAG,
             "MAC override status: was %02X:%02X:%02X:%02X:%02X:%02X "
             "after-set %02X:%02X:%02X:%02X:%02X:%02X "
             "live %02X:%02X:%02X:%02X:%02X:%02X (rc=%d)",
             mac_before_[0], mac_before_[1], mac_before_[2],
             mac_before_[3], mac_before_[4], mac_before_[5],
             mac_after_[0], mac_after_[1], mac_after_[2],
             mac_after_[3], mac_after_[4], mac_after_[5],
             live[0], live[1], live[2], live[3], live[4], live[5],
             mac_set_rc_ ? 1 : 0);
  } else {
    ESP_LOGW(TAG,
             "MAC override SKIPPED — no DEVICE_ID from MCU within 500ms; "
             "live MAC stays %02X:%02X:%02X:%02X:%02X:%02X",
             live[0], live[1], live[2], live[3], live[4], live[5]);
  }
#endif
  mac_status_logged_ = true;
}

uint8_t OpenevchargerTlv::send_set_advertised_amps(uint8_t amps) {
  uint8_t s = next_seq_();
  send_frame_(CMD_SET_ADVERTISED_AMPS, s, &amps, 1);
  // Refresh state so HA reflects the new value once persisted.
  send_get_state();
  return s;
}

uint8_t OpenevchargerTlv::send_request_stop() {
  uint8_t s = next_seq_();
  uint8_t z = 0;
  send_frame_(CMD_REQUEST_STOP, s, &z, 1);
  send_get_state();
  return s;
}

uint8_t OpenevchargerTlv::send_request_start_resume() {
  uint8_t s = next_seq_();
  send_frame_(CMD_REQUEST_START_RESUME, s, nullptr, 0);
  send_get_state();
  return s;
}

uint8_t OpenevchargerTlv::send_clear_fault(uint32_t fid) {
  uint8_t s = next_seq_();
  uint8_t buf[4] = {uint8_t(fid), uint8_t(fid >> 8), uint8_t(fid >> 16), uint8_t(fid >> 24)};
  send_frame_(CMD_CLEAR_FAULT, s, buf, 4);
  return s;
}

uint8_t OpenevchargerTlv::send_get_fault_log(uint8_t max_count) {
  uint8_t s = next_seq_();
  send_frame_(CMD_GET_FAULT_LOG, s, &max_count, 1);
  return s;
}

uint8_t OpenevchargerTlv::send_get_lifetime_kwh() {
  uint8_t s = next_seq_();
  send_frame_(CMD_GET_LIFETIME_KWH, s, nullptr, 0);
  return s;
}

uint8_t OpenevchargerTlv::send_buzzer_beep(uint16_t ms) {
  uint8_t s = next_seq_();
  uint8_t buf[2] = {uint8_t(ms), uint8_t(ms >> 8)};
  send_frame_(CMD_BUZZER_BEEP, s, buf, 2);
  return s;
}

uint8_t OpenevchargerTlv::send_set_led_override(uint8_t mode, uint8_t r, uint8_t g, uint8_t b) {
  uint8_t s = next_seq_();
  uint8_t buf[4] = {mode, r, g, b};
  send_frame_(CMD_SET_LED_OVERRIDE, s, buf, 4);
  return s;
}

uint8_t OpenevchargerTlv::send_write_bl0939_cal(int16_t v, int16_t ia, int16_t ib,
                                                int16_t pa, int32_t freq_const) {
  uint8_t s = next_seq_();
  uint8_t buf[12] = {
      uint8_t(uint16_t(v)),         uint8_t(uint16_t(v) >> 8),
      uint8_t(uint16_t(ia)),        uint8_t(uint16_t(ia) >> 8),
      uint8_t(uint16_t(ib)),        uint8_t(uint16_t(ib) >> 8),
      uint8_t(uint16_t(pa)),        uint8_t(uint16_t(pa) >> 8),
      uint8_t(uint32_t(freq_const)),         uint8_t(uint32_t(freq_const) >>  8),
      uint8_t(uint32_t(freq_const) >> 16),   uint8_t(uint32_t(freq_const) >> 24),
  };
  send_frame_(CMD_WRITE_BL0939_CAL, s, buf, 12);
  return s;
}

uint8_t OpenevchargerTlv::send_rfid_learn_next() {
  uint8_t s = next_seq_();
  send_frame_(CMD_RFID_LEARN_NEXT, s, nullptr, 0);
  return s;
}

uint8_t OpenevchargerTlv::send_rfid_clear_list() {
  uint8_t s = next_seq_();
  send_frame_(CMD_RFID_CLEAR_LIST, s, nullptr, 0);
  // Refresh count after the MCU processes the clear.
  send_rfid_get_list();
  return s;
}

uint8_t OpenevchargerTlv::send_rfid_get_list() {
  uint8_t s = next_seq_();
  send_frame_(CMD_RFID_GET_LIST, s, nullptr, 0);
  return s;
}

uint8_t OpenevchargerTlv::send_rfid_remove_uid(uint32_t uid) {
  uint8_t s = next_seq_();
  uint8_t buf[4] = {uint8_t(uid), uint8_t(uid >> 8),
                    uint8_t(uid >> 16), uint8_t(uid >> 24)};
  send_frame_(CMD_RFID_REMOVE_UID, s, buf, 4);
  send_rfid_get_list();
  return s;
}

uint8_t OpenevchargerTlv::send_set_require_rfid_auth(bool enable) {
  uint8_t s = next_seq_();
  uint8_t v = enable ? 1u : 0u;
  send_frame_(CMD_SET_REQUIRE_RFID_AUTH, s, &v, 1);
  // MCU will publish a fresh EVT_RFID_CONFIG once persisted.
  return s;
}

uint8_t OpenevchargerTlv::send_get_rfid_config() {
  uint8_t s = next_seq_();
  send_frame_(CMD_GET_RFID_CONFIG, s, nullptr, 0);
  return s;
}

uint8_t OpenevchargerTlv::send_set_gfci_policy(uint8_t policy) {
  uint8_t s = next_seq_();
  send_frame_(CMD_SET_GFCI_POLICY, s, &policy, 1);
  // MCU will publish a fresh EVT_GFCI_POLICY once persisted.
  return s;
}

uint8_t OpenevchargerTlv::send_get_gfci_policy() {
  uint8_t s = next_seq_();
  send_frame_(CMD_GET_GFCI_POLICY, s, nullptr, 0);
  return s;
}

uint8_t OpenevchargerTlv::send_set_time(uint32_t unix_seconds) {
  /* Rate-limit at 2 s gap: HA fires on_client_connected and time
   * on_time_sync within ms of each other on every reconnect, which
   * spams the MCU log + a redundant W25Q crash_state touch. The
   * 30-minute on_time cron is well outside the gap so legitimate
   * resyncs always go through. unix_seconds=0 (clear) bypasses the
   * gate so an explicit clear is always honoured. */
  uint32_t now = millis();
  if (unix_seconds != 0u &&
      last_set_time_unix_ == unix_seconds &&
      (now - last_set_time_ms_) < 2000u) {
    return last_set_time_seq_;
  }
  uint8_t s = next_seq_();
  uint8_t buf[4] = {
      uint8_t(unix_seconds),         uint8_t(unix_seconds >> 8),
      uint8_t(unix_seconds >> 16),   uint8_t(unix_seconds >> 24),
  };
  send_frame_(CMD_SET_TIME, s, buf, sizeof buf);
  last_set_time_ms_   = now;
  last_set_time_unix_ = unix_seconds;
  last_set_time_seq_  = s;
  return s;
}

uint8_t OpenevchargerTlv::send_get_time() {
  uint8_t s = next_seq_();
  send_frame_(CMD_GET_TIME, s, nullptr, 0);
  return s;
}

uint8_t OpenevchargerTlv::send_restart() {
  uint8_t s = next_seq_();
  send_frame_(CMD_RESTART, s, nullptr, 0);
  ESP_LOGI(TAG, "MCU restart requested (seq=%u)", unsigned(s));
  return s;
}

uint8_t OpenevchargerTlv::send_simulate_replug() {
  uint8_t s = next_seq_();
  send_frame_(CMD_SIMULATE_REPLUG, s, nullptr, 0);
  ESP_LOGI(TAG, "Simulate replug requested (seq=%u)", unsigned(s));
  return s;
}

uint8_t OpenevchargerTlv::send_run_gfci_cal_test() {
  uint8_t s = next_seq_();
  send_frame_(CMD_RUN_GFCI_CAL_TEST, s, nullptr, 0);
  ESP_LOGI(TAG, "GFCI CAL self-test requested (seq=%u)", unsigned(s));
  return s;
}

uint8_t OpenevchargerTlv::send_write_bl0939_cal_from_yaml() {
#ifdef USE_SENSOR
  auto clamp_i16 = [](int32_t v) -> int16_t {
    if (v >  INT16_MAX) return INT16_MAX;
    if (v <  INT16_MIN) return INT16_MIN;
    return int16_t(v);
  };
  int16_t v  = clamp_i16(bl0939_v_uv_per_raw_);
  int16_t ia = clamp_i16(bl0939_ia_na_per_raw_);
  int16_t ib = clamp_i16(bl0939_ib_ua_per_raw_);
  int16_t pa = clamp_i16(bl0939_pa_uw_per_raw_);
  int32_t fc = bl0939_freq_const_;
  ESP_LOGI(TAG, "Push BL0939 cal: V=%d uV/raw IA=%d nA/raw IB=%d uA/raw "
                "PA=%d uW/raw freq_const=%d (0=use default)",
           int(v), int(ia), int(ib), int(pa), int(fc));
  return send_write_bl0939_cal(v, ia, ib, pa, fc);
#else
  return 0;
#endif
}

// --- OTA push state machine --------------------------------------------

namespace {
// IEEE 802.3 CRC32 (poly 0xEDB88320, reflected, init 0xFFFFFFFF, xor-out
// 0xFFFFFFFF). Matches src/persist/crc.c on the MCU side.
uint32_t crc32_ieee(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  while (len--) {
    crc ^= *data++;
    for (unsigned i = 0; i < 8; ++i) {
      crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
  }
  return ~crc;
}
}  // namespace

uint8_t OpenevchargerTlv::send_ota_begin(uint32_t image_size, uint32_t image_crc32,
                                    uint32_t session_id) {
  uint8_t s = next_seq_();
  uint8_t buf[12] = {
      uint8_t(image_size),         uint8_t(image_size  >> 8),
      uint8_t(image_size  >> 16),  uint8_t(image_size  >> 24),
      uint8_t(image_crc32),        uint8_t(image_crc32 >> 8),
      uint8_t(image_crc32 >> 16),  uint8_t(image_crc32 >> 24),
      uint8_t(session_id),         uint8_t(session_id  >> 8),
      uint8_t(session_id  >> 16),  uint8_t(session_id  >> 24),
  };
  send_frame_(CMD_OTA_BEGIN, s, buf, sizeof buf);
  return s;
}

uint8_t OpenevchargerTlv::send_ota_chunk(uint32_t session_id, uint32_t offset,
                                    const uint8_t *data, uint8_t data_len) {
  if (data_len == 0 || data_len > OTA_CHUNK_MAX_DATA) return 0;
  uint8_t s = next_seq_();
  uint8_t buf[8 + OTA_CHUNK_MAX_DATA];
  buf[0] = uint8_t(session_id);
  buf[1] = uint8_t(session_id >> 8);
  buf[2] = uint8_t(session_id >> 16);
  buf[3] = uint8_t(session_id >> 24);
  buf[4] = uint8_t(offset);
  buf[5] = uint8_t(offset >> 8);
  buf[6] = uint8_t(offset >> 16);
  buf[7] = uint8_t(offset >> 24);
  for (uint8_t i = 0; i < data_len; ++i) buf[8 + i] = data[i];
  send_frame_(CMD_OTA_CHUNK, s, buf, 8u + data_len);
  return s;
}

uint8_t OpenevchargerTlv::send_ota_commit(uint32_t session_id) {
  uint8_t s = next_seq_();
  uint8_t buf[4] = {uint8_t(session_id),         uint8_t(session_id >> 8),
                    uint8_t(session_id >> 16),  uint8_t(session_id >> 24)};
  send_frame_(CMD_OTA_COMMIT, s, buf, sizeof buf);
  return s;
}

uint8_t OpenevchargerTlv::send_ota_abort(uint32_t session_id) {
  uint8_t s = next_seq_();
  uint8_t buf[4] = {uint8_t(session_id),         uint8_t(session_id >> 8),
                    uint8_t(session_id >> 16),  uint8_t(session_id >> 24)};
  send_frame_(CMD_OTA_ABORT, s, buf, sizeof buf);
  return s;
}

bool OpenevchargerTlv::fetch_and_push_ota(const std::string &url) {
  // Plain HTTP only. LibreTiny's BK7231N mbedtls port is missing
  // mbedtls_net_set_nonblock (link error in MbedTLSClient::connect),
  // so WiFiClientSecure won't link. If your HA is HTTPS-only, hit it
  // by direct local-IP + port 8123 (HA's built-in listener is plain
  // HTTP unless you put it behind a reverse proxy):
  //   http://<ha-local-ip>:8123/local/openevcharger.bin
  // Or stand up a plain-HTTP server alongside HA (python -m
  // http.server, busybox httpd, etc.) and serve the .bin from there.
  static const std::string scheme = "http://";
  if (url.compare(0, scheme.size(), scheme) != 0) {
    ESP_LOGW(TAG, "OTA fetch: only http:// supported (got %s) — see "
                  "openevcharger_tlv.cpp comment for HTTPS workaround",
             url.c_str());
    return false;
  }
  size_t a = scheme.size();
  size_t slash = url.find('/', a);
  std::string authority = url.substr(a, slash == std::string::npos ? std::string::npos
                                                                   : slash - a);
  std::string path = (slash == std::string::npos) ? "/" : url.substr(slash);
  std::string host = authority;
  uint16_t port = 80;
  size_t colon = authority.find(':');
  if (colon != std::string::npos) {
    host = authority.substr(0, colon);
    port = (uint16_t) std::atoi(authority.c_str() + colon + 1);
  }
  ESP_LOGI(TAG, "OTA fetch: host=%s port=%u path=%s",
           host.c_str(), unsigned(port), path.c_str());

  WiFiClient cstack;
  Client *client = &cstack;
  cstack.setTimeout(10000);   // ms
  if (!client->connect(host.c_str(), port)) {
    ESP_LOGE(TAG, "OTA fetch: connect %s:%u failed", host.c_str(), unsigned(port));
    return false;
  }

  // HTTP/1.1 GET, close after response so EOF terminates the body —
  // simpler than parsing chunked transfer encoding.
  std::string req;
  req.reserve(128 + path.size() + host.size());
  req  = "GET ";  req += path;  req += " HTTP/1.1\r\n";
  req += "Host: "; req += host;
  if (port != 80) { req += ':'; req += std::to_string(port); }
  req += "\r\n";
  req += "User-Agent: openevcharger-fc41d/1\r\n";
  req += "Accept: application/octet-stream\r\n";
  req += "Connection: close\r\n\r\n";
  client->write(reinterpret_cast<const uint8_t *>(req.data()), req.size());

  // Read status line + headers. Bail on anything but 200.
  uint32_t deadline = millis() + 10000;
  auto wait_line = [&](std::string &out) -> bool {
    out.clear();
    while (millis() < deadline) {
      if (!client->connected() && !client->available()) return false;
      if (!client->available()) { delay(5); continue; }
      char c = char(client->read());
      if (c == '\n') {
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return true;
      }
      out.push_back(c);
    }
    return false;
  };

  std::string line;
  if (!wait_line(line)) {
    ESP_LOGE(TAG, "OTA fetch: status line timeout");
    client->stop();
    return false;
  }
  ESP_LOGD(TAG, "OTA fetch status: %s", line.c_str());
  // Status line: "HTTP/1.1 200 OK"
  size_t sp1 = line.find(' ');
  int status = (sp1 == std::string::npos) ? 0 : std::atoi(line.c_str() + sp1 + 1);
  if (status != 200) {
    ESP_LOGW(TAG, "OTA fetch: HTTP %d", status);
    client->stop();
    return false;
  }

  size_t content_length = 0;
  bool   have_clen = false;
  while (wait_line(line)) {
    if (line.empty()) break;   // header/body separator
    // Case-insensitive Content-Length match.
    if (line.size() >= 16 &&
        (line.compare(0, 15, "Content-Length:") == 0 ||
         line.compare(0, 15, "content-length:") == 0)) {
      const char *v = line.c_str() + 15;
      while (*v == ' ') ++v;
      content_length = (size_t) std::atoll(v);
      have_clen = true;
    }
  }
  if (!have_clen || content_length == 0u) {
    ESP_LOGW(TAG, "OTA fetch: missing/zero Content-Length");
    client->stop();
    return false;
  }
  if (content_length > OTA_PUSH_MAX_IMAGE_BYTES) {
    ESP_LOGW(TAG, "OTA fetch: %u bytes exceeds cap %u",
             unsigned(content_length), unsigned(OTA_PUSH_MAX_IMAGE_BYTES));
    client->stop();
    return false;
  }

  // Stream body straight into ota_buf_. This avoids a doubled
  // allocation (body + ota_buf_) that fragmented the FC41D heap on
  // 48 KB images. ota_buf_ is the state machine's source of truth
  // anyway; we just fill it here and call the state-machine half of
  // start_ota_push() directly.
  if (ota_push_active()) {
    ESP_LOGW(TAG, "OTA fetch: push already active (state=%s) — refusing",
             ota_state_name());
    client->stop();
    return false;
  }
  ota_buf_.clear();
  ota_buf_.shrink_to_fit();
  ota_buf_.resize(content_length);
  if (ota_buf_.size() != content_length) {
    ESP_LOGE(TAG, "OTA fetch: heap alloc failed for %u-byte ota_buf_",
             unsigned(content_length));
    ota_buf_.clear();
    ota_buf_.shrink_to_fit();
    client->stop();
    return false;
  }
  size_t got = 0;
  while (got < content_length && millis() < deadline) {
    if (!client->connected() && !client->available()) break;
    int n = client->read(ota_buf_.data() + got, content_length - got);
    if (n > 0) {
      got += size_t(n);
    } else {
      delay(5);
    }
  }
  client->stop();

  if (got != content_length) {
    ESP_LOGW(TAG, "OTA fetch: short read %u/%u",
             unsigned(got), unsigned(content_length));
    ota_buf_.clear();
    ota_buf_.shrink_to_fit();
    return false;
  }
  ESP_LOGI(TAG, "OTA fetch: got %u bytes — starting push", unsigned(got));
  return start_ota_push_from_buf_();
}

bool OpenevchargerTlv::start_ota_push(const uint8_t *data, size_t len) {
  if (ota_push_active()) {
    ESP_LOGW(TAG, "OTA push already active (state=%s) — refusing",
             ota_state_name());
    return false;
  }
  if (data == nullptr || len == 0 || len > OTA_PUSH_MAX_IMAGE_BYTES) {
    ESP_LOGW(TAG, "OTA push rejected: size=%u out of range (cap=%u)",
             unsigned(len), unsigned(OTA_PUSH_MAX_IMAGE_BYTES));
    return false;
  }
  ota_buf_.assign(data, data + len);
  if (ota_buf_.size() != len) {
    ESP_LOGE(TAG, "OTA push: heap alloc failed for %u-byte buffer",
             unsigned(len));
    ota_buf_.clear();
    ota_buf_.shrink_to_fit();
    return false;
  }
  return start_ota_push_from_buf_();
}

bool OpenevchargerTlv::start_ota_push_from_buf_() {
  ota_total_bytes_ = uint32_t(ota_buf_.size());
  ota_image_crc32_ = crc32_ieee(ota_buf_.data(), ota_buf_.size());
  // Pick a non-zero session_id from millis(). Collision-resistant enough
  // for the FC41D side (only one push at a time).
  ota_session_id_  = millis();
  if (ota_session_id_ == 0u) ota_session_id_ = 1u;
  ota_next_offset_ = 0u;
  ota_last_io_ms_  = millis();
  ota_state_       = OtaState::AWAIT_BEGIN;
  ota_progress_pct_cache_ = 0xFF;   // force first publish
  ota_publish_progress_();

  ota_seq_begin_ = send_ota_begin(ota_total_bytes_, ota_image_crc32_,
                                  ota_session_id_);
  ESP_LOGI(TAG, "OTA push started: %u bytes crc=0x%08x sid=0x%08x",
           unsigned(ota_total_bytes_), unsigned(ota_image_crc32_),
           unsigned(ota_session_id_));
  return true;
}

void OpenevchargerTlv::abort_ota_push() {
  if (!ota_push_active()) return;
  ESP_LOGI(TAG, "OTA push aborted by host (sid=0x%08x at %u/%u)",
           unsigned(ota_session_id_),
           unsigned(ota_next_offset_), unsigned(ota_total_bytes_));
  send_ota_abort(ota_session_id_);
  ota_finish_(OtaState::FAILED, "aborted by host");
}

void OpenevchargerTlv::ota_send_next_chunk_() {
  if (ota_state_ != OtaState::AWAIT_CHUNK) return;
  uint32_t remaining = ota_total_bytes_ - ota_next_offset_;
  uint8_t  this_len  = remaining > OTA_CHUNK_MAX_DATA
                         ? uint8_t(OTA_CHUNK_MAX_DATA)
                         : uint8_t(remaining);
  if (this_len == 0u) return;
  ota_seq_chunk_ = send_ota_chunk(ota_session_id_, ota_next_offset_,
                                  ota_buf_.data() + ota_next_offset_,
                                  this_len);
  ESP_LOGV(TAG, "OTA chunk off=%u len=%u seq=%u",
           unsigned(ota_next_offset_), unsigned(this_len),
           unsigned(ota_seq_chunk_));
}

void OpenevchargerTlv::ota_loop_tick_() {
  if (!ota_push_active()) return;
  uint32_t now = millis();
  if (now - ota_last_io_ms_ > ota_op_timeout_ms_) {
    ESP_LOGW(TAG, "OTA timeout waiting in state %s (%u ms idle)",
             ota_state_name(), unsigned(now - ota_last_io_ms_));
    ota_finish_(OtaState::FAILED, "ack timeout");
  }
}

void OpenevchargerTlv::ota_finish_(OtaState end_state, const char *why) {
  ESP_LOGI(TAG, "OTA push finished: %s (state=%s last_status=%u)",
           why, ota_state_name(), unsigned(ota_last_status_));
  ota_state_ = end_state;
  // Free the buffer immediately; ota_progress_pct() falls back on cached
  // values for the post-push UI snapshot.
  ota_buf_.clear();
  ota_buf_.shrink_to_fit();
  ota_publish_progress_();
}

uint8_t OpenevchargerTlv::ota_progress_pct() const {
  if (ota_state_ == OtaState::DONE) return 100u;
  if (ota_total_bytes_ == 0u) return 0u;
  uint64_t pct = (uint64_t(ota_next_offset_) * 100u) / ota_total_bytes_;
  if (pct > 100u) pct = 100u;
  return uint8_t(pct);
}

void OpenevchargerTlv::ota_publish_progress_() {
#ifdef USE_SENSOR
  uint8_t pct = ota_progress_pct();
  if (pct != ota_progress_pct_cache_) {
    ota_progress_pct_cache_ = pct;
    if (ota_progress_sensor_) ota_progress_sensor_->publish_state(float(pct));
  }
#endif
}

const char *OpenevchargerTlv::ota_state_name() const {
  switch (ota_state_) {
    case OtaState::IDLE:         return "IDLE";
    case OtaState::AWAIT_BEGIN:  return "AWAIT_BEGIN";
    case OtaState::AWAIT_CHUNK:  return "AWAIT_CHUNK";
    case OtaState::AWAIT_COMMIT: return "AWAIT_COMMIT";
    case OtaState::DONE:         return "DONE";
    case OtaState::FAILED:       return "FAILED";
  }
  return "?";
}

// --- Helpers ------------------------------------------------------------

uint16_t OpenevchargerTlv::crc16_ccitt_(const uint8_t *p, size_t n) {
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

const char *OpenevchargerTlv::evse_state_name(uint8_t s) {
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

const char *OpenevchargerTlv::j1772_state_name(uint8_t s) {
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

const char *OpenevchargerTlv::fault_name(uint32_t id) {
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
void OpenevchargerTlvNumber::setup() {
  if (parent_) publish_from_state();
}

void OpenevchargerTlvNumber::publish_from_state() {
  if (!parent_) return;
  switch (kind_) {
    case NumberKind::ADVERTISED_AMPS: {
      auto v = float(parent_->state().advertised_amps);
      if (v != this->state) this->publish_state(v);
      break;
    }
  }
}

void OpenevchargerTlvNumber::control(float value) {
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
void OpenevchargerTlvButton::press_action() {
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
    case ButtonAction::PUSH_BL0939_CAL: parent_->send_write_bl0939_cal_from_yaml(); break;
    case ButtonAction::RFID_LEARN_NEXT: parent_->send_rfid_learn_next(); break;
    case ButtonAction::RFID_CLEAR_LIST: parent_->send_rfid_clear_list(); break;
    case ButtonAction::RFID_GET_LIST: parent_->send_rfid_get_list(); break;
    case ButtonAction::OTA_ABORT: parent_->abort_ota_push(); break;
    case ButtonAction::RESTART: parent_->send_restart(); break;
    case ButtonAction::SIMULATE_REPLUG: parent_->send_simulate_replug(); break;
    case ButtonAction::RUN_GFCI_CAL_TEST: parent_->send_run_gfci_cal_test(); break;
  }
}
#endif

}  // namespace openevcharger_tlv
}  // namespace esphome
