# Provenance — Stackly Spectr firmware fork

This repository (`stacklyhosting-byte/spectr-espectre`) is a fork of
[`francescopace/espectre`](https://github.com/francescopace/espectre) maintained by
Stackly Hosting for the Spectr (Aether) Wi-Fi CSI sensing product.

## Why this fork / pin

Upstream `main` (3.x) moved Micro-ESPectre to a native C SDK with a Direct HTTP
interface and no MQTT support. The Spectr backend consumes the MQTT payloads of the
2.x Micro-ESPectre application (`home/espectre/<device>`). To keep firmware and
backend compatible and reproducible, this fork pins the last MQTT-based release.

- Base project: ESPectre
- Base tag: `2.8.0`
- Base commit: `29e457a0cf4251d681905f0df60832988f2f7559` (2026-05-21)
- Product branch: `spectr/1.0.x`
- Product release tag: `spectr-v1.0.0`

## Local modifications (relative to tag 2.8.0)

Fork modifications by Stackly Hosting, 2026-10-03:

| File | Change |
|---|---|
| `micro-espectre/main.py` | **Added.** Standalone boot shim: creates the WLAN object at module level and passes it to `src.main.main()` (fixes ESP32-S3 cold-boot "WiFi Out of Memory"). |
| `micro-espectre/src/main.py` | Dual-detector runtime: MVS remains the decision-maker; ML runs in parallel for logging (`ml_score`). Idempotent WiFi connect with pre-created WLAN support; extra GC before WiFi; publishes `turbulence` and `ml_score`. |
| `micro-espectre/src/config.py` | `CALIBRATION_NUM_WINDOWS` 10 → 5; `ENABLE_LOWPASS_FILTER` False → True. |
| `micro-espectre/src/mqtt/handler.py` | `publish_state()` adds `turbulence`, `ml_score`, `gain_lock`, `algorithm`, `rssi`; accepts `ml_detector` for logging. |
| `micro-espectre/src/mqtt/commands.py` | ML logging-only integration; `factory_reset` resets both detectors; info reports `ml_enabled` and always uses the MVS calibrator. |
| `micro-espectre/me` | Deploys the new root `main.py`; firmware download URL pinned to this repository's release assets. |
| `README.md` | Fork notice. |

All other files under `micro-espectre/` are unmodified from tag `2.8.0`.

## Licensing

ESPectre is licensed under GPLv3 (see `LICENSE`). This fork and all modifications
to it are distributed under GPLv3. See `THIRD_PARTY_NOTICES.md` and
`SOURCE-OFFER.md`.

`micro-espectre` is an independent device application. The Spectr backend and web
application communicate with it over MQTT/HTTP and are not part of this work.
