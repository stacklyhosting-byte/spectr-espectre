#include "spectr_health.h"

#include <cstdio>

#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>

namespace spectr {
namespace {

constexpr const char *kTag = "spectr.health";
constexpr const char *kNvsNamespace = "spectr_health";
constexpr uint32_t kSampleMs = 1000;
constexpr uint32_t kStartupGraceMs = 30000;
constexpr uint32_t kBadDwellMs = 4000;
constexpr uint32_t kLostDwellMs = 6000;
constexpr uint32_t kRealertMs = 300000;
constexpr uint32_t kMqttDownAlertMs = 30000;

uint32_t wrap_delta_u32(uint32_t now, uint32_t previous) {
  return now >= previous ? now - previous : (0xFFFFFFFFu - previous) + now + 1u;
}

}  // namespace

void SpectrHealthMonitor::begin(espectre::NativeFrontend *frontend) {
  frontend_ = frontend;
  open_store_();
  started_ms_ = static_cast<uint32_t>(esp_timer_get_time() / 1000);
  ESP_LOGI(kTag, "link health monitor started (buffered alerts: %u)", static_cast<unsigned>(count_));
}

void SpectrHealthMonitor::open_store_() {
  nvs_handle_t handle = 0;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    ESP_LOGW(kTag, "alert store unavailable");
    return;
  }
  store_ready_ = true;
  nvs_get_u8(handle, "head", &head_);
  nvs_get_u8(handle, "count", &count_);
  if (count_ > kQueueMax) count_ = kQueueMax;
  nvs_close(handle);
}

void SpectrHealthMonitor::loop(uint32_t now_ms) {
  if (frontend_ == nullptr) return;
  if (now_ms - last_sample_ms_ < kSampleMs) return;
  last_sample_ms_ = now_ms;
  sample_(now_ms);
  flush_queue_();
}

void SpectrHealthMonitor::sample_(uint32_t now_ms) {
  const espectre::RuntimeDiagnosticsSnapshot d = frontend_->diagnostics();
  const espectre::RuntimeSnapshot snap = frontend_->snapshot();

  Counters now{};
  now.callbacks = d.csi.callbacks_total;
  now.accepted = d.csi.accepted_total;
  now.hw_errors = d.csi.rx_error_total + d.csi.rx_end_error_total +
                  d.csi.invalid_estimate_total + d.csi.invalid_first_word_total;
  now.missing = d.csi.missing_slots_total;
  now.provenance = d.csi.provenance_rejected_total;
  now.tx = d.traffic.tx_packets_total;
  now.rx = d.traffic.rx_packets_total;

  if (!previous_.valid) {
    previous_ = now;
    previous_.valid = true;
    return;
  }

  const uint32_t rx_delta = wrap_delta_u32(now.rx, previous_.rx);
  const uint32_t tx_delta = wrap_delta_u32(now.tx, previous_.tx);
  const uint64_t hw_delta = now.hw_errors >= previous_.hw_errors ? now.hw_errors - previous_.hw_errors : 0;
  const uint64_t accepted_delta = now.accepted >= previous_.accepted ? now.accepted - previous_.accepted : 0;
  const double occupancy = d.csi.window_slots > 0
                               ? static_cast<double>(d.csi.occupancy_slots) / d.csi.window_slots
                               : 0.0;
  previous_ = now;

  const bool mqtt_up = frontend_->mqtt_connected();
  const bool ready = snap.ready_to_publish;
  const bool calibrating = snap.calibrating;

  if (!mqtt_up) {
    if (mqtt_down_since_ms_ == 0) {
      mqtt_down_since_ms_ = now_ms;
    } else if (now_ms - mqtt_down_since_ms_ >= kMqttDownAlertMs &&
               (last_alert_ms_ == 0 || now_ms - last_alert_ms_ >= kRealertMs)) {
      queue_(alert_json_("cloud_link_lost", "warning", "broker_unreachable"));
      last_alert_ms_ = now_ms;
      ESP_LOGW(kTag, "cloud link down for %u s; alert buffered",
               static_cast<unsigned>((now_ms - mqtt_down_since_ms_) / 1000));
    }
  } else {
    mqtt_down_since_ms_ = 0;
  }

  if (now_ms - started_ms_ < kStartupGraceMs || calibrating) {
    state_ = State::kHealthy;
    candidate_ = State::kHealthy;
    bad_since_ms_ = 0;
    return;
  }

  State target = State::kHealthy;
  const char *reason = "healthy";
  if (!ready) {
    if (hw_delta > 10 && rx_delta * 4 < tx_delta) {
      target = State::kInterference;
      reason = "possible_interference";
    } else if (rx_delta == 0 && tx_delta > 0) {
      target = State::kLinkLost;
      reason = "no_received_frames";
    } else if (occupancy > 0.0 && occupancy < 0.5) {
      target = State::kDegraded;
      reason = "low_occupancy";
    } else if (rx_delta == 0) {
      target = State::kLinkLost;
      reason = "sensing_stopped";
    }
  } else if (occupancy > 0.0 && occupancy < 0.4) {
    target = State::kDegraded;
    reason = "low_occupancy";
  }

  if (target == state_) {
    bad_since_ms_ = 0;
    if (state_ != State::kHealthy && now_ms - last_alert_ms_ >= kRealertMs) {
      const char *type = state_ == State::kInterference ? "jamming_suspected"
                        : state_ == State::kLinkLost    ? "link_lost"
                                                        : "link_degraded";
      const char *severity = state_ == State::kDegraded ? "info" : "warning";
      publish_or_queue_(alert_json_(type, severity, reason));
      last_alert_ms_ = now_ms;
    }
    return;
  }

  if (target == State::kHealthy) {
    publish_or_queue_(alert_json_("link_restored", "info", "link_recovered"));
    ESP_LOGI(kTag, "sensing link recovered");
    state_ = State::kHealthy;
    candidate_ = State::kHealthy;
    bad_since_ms_ = 0;
    return;
  }

  if (candidate_ != target || bad_since_ms_ == 0) {
    candidate_ = target;
    bad_since_ms_ = now_ms;
    return;
  }

  const uint32_t dwell = target == State::kLinkLost ? kLostDwellMs : kBadDwellMs;
  if (now_ms - bad_since_ms_ < dwell) return;

  const char *type = target == State::kInterference ? "jamming_suspected"
                    : target == State::kLinkLost    ? "link_lost"
                                                    : "link_degraded";
  const char *severity = target == State::kDegraded ? "info" : "warning";
  publish_or_queue_(alert_json_(type, severity, reason));
  ESP_LOGW(kTag, "link health -> %s (%s)", type, reason);
  state_ = target;
  candidate_ = target;
  bad_since_ms_ = 0;
  last_alert_ms_ = now_ms;
}

