# Spectr Node firmware

Product firmware for the Spectr sensor, built on the **ESPectre 3.x Native
frontend + SDK** (vendored under `vendor/espectre/`, see `UPSTREAM.md`).

It provides on-device Wi-Fi CSI sensing and speaks the ESPectre protocol v1.0
that the Spectr backend ingests (`espectre/v1/devices/#`): retained
`health`/`device`/`capabilities`/`sensing`/`wifi`/`ota`, `motion`, `fault`,
and `commands/request`/`commands/result`; plus Direct HTTP on port `62587` for
the app's Local tools.

## Layout

```
firmware/spectr-node/
├── CMakeLists.txt              # ESP-IDF project root (Spectr wrapper)
├── main/                       # app entry (from upstream Native app)
├── partitions.csv              # factory + OTA partitions
├── sdkconfig.defaults          # shared + per-target defaults
├── vendor/espectre/src/cpp/    # pinned upstream sources (see UPSTREAM.md)
└── UPSTREAM.md                 # pin, provenance, and our modifications
```

## Building

Compile-only; **no hardware is required**. On the VPS:

```bash
cd /opt/spectr
bash scripts/build-firmware.sh
```

Artifacts land in `/opt/spectr/firmware-out/`:
- `<ver>-<target>-factory.bin` — single image for first flash (address `0x0`)
- `<ver>-<target>-app.bin` — application image (OTA / advanced flashing)
- `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin`
- `SHA256SUMS.txt`, `size.txt`

Environment overrides: `IDF_TARGET` (default `esp32s3`), `IDF_IMAGE`
(default `espressif/idf:v5.5.5`), `SPECTR_FIRMWARE_VERSION`,
`NATIVE_OTA_CHANNEL`.

The build runs in a container; it never touches the live VPS stack and never
flashes a device.

## Onboarding (end-user flow)

1. **Plug in the sensor.** With no saved Wi-Fi it starts a hotspot named
   `Spectr-XXXX` (last four characters of the device ID), protected by a
   per-device WPA2 password generated on first boot and printed on the serial
   console and on the label.
2. **Join the hotspot** from the phone or laptop and open `http://192.168.4.1/`
   (most devices open the captive portal automatically). Choose the home Wi-Fi
   from the scan list, enter its password, optionally name the sensor, and press
   **Connect sensor**. The advanced section can override the MQTT broker settings.
3. **The sensor verifies the network**, saves it the moment it gets an address,
   then restarts into sensing mode and connects to the Spectr backend.
4. **Claim it in the app**: Devices → *Add sensor* → enter the 16-character
   device ID (or scan the claim QR from the label).

Recovery: hold **BOOT** for 5 seconds to clear the saved Wi-Fi; the device
restarts into the hotspot setup mode. Improv Serial over USB still works for
scripted provisioning.

### Labels and QR codes

```bash
cd firmware/spectr-node/tools && npm install        # once: qrcode dependency
node tools/make-qr.mjs --device-id a1b2c3d4e5f60718 \
  --ap-password K7MQP4XR9T --name "Study" --out labels
```
Writes a printable `*-label.html` (join QR + claim QR + steps) and the two SVGs.
Omit `--ap-password` while planning; the real value comes from the device's
serial log at first boot.

## Protocol parity

The firmware is the Native frontend on the pinned ESPectre SDK, so it speaks the
exact protocol the backend was built against (verified in Phase 1 against the
simulator):

| Surface | Where |
|---|---|
| MQTT retained `health`/`device`/`capabilities`/`sensing`/`wifi`/`ota` | `espectre/v1/devices/<id>/…` |
| MQTT `motion`, `fault`, `commands/result` | same prefix, unretained |
| Commands `update_device`/`update_sensing`/`recalibrate`/`read_diagnostics`/`check_ota`/`start_ota` | `commands/request` |
| Direct HTTP resources, SSE, raw CSI | `http://<ip>:62587/espectre/v1/…` |
| Availability | retained `health` + MQTT Last Will |

Differences from the simulator: real sensing, per-device hotspot provisioning,
hardware OTA channel handling, and mDNS bootstrap responses.

## Flashing (only when explicitly decided)

```powershell
# Windows helper: newest factory image, COM4 by default
.\flash-spectr.bat COM4

# or manually with esptool
esptool.py --chip esp32s3 --port COM4 write_flash 0x0 spectr-node-<ver>-esp32s3-factory.bin
```

The factory image flashes one file at `0x0` (bootloader, partition table, OTA
data, application). The application-only image is used for OTA later (Phase 5).

Rollback: the current MicroPython firmware image and the legacy bridge path stay
available; reflashing the old firmware restores the previous behaviour. A/B
validation happens before any cut-over, and the real device is flashed only on
an explicit decision.

## Ready-to-flash checklist

- [x] Upstream pinned (tag + commit) and vendored, with provenance in `UPSTREAM.md`
- [x] Warning-free ESP32-S3 build in the ESP-IDF 5.5.5 container
- [x] Factory image, app/OTA image, checksums, and size report in `firmware-out/`
- [x] SoftAP onboarding + host tests; portal HTML embedded and previewable
- [x] Label/QR tooling and the Windows flash helper
- [x] Rollback path documented (legacy MicroPython firmware stays available)
- [ ] Re-pin the SDK to the stable 3.0.0 release and rebuild before flashing
- [ ] Hardware A/B benchmark vs the MicroPython fork (P4.6, explicit go)
- [ ] GPLv3 source published to the mirror at release tagging

## Release and GPLv3 compliance

At release tagging time, publish this directory together with `UPSTREAM.md` to
the canonical mirror (`stacklyhosting-byte/spectr-espectre`) alongside
`PROVENANCE`/`SOURCE-OFFER`, and attach the factory/OTA images with their
checksums. The MQTT password is never committed or shipped in source: the build
script injects it from the host environment into a gitignored file.

## Hardware-free validation (P4.5)

- Build: `bash scripts/build-firmware.sh` (container, no hardware).
- Logic: `bash scripts/test-provisioning-logic.sh` (host tests).
- Portal: open `components/spectr_provisioning/portal.html` in a browser; it
  shows preview data when no sensor answers.
- Parity: Phase 1 verified the protocol against the simulator; the firmware is
  the Native frontend on that same SDK/protocol.
- QEMU boot smoke is optional and not wired up yet; Wi-Fi/AP behaviour is not
  emulated usefully, so hardware validation remains the P4.6 step.

## Status

Phase 4.0–4.1 delivered: the pinned Native/SDK base builds cleanly in the ESP-IDF
5.5.5 container for `esp32s3`, with product defaults (High Accuracy, HA discovery
off, MQTT to the Spectr backend with build-time password, Spectr app origins for
Direct HTTP). SoftAP provisioning (P4.2) and release packaging (P4.4) are next;
see `docs/forum-advanced/ESPECTRE-3X-PLAN.md`. **Nothing has been flashed.**
