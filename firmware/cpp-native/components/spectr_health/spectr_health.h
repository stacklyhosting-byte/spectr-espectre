/*
 * Spectr Link Health Monitor.
 *
 * Two tiers:
 *   - Routine (1 Hz): cumulative-delta link health, cloud-link loss, degraded states.
 *   - Urgent (250 ms): fast link-loss / possible-interference trigger (provisional
 *     within ~0.75 s), escalated only when interference evidence persists.
 *
 * The pre-computed diagnostics rates freeze exactly when the link degrades, so this
 * monitor derives its own deltas from cumulative counters. Alerts are buffered in NVS
 * and published to MQTT when the broker is reachable.
 *
 * Conservative by design: it reports "possible interference", never a definitive
 * jamming claim, and it never touches the presence-sensing path.
 */
#pragma once

#include <cstdint>
#include <string>

#include "native_frontend.h"

namespace spectr {

class SpectrHealthMonitor {
 public:
  void begin(espectre::NativeFrontend *frontend);
  /** Call from the frontend loop task; runs the fast (250 ms) and routine (1 Hz) tiers. */
  void loop(uint32_t now_ms);

 private:
  enum class State { kHealthy, kDegraded, kInterference, kLinkLost };

  struct Counters {
    uint64_t callbacks{0};
    uint64_t accepted{0};
    uint64_t hw_errors{0};
    uint64_t missing{0};
    uint64_t provenance{0};
    uint32_t tx{0};
    uint32_t rx{0};
    bool valid{false};
  };

  void fast_sample_(uint32_t now_ms);
  void sample_(uint32_t now_ms);
  std::string alert_json_(const char *type, const char *severity, const char *reason,
                          const char *trigger, const char *confidence, uint32_t latency_ms) const;
  void publish_or_queue_(const std::string &json);
  void queue_(const std::string &json);
  void flush_queue_();
  void open_store_();

  espectre::NativeFrontend *frontend_{nullptr};
  bool store_ready_{false};

  Counters previous_{};
  uint32_t last_sample_ms_{0};
  uint32_t started_ms_{0};
  uint32_t bad_since_ms_{0};
  uint32_t healthy_since_ms_{0};
  uint32_t mqtt_down_since_ms_{0};
  uint32_t last_alert_ms_{0};
  State state_{State::kHealthy};
  State candidate_{State::kHealthy};

  // Fast tier (250 ms).
  Counters fast_previous_{};
  uint32_t last_fast_ms_{0};
  uint32_t fast_streak_start_ms_{0};
  uint8_t fast_bad_streak_{0};
  uint8_t fast_evidence_streak_{0};
  uint8_t fast_good_streak_{0};
  bool provisional_active_{false};
  bool provisional_upgraded_{false};
  const char *provisional_type_{nullptr};
  uint32_t provisional_since_ms_{0};
  bool was_ready_{false};

  static constexpr uint8_t kQueueMax = 16;
  uint8_t head_{0};
  uint8_t count_{0};
};

}  // namespace spectr