std::string SpectrHealthMonitor::alert_json_(const char *type, const char *severity, const char *reason) const {
  const espectre::RuntimeDiagnosticsSnapshot d = frontend_->diagnostics();
  const espectre::RuntimeSnapshot snap = frontend_->snapshot();
  const double occupancy = d.csi.window_slots > 0
                               ? static_cast<double>(d.csi.occupancy_slots) / d.csi.window_slots
                               : 0.0;
  char buffer[320];
  snprintf(buffer, sizeof(buffer),
           "{\"type\":\"%s\",\"severity\":\"%s\",\"reason\":\"%s\",\"rssi\":%d,\"channel\":%u,"
           "\"occupancy\":%.2f,\"ready\":%s,\"uptime_s\":%u,\"source\":\"spectr-health\"}",
           type, severity, reason, static_cast<int>(d.link.rssi_dbm),
           static_cast<unsigned>(d.link.channel), occupancy,
           snap.ready_to_publish ? "true" : "false",
           static_cast<unsigned>(esp_timer_get_time() / 1000000));
  return std::string(buffer);
}

bool SpectrHealthMonitor::publish_or_queue_(const std::string &json) {
  if (frontend_ != nullptr && frontend_->publish_spectr_alert(json)) return true;
  queue_(json);
  return false;
}

void SpectrHealthMonitor::queue_(const std::string &json) {
  if (!store_ready_) return;
  nvs_handle_t handle = 0;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
  char key[8];
  if (count_ == kQueueMax) {
    snprintf(key, sizeof(key), "q%u", static_cast<unsigned>(head_));
    nvs_erase_key(handle, key);
    head_ = static_cast<uint8_t>((head_ + 1) % kQueueMax);
    count_--;
  }
  const uint8_t slot = static_cast<uint8_t>((head_ + count_) % kQueueMax);
  snprintf(key, sizeof(key), "q%u", static_cast<unsigned>(slot));
  nvs_set_str(handle, key, json.c_str());
  nvs_set_u8(handle, "head", head_);
  nvs_set_u8(handle, "count", static_cast<uint8_t>(count_ + 1));
  nvs_commit(handle);
  nvs_close(handle);
  count_++;
}

void SpectrHealthMonitor::flush_queue_() {
  if (!store_ready_ || count_ == 0) return;
  if (frontend_ == nullptr || !frontend_->mqtt_connected()) return;
  nvs_handle_t handle = 0;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
  while (count_ > 0) {
    char key[8];
    snprintf(key, sizeof(key), "q%u", static_cast<unsigned>(head_));
    size_t length = 0;
    if (nvs_get_str(handle, key, nullptr, &length) != ESP_OK || length == 0) {
      nvs_erase_key(handle, key);
      head_ = static_cast<uint8_t>((head_ + 1) % kQueueMax);
      count_--;
      continue;
    }
    std::string payload(length - 1, '\0');
    if (nvs_get_str(handle, key, payload.data(), &length) != ESP_OK) break;
    if (!frontend_->publish_spectr_alert(payload)) break;
    nvs_erase_key(handle, key);
    head_ = static_cast<uint8_t>((head_ + 1) % kQueueMax);
    count_--;
  }
  nvs_set_u8(handle, "head", head_);
  nvs_set_u8(handle, "count", count_);
  nvs_commit(handle);
  nvs_close(handle);
}

}  // namespace spectr
