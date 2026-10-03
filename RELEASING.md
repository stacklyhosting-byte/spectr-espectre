# Releasing Spectr firmware (GPLv3 compliance runbook)

Version scheme: `spectr-vX.Y.Z`. Each release tag is the exact source for the
firmware flashed on the devices of that batch.

## Release checklist

1. Changes land on branch `spectr/1.0.x`; update the modification table in
   `PROVENANCE.md`.
2. Tag and push: `git tag -a spectr-vX.Y.Z -m "Spectr firmware X.Y.Z"` followed by
   `git push origin spectr/1.0.x spectr-vX.Y.Z`.
3. Create the GitHub Release from the tag.
4. Attach:
   - firmware binaries `ESP32_CSI*.bin` (hashes recorded in
     `compliance/SHA256SUMS`, verified by `micro-espectre/me`), and
   - the compliance bundle `spectr-compliance-vX.Y.Z.zip` (this repository's
     `LICENSE`, `PROVENANCE.md`, `THIRD_PARTY_NOTICES.md`, `SOURCE-OFFER.md`,
     `RELEASING.md`, `compliance/SHA256SUMS`).
5. Verify the source tarball auto-attached to the release matches the flashed
   firmware (build/flash instructions below).
6. Record in the main repository's `docs/OPEN-SOURCE-COMPLIANCE.md`: device
   model, firmware version, tag, source URL, release date. Keep records for at
   least three years after the last shipment.

## Building and flashing

```bash
python -m venv venv && . venv/bin/activate
pip install -r micro-espectre/requirements.txt
cd micro-espectre
./me flash --erase      # one-time: flashes the pinned MicroPython CSI image
./me deploy             # uploads the .py sources (including root main.py)
./me run                # starts the application
```

Firmware binary download URL and hashes are pinned in `micro-espectre/me`.

## Installation Information policy (GPLv3 §6)

Spectr devices are "User Products" under GPLv3. Therefore:

- Secure Boot and Flash Encryption MUST NOT be enabled in a way that prevents
  installing modified firmware. Do not lock the bootloader.
- The UART flashing path and instructions must ship with the device so anyone
  can rebuild from the published source and flash it.
- A future product that requires a locked bootloader must either obtain a
  commercial ESPectre license or keep GPLv3 firmware on an unlocked device.

## Data privacy

`micro-espectre/src/config_local.py` contains Wi-Fi credentials and is
git-ignored. Never commit it. Published source releases must not contain real
credentials, device identifiers, or customer data.
