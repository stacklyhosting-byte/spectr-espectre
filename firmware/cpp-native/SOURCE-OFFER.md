# Source offer — Spectr Node (C++ firmware)

This directory contains the complete corresponding source of the **Spectr Node**
firmware for the ESP32-S3, built on **ESPectre 3.x** (GPL-3.0-only).

- Upstream: ESPectre `3.0.0-rc3` (Native frontend + SDK), vendored under
  `vendor/espectre/`.
- Our modifications and the exact upstream tag/commit are listed in `UPSTREAM.md`.
- Build: ESP-IDF 5.5.5, per `README.md` (the same toolchain used for the released
  images).
- Released images: see the `spectr-node-v1.0.3` release in this repository
  (factory image, OTA image, and SHA-256 checksums).

GPLv3 §6: the complete source required to build the shipped binaries is provided
here, with the modification history in `UPSTREAM.md`. The earlier MicroPython line
(Spectr firmware 1.0.0) remains available on the `spectr/1.0.x` branch and its
`spectr-v1.0.0` release.
