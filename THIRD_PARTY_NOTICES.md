# Third-party notices

This repository redistributes upstream ESPectre material and MicroPython CSI
firmware binaries.

## ESPectre / Micro-ESPectre

- Copyright (C) Francesco Pace and contributors
- License: GNU General Public License v3.0 (`GPL-3.0`), full text in `LICENSE`
- Source: https://github.com/francescopace/espectre

## MicroPython CSI firmware binaries

The release assets named `ESP32_CSI*.bin` are builds of
[`francescopace/micropython-esp32-csi`](https://github.com/francescopace/micropython-esp32-csi)
(release `v1.0.0-rc7`), a fork of
[MicroPython](https://github.com/micropython/micropython).

- MicroPython: MIT License, Copyright (c) 2013-2026 Damien P. George and contributors
- License text: https://github.com/micropython/micropython/blob/master/LICENSE

The firmware binaries are redistributed unchanged. Their SHA-256 hashes are
recorded in `compliance/SHA256SUMS` and verified by `micro-espectre/me` before
flashing.

## Host tooling (not part of the firmware)

`micro-espectre/me` and `requirements.txt` reference host-side tools:

- `esptool` — GPL-2.0-or-later (separate flashing and image-merging tool)
- `mpremote` — MIT (MicroPython tooling)
- `paho-mqtt` — EPL-2.0 or Eclipse Distribution License 1.0
- Other packages listed in `requirements.txt` carry their own licenses.

These tools run on the operator's computer and are not linked into the firmware
image.

## Trademarks

"ESPectre" and its logo are the property of their respective owner. This fork is
not affiliated with or endorsed by the ESPectre project. The GPLv3 license does
not grant trademark rights.
