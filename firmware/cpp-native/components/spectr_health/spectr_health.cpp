#include "spectr_health.h"

#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>

namespace spectr {
namespace {

constexpr const char *kTag = "spectr.health";
constexpr const char *kNvsNamespace = "spectr_health";

// Routine tier.
constexpr uint32_t kSampleMs = 1000;
constexpr uint32_t kStartupGraceMs = 30000;
constexpr uint32_t kBadDwellMs = 4000;
constexpr uint32_t kLostDwellMs = 6000;
constexpr uint32_t kHealthyDwellMs = 4000;
constexpr uint32_t kRealertMs = 300000;
constexpr uint32_t kMqttDownAlertMs = 30000;

// Fast tier (aligned with the detector evaluation cadence).
constexpr uint32_t kFastSampleMs = 250;
constexpr uint8_t kFastBadSamples = 3;       // ~0.75 s of a sustained bad signature
constexpr uint8_t kFastEvidenceSamples = 2;  // interference evidence needed for the jamming label
constexpr uint8_t kFastGoodSamples = 8;      // ~2 s of good samples before declaring recovery
constexpr int8_t kNoiseFloorJamDbm = -70;    // fresh noise floor above this is interference evidence

uint32_t wrap_delta_u32(uint32_t now, uint32_t previous) {
  return now >= previous ? now - previous : (0xFFFFFFFFu - previous) + now + 1u;
}

uint64_t monotonic_delta_u64(uint64_t now, uint64_t previous) {
  return now >= previous ? now - previous : 0;
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
  if (now_ms - last_fast_ms_ >= kFastSampleMs) {
    last_fast_ms_ = now_ms;
    fast_sample_(now_ms);
    flush_queue_();
  }
  if (now_ms - last_sample_ms_ >= kSampleMs) {
    last_sample_ms_ = now_ms;
    sample_(now_ms);
  }
}

void SpectrHealthMonitor::fast_sample_(uint32_t now_ms) {
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

  if (!fast_previous_.valid) {
    fast_previous_ = now;
    fast_previous_.valid = true;
    return;
  }

  const uint64_t callbacks_delta = monotonic_delta_u64(now.callbacks, fast_previous_.callbacks);
  const uint64_t accepted_delta = monotonic_delta_u64(now.accepted, fast_previous_.accepted);
  const uint64_t hw_delta = monotonic_delta_u64(now.hw_errors, fast_previous_.hw_errors);
  const int8_t noise_floor = d.link.noise_floor_dbm;
  const bool noise_fresh = callbacks_delta > 0;
  const bool noise_high = noise_fresh && noise_floor > kNoiseFloorJamDbm;
  fast_previous_ = now;

  const bool ready = snap.ready_to_publish;
  if (ready) was_ready_ = true;

  const bool armed = now_ms - started_ms_ >= kStartupGraceMs && !snap.calibrating;
  if (!armed) {
    fast_bad_streak_ = 0;
    fast_evidence_streak_ = 0;
    fast_good_streak_ = 0;
    return;
  }

  // Interference: callbacks arrive (fresh noise floor) but nothing is usable.
  const bool interference =
      callbacks_delta > 0 && accepted_delta == 0 && (hw_delta > 0 || noise_high);
  // Missing expected input: the link has been ready at some point and now all CSI
  // input has stopped. When the AP dies, ESPectre also stops its traffic generator,
  // so a TX-active signature is not reliable — expected-vs-actual input is.
  const bool blackout =
      was_ready_ && !ready && callbacks_delta == 0 && accepted_delta == 0;
  const bool bad = interference || blackout;

  if (bad) {
    if (fast_bad_streak_ == 0) fast_streak_start_ms_ = now_ms;
    if (fast_bad_streak_ < UINT8_MAX) fast_bad_streak_++;
    if (interference) {
      if (fast_evidence_streak_ < UINT8_MAX) fast_evidence_streak_++;
    } else {
      fast_evidence_streak_ = 0;
    }
    fast_good_streak_ = 0;
  } else {
    fast_bad_streak_ = 0;
    fast_evidence_streak_ = 0;
    if (provisional_active_ && fast_good_streak_ < UINT8_MAX) fast_good_streak_++;
  }

  const bool evidence = fast_evidence_streak_ >= kFastEvidenceSamples;

  // Provisional trigger.
  if (!provisional_active_ && fast_bad_streak_ >= kFastBadSamples) {
    const uint32_t latency_ms = now_ms - fast_streak_start_ms_;
    provisional_active_ = true;
    provisional_upgraded_ = evidence;
    provisional_type_ = evidence ? "jamming_suspected" : "link_lost";
    provisional_since_ms_ = fast_streak_start_ms_;
    publish_or_queue_(alert_json_(provisional_type_, "warning",
                                  evidence ? "possible_interference" : "sensing_link_lost",
                                  "fast_path", "provisional", latency_ms));
    state_ = evidence ? State::kInterference : State::kLinkLost;
    candidate_ = state_;
    bad_since_ms_ = 0;
    last_alert_ms_ = now_ms;
    ESP_LOGW(kTag, "fast path -> %s (provisional, %u ms)",
             provisional_type_, static_cast<unsigned>(latency_ms));
  }

  // Escalate a provisional link_lost to possible interference when evidence appears.
  if (provisional_active_ && !provisional_upgraded_ && evidence) {
    provisional_upgraded_ = true;
    provisional_type_ = "jamming_suspected";
    publish_or_queue_(alert_json_("jamming_suspected", "warning", "possible_interference",
                                  "fast_path", "confirmed", now_ms - provisional_since_ms_));
    state_ = State::kInterference;
    candidate_ = State::kInterference;
    last_alert_ms_ = now_ms;
    ESP_LOGW(kTag, "fast path -> jamming_suspected (confirmed)");
  }

  // Recovery.
  if (provisional_active_ && fast_good_streak_ >= kFastGoodSamples) {
    publish_or_queue_(alert_json_("link_restored", "info", "link_recovered",
                                  "fast_path", "confirmed", now_ms - provisional_since_ms_));
    provisional_active_ = false;
    provisional_upgraded_ = false;
    provisional_type_ = nullptr;
    provisional_since_ms_ = 0;
    fast_good_streak_ = 0;
    state_ = State::kHealthy;
    candidate_ = State::kHealthy;
    last_alert_ms_ = now_ms;
    ESP_LOGI(kTag, "fast path -> link recovered");
  }
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
  const uint64_t hw_delta = monotonic_delta_u64(now.hw_errors, previous_.hw_errors);
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
      queue_(alert_json_("cloud_link_lost", "warning", "broker_unreachable",
                         "routine", "confirmed", 0));
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

  // The fast tier owns an active incident; do not duplicate its alerts.
  if (provisional_active_) {
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
    healthy_since_ms_ = 0;
    if (state_ != State::kHealthy && now_ms - last_alert_ms_ >= kRealertMs) {
      const char *type = state_ == State::kInterference ? "jamming_suspected"
                        : state_ == State::kLinkLost    ? "link_lost"
                                                        : "link_degraded";
      const char *severity = state_ == State::kDegraded ? "info" : "warning";
      publish_or_queue_(alert_json_(type, severity, reason, "routine", "confirmed", 0));
      last_alert_ms_ = now_ms;
    }
    return;
  }

  if (target == State::kHealthy) {
    // Recovery hysteresis: a congested link can oscillate around the occupancy
    // threshold; require a sustained healthy window before declaring recovery.
    if (healthy_since_ms_ == 0) healthy_since_ms_ = now_ms;
    if (now_ms - healthy_since_ms_ < kHealthyDwellMs) return;
    publish_or_queue_(alert_json_("link_restored", "info", "link_recovered",
                                  "routine", "confirmed", 0));
    ESP_LOGI(kTag, "sensing link recovered");
    state_ = State::kHealthy;
    candidate_ = State::kHealthy;
    bad_since_ms_ = 0;
    healthy_since_ms_ = 0;
    return;
  }
  healthy_since_ms_ = 0;

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
  publish_or_queue_(alert_json_(type, severity, reason, "routine", "confirmed", 0));
  ESP_LOGW(kTag, "link health -> %s (%s)", type, reason);
  state_ = target;
  candidate_ = target;
  bad_since_ms_ = 0;
  last_alert_ms_ = now_ms;
}

std::string SpectrHealthMonitor::alert_json_(const char *type, const char *severity, const char *reason,
                                             const char *trigger, const char *confidence,
                                             uint32_t latency_ms) const {
  const espectre::RuntimeDiagnosticsSnapshot d = frontend_->diagnostics();
  const espectre::RuntimeSnapshot snap = frontend_->snapshot();
  const double occupancy = d.csi.window_slots > 0
                               ? static_cast<double>(d.csi.occupancy_slots) / d.csi.window_slots
                               : 0.0;
  char buffer[480];
  snprintf(buffer, sizeof(buffer),
           "{\"type\":\"%s\",\"severity\":\"%s\",\"reason\":\"%s\",\"trigger\":\"%s\","
           "\"confidence\":\"%s\",\"latency_ms\":%u,\"uptime_ms\":%u,\"rssi\":%d,\"noise_floor\":%d,"
           "\"channel\":%u,\"occupancy\":%.2f,\"ready\":%s,\"source\":\"spectr-health\"}",
           type, severity, reason, trigger, confidence, static_cast<unsigned>(latency_ms),
           static_cast<unsigned>(esp_timer_get_time() / 1000),
           static_cast<int>(d.link.rssi_dbm), static_cast<int>(d.link.noise_floor_dbm),
           static_cast<unsigned>(d.link.channel), occupancy,
           snap.ready_to_publish ? "true" : "false");
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
