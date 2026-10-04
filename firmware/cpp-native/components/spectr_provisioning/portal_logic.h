/*
 * Spectr - SoftAP provisioning logic (portable helpers)
 *
 * Pure, host-testable helpers for the setup portal. No ESP-IDF dependencies.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace spectr {
namespace portal_logic {

/** Wi-Fi SSIDs are 1..32 bytes. */
inline bool valid_ssid(const std::string &ssid) {
  return !ssid.empty() && ssid.size() <= 32U;
}

/** WPA2 passphrases are 8..63 characters; an empty string selects an open network. */
inline bool valid_wifi_password(const std::string &password) {
  return password.empty() || (password.size() >= 8U && password.size() <= 63U);
}

/** Setup hotspot name: `Spectr-XXXX`, using the last four device-id characters. */
inline std::string make_ap_ssid(const std::string &device_id) {
  std::string suffix = device_id;
  if (suffix.size() > 4U) {
    suffix = suffix.substr(suffix.size() - 4U);
  }
  for (char &c : suffix) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return "Spectr-" + suffix;
}

/** Unambiguous characters for a generated hotspot password (no 0/O/1/I/L). */
inline const char *ap_password_alphabet() {
  return "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
}

/** Generated hotspot passwords are 10 characters from the unambiguous alphabet. */
inline bool valid_generated_ap_password(const std::string &password) {
  if (password.size() != 10U) {
    return false;
  }
  const std::string alphabet = ap_password_alphabet();
  return std::all_of(password.begin(), password.end(), [&alphabet](char c) {
    return alphabet.find(c) != std::string::npos;
  });
}

/** MQTT broker schemes accepted by the portal's advanced settings. */
inline bool valid_mqtt_scheme(const std::string &scheme) {
  return scheme.empty() || scheme == "mqtt" || scheme == "mqtts";
}

/** Broker ports (1..65535); an empty string keeps the existing/default value. */
inline bool valid_mqtt_port(const std::string &port) {
  if (port.empty()) {
    return true;
  }
  if (port.size() > 5U) {
    return false;
  }
  int value = 0;
  for (char c : port) {
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
      return false;
    }
    value = value * 10 + (c - '0');
  }
  return value >= 1 && value <= 65535;
}

}  // namespace portal_logic
}  // namespace spectr
