/*
 * Host test for the portable Spectr provisioning logic.
 *
 * Build (inside the ESP-IDF container or any host with g++):
 *   g++ -std=c++17 -Wall -Wextra -Werror -I components/spectr_provisioning \
 *       components/spectr_provisioning/test/host_portal_logic_test.cpp -o /tmp/test && /tmp/test
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "portal_logic.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_failures = 0;

void expect(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
  }
}

void test_ssid_validation() {
  expect(spectr::portal_logic::valid_ssid("Home"), "plain SSID is valid");
  expect(!spectr::portal_logic::valid_ssid(""), "empty SSID is invalid");
  expect(spectr::portal_logic::valid_ssid(std::string(32, 'a')), "32-byte SSID is valid");
  expect(!spectr::portal_logic::valid_ssid(std::string(33, 'a')), "33-byte SSID is invalid");
}

void test_wifi_password_validation() {
  expect(spectr::portal_logic::valid_wifi_password(""), "open network is allowed");
  expect(spectr::portal_logic::valid_wifi_password("12345678"), "8 characters is valid");
  expect(spectr::portal_logic::valid_wifi_password(std::string(63, 'x')), "63 characters is valid");
  expect(!spectr::portal_logic::valid_wifi_password("1234567"), "7 characters is invalid");
  expect(!spectr::portal_logic::valid_wifi_password(std::string(64, 'x')), "64 characters is invalid");
}

void test_ap_ssid() {
  expect(spectr::portal_logic::make_ap_ssid("a1b2c3d4e5f60718") == "Spectr-0718",
         "AP SSID uses the last four characters, upper-cased");
  expect(spectr::portal_logic::make_ap_ssid("ABCD") == "Spectr-ABCD", "short device id is kept");
  expect(spectr::portal_logic::make_ap_ssid("abc") == "Spectr-ABC", "three characters is tolerated");
}

void test_generated_ap_password() {
  expect(spectr::portal_logic::valid_generated_ap_password("K7MQP4XR9T"),
         "valid generated password accepted");
  expect(!spectr::portal_logic::valid_generated_ap_password("K7MQP4XR9"), "short password rejected");
  expect(!spectr::portal_logic::valid_generated_ap_password("K7MQP4XR9O"), "ambiguous character rejected");
  expect(!spectr::portal_logic::valid_generated_ap_password("k7mqp4xr9t"), "lowercase rejected");
}

void test_mqtt_validation() {
  expect(spectr::portal_logic::valid_mqtt_scheme(""), "empty scheme keeps the default");
  expect(spectr::portal_logic::valid_mqtt_scheme("mqtt"), "mqtt scheme valid");
  expect(spectr::portal_logic::valid_mqtt_scheme("mqtts"), "mqtts scheme valid");
  expect(!spectr::portal_logic::valid_mqtt_scheme("ws"), "websocket scheme rejected");
  expect(spectr::portal_logic::valid_mqtt_port(""), "empty port keeps the default");
  expect(spectr::portal_logic::valid_mqtt_port("1883"), "numeric port valid");
  expect(spectr::portal_logic::valid_mqtt_port("65535"), "max port valid");
  expect(!spectr::portal_logic::valid_mqtt_port("65536"), "too many digits rejected");
  expect(!spectr::portal_logic::valid_mqtt_port("18a3"), "non-numeric port rejected");
}

}  // namespace

int main() {
  test_ssid_validation();
  test_wifi_password_validation();
  test_ap_ssid();
  test_generated_ap_password();
  test_mqtt_validation();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d provisioning logic test(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("provisioning logic tests passed\n");
  return EXIT_SUCCESS;
}
