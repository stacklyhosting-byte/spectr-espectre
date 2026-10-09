/*
 * Spectr Link Health Monitor.
 *
 * Detects an unhealthy sensing link (degraded / lost / possible interference) from
 * cumulative diagnostics deltas. The pre-computed diagnostics rates freeze exactly
 * when the link degrades, so this monitor derives its own per-second deltas.
 * Alerts are buffered in NVS and published to MQTT when the broker is reachable.
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
  /** Call from the frontend loop task; samples at 1 Hz. */
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

  void sample_(uint32_t now_ms);
  std::string alert_json_(const char *type, const char *severity, const char *reason) const;
  bool publish_or_queue_(const std::string &json);
  void queue_(const std::string &json);
  void flush_queue_();
  void open_store_();

  espectre::NativeFrontend *frontend_{nullptr};
  bool store_ready_{false};

  Counters previous_{};
  uint32_t last_sample_ms_{0};
  uint32_t started_ms_{0};
  uint32_t bad_since_ms_{0};
  uint32_t mqtt_down_since_ms_{0};
  uint32_t last_alert_ms_{0};
  State state_{State::kHealthy};
  State candidate_{State::kHealthy};

  static constexpr uint8_t kQueueMax = 16;
  uint8_t head_{0};
  uint8_t count_{0};
};

}  // namespace spectr
