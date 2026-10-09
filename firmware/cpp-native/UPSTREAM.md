# Spectr Node — upstream provenance

Spectr Node is our product firmware for the Spectr sensor. It is built on the
**ESPectre Native frontend + SDK**, vendored into this project unchanged unless
listed below.

| | |
|---|---|
| Upstream project | https://github.com/francescopace/espectre |
| Upstream tag | `3.0.0-rc3` |
| Upstream commit | `d79b4be232a1e2e464e85efbc7091aa562aed84d` |
| Vendored path | `firmware/spectr-node/vendor/espectre/src/cpp/` |
| Upstream license | GPL-3.0-only (see the upstream repository `LICENSE`) |
| Vendored on | 2026-10-03 |

The vendored tree is a copy taken from the GitHub source archive for the tag
above, limited to `src/cpp/` (SDK, shared frontend sources, Native frontend, and
the target-private `runtime/esp_idf/espectre_config` component).

## Spectr modifications

Every change we make relative to upstream is recorded here; this file is part of
our GPLv3 corresponding-source obligation together with the published mirror.

| Date | Area | Change |
|---|---|---|
| 2026-10-03 | project | Added the Spectr project root (`CMakeLists.txt`, `main/` from `frontend/native/app`, target `sdkconfig.defaults`, `partitions.csv`). No changes to the vendored sources yet. |
| 2026-10-03 | vendored code | `frontend/native/espectre/native_direct_frontend.cpp`: append the Spectr app origins (`https://micro-espectre-five.vercel.app`, `https://app.spectr.co.za`) to `DirectHttpServiceConfig::allowed_origins` so the app's Local tools may call Direct HTTP. |
| 2026-10-03 | project config | `sdkconfig.defaults`: product overrides — High Accuracy detector default, Home Assistant discovery off, MQTT enabled with Spectr backend defaults, device label "Spectr Sensor". The MQTT password is injected at build time via the gitignored `sdkconfig.defaults.build`. |
| 2026-10-03 | app entry | `main/app_main.cpp`: first boot with no saved Wi-Fi runs the Spectr SoftAP setup portal before the normal boot flow; physical recovery restarts into setup mode; recovery log wording updated. |
| 2026-10-03 | new component | `components/spectr_provisioning/` (Spectr-original, GPLv3): SoftAP hotspot with a per-device password stored in NVS, captive portal (Wi-Fi scan, credential hand-off, connection verification, advanced MQTT settings), embedded `portal.html`, and captive DNS. Host tests in `components/spectr_provisioning/test/`. |
| 2026-10-03 | tooling | `tools/make-qr.mjs` + `tools/package.json`: printable setup label (hotspot join QR + app claim QR) for factory/production. `flash-spectr.bat`: Windows esptool flashing helper. |
| 2026-10-04 | project config | `sdkconfig.defaults`: 16 MB flash target, MQTT endpoint switched to `mqtts://mqtt.spectr.co.za:8883` (TLS with the shared bootstrap user until the app rotates to per-device credentials). |
| 2026-10-04 | project config | `partitions.csv`: 16 MB layout with two 3.875 MB OTA application slots. |
| 2026-10-04 | vendored code | `frontend/ota_protocol.cpp`: `espectre_ota_manifest_url()` points at the Spectr catalog (`https://firmware.spectr.co.za/<channel>/firmware-manifest-<channel>.json`) instead of the ESPectre GitHub releases. |
| 2026-10-04 | provisioning fix | `components/spectr_provisioning/spectr_provisioning_portal.cpp`: create the default Wi-Fi **station** netif in setup mode (previously only the AP netif existed, so the candidate network associated but never obtained an IP). Firmware 1.0.1. |
| 2026-10-05 | vendored code | `frontend/native/espectre/native_direct_frontend.cpp`: the Spectr app origin is now `https://devspectr.vercel.app` (Vercel project domain renamed); `https://app.spectr.co.za` stays. Firmware 1.0.4. |
| 2026-10-09 | vendored code | `runtime/esp_idf/mqtt_transport_esp_idf.cpp`: MQTT keepalive 20 s with 5 s reconnect / 10 s network timeouts, so broker-side Last Will offline detection is fast (defaults were ~2–3 min). |
| 2026-10-09 | vendored code | `frontend/native/espectre/native_frontend.{h,cpp}`: runtime faults are now also published to MQTT (`fault` topic; previously SSE-only), and public accessors were added for the Spectr Link Health Monitor: `diagnostics()`, `mqtt_connected()`, and `publish_spectr_alert()` which publishes to `<prefix>/<device_id>/events`. |
| 2026-10-09 | new component | `components/spectr_health/` (Spectr-original, GPLv3): Link Health Monitor — cumulative-delta link-health state machine (healthy/degraded/possible-interference/link-lost) with NVS-buffered MQTT alerts (`jamming_suspected`, `link_lost`, `link_degraded`, `link_restored`, `cloud_link_lost`). Conservative wording; never touches the sensing path. |
| 2026-10-09 | app entry | `main/app_main.cpp`: instantiate the Link Health Monitor and pump it from the frontend loop task. Firmware 1.1.0. |
| 2026-10-09 | vendored code | `runtime/esp_idf/csi_capture_service.{h,cpp}` + `runtime/esp_idf/csi_pipeline.h` + `runtime/runtime_snapshot.h` + `runtime/esp_idf/esp_idf_runtime.cpp`: record and expose the receiver noise floor (`rx_ctrl.noise_floor`) of every CSI callback as `RuntimeDiagnosticsSnapshot::Link::noise_floor_dbm`, for the Link Health Monitor's fast interference discriminator. |
| 2026-10-09 | component | `components/spectr_health/`: two-tier Link Health Monitor — 1 Hz routine tier plus a 250 ms urgent tier (3 consecutive bad samples → provisional `link_lost`; interference evidence upgrades to `jamming_suspected`), alerts carrying `trigger`, `confidence`, `latency_ms`, `uptime_ms`, `noise_floor`. Firmware 1.2.0. |
| 2026-10-09 | component | `components/spectr_health/`: all alerts are now persisted to the NVS ring first and delivered only while the sensing link is ready AND MQTT is connected. This stops an urgent alert being lost into a doomed MQTT outbox at the moment of a link drop (the client still reports connected for a few seconds). Firmware 1.2.4. |
| 2026-10-09 | runtime | `runtime/esp_idf/mqtt_transport_esp_idf.cpp`: `publish_suffix` marked every topic except `commands/result` as *replaceable*, so a later alert on the same `events` topic silently overwrote an earlier one in the pending-publish queue — a buffered `link_lost` was lost when `link_restored` was flushed in the same batch. `events` and `fault` are now non-replaceable, and non-replaceable messages publish at QoS 1 with `store=true` so they survive a reconnect. Firmware 1.2.7. |

## Re-sync procedure

1. Download the new upstream tag archive.
2. Replace `vendor/espectre/src/cpp/` with the new `src/cpp/`.
3. Re-apply the changes recorded above (the table must stay complete).
4. Update the tag/commit/date in this file.
5. Rebuild and re-run the parity checklist before any release.
