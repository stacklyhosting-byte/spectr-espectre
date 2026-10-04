/*
 * Spectr - SoftAP provisioning portal (public interface)
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

namespace spectr {

/**
 * True when the device has no saved Wi-Fi credentials and no build-time SSID,
 * so it must run the setup hotspot before the normal firmware can start.
 */
bool provisioning_required();

/**
 * Run the SoftAP captive portal: hotspot, Wi-Fi scan, credential hand-off,
 * connection verification, then persist and reboot into the normal firmware.
 *
 * Blocking and terminal: only returns if the portal cannot start, after which
 * the caller should continue with the normal boot flow.
 */
void run_provisioning_portal();

}  // namespace spectr
