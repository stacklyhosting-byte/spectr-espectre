/*
 * Spectr - SoftAP provisioning portal
 *
 * First-boot setup mode: creates the `Spectr-XXXX` hotspot with a per-device
 * WPA2 password, serves a captive portal for Wi-Fi and (advanced) broker
 * settings, verifies the candidate network before saving, then reboots into
 * the normal firmware.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "spectr_provisioning_portal.h"

#include "portal_logic.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"

#include "runtime/esp_idf/device_config_store.h"
#include "runtime/esp_idf/device_identity.h"

namespace spectr {
namespace {

constexpr const char *kTag = "spectr.portal";
constexpr const char *kApAddress = "192.168.4.1";
constexpr const char *kNvsNamespace = "spectr";
constexpr const char *kNvsApPasswordKey = "ap_password";
constexpr EventBits_t kBitConnected = BIT0;
constexpr EventBits_t kBitVerifyFailed = BIT1;
constexpr uint32_t kVerifyTimeoutMs = 30000U;
constexpr int kVerifyMaxAttempts = 4;
constexpr size_t kMaxBodyBytes = 1024U;

extern "C" const uint8_t portal_html_start[] asm("_binary_portal_html_start");
extern "C" const uint8_t portal_html_end[] asm("_binary_portal_html_end");

struct PortalState {
  std::string device_id;
  std::string ap_ssid;
  std::string ap_password;
  EventGroupHandle_t events{nullptr};
  std::atomic<bool> verifying{false};
  std::atomic<int> verify_attempts{0};
};

PortalState *g_state = nullptr;

std::string generate_ap_password() {
  const std::string alphabet = portal_logic::ap_password_alphabet();
  std::string password;
  password.reserve(10U);
  for (int i = 0; i < 10; ++i) {
    password.push_back(alphabet[esp_random() % alphabet.size()]);
  }
  return password;
}

/** Read the per-device hotspot password from NVS, creating it on first boot. */
std::string load_or_create_ap_password() {
  nvs_handle_t handle = 0;
  std::string password;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    return generate_ap_password();
  }
  size_t length = 0;
  if (nvs_get_str(handle, kNvsApPasswordKey, nullptr, &length) == ESP_OK && length > 1U &&
      length <= 65U) {
    password.resize(length - 1U);
    if (nvs_get_str(handle, kNvsApPasswordKey, password.data(), &length) != ESP_OK) {
      password.clear();
    }
  }
  if (!portal_logic::valid_generated_ap_password(password)) {
    password = generate_ap_password();
    if (nvs_set_str(handle, kNvsApPasswordKey, password.c_str()) == ESP_OK) {
      (void) nvs_commit(handle);
    }
  }
  nvs_close(handle);
  return password;
}

std::string json_escape(const std::string &value) {
  std::string out;
  out.reserve(value.size() + 8U);
  for (char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20U) {
          char buffer[7];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
          out += buffer;
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  return out;
}

void send_json(httpd_req_t *request, const char *status, const std::string &body) {
  httpd_resp_set_status(request, status);
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_send(request, body.c_str(), static_cast<ssize_t>(body.size()));
}

void send_error(httpd_req_t *request, const std::string &message, const char *status = "200 OK") {
  send_json(request, status, "{\"ok\":false,\"error\":\"" + json_escape(message) + "\"}");
}

std::string url_decode(const std::string &value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (c == '+') {
      out.push_back(' ');
    } else if (c == '%' && i + 2U < value.size()) {
      const auto hex = [](char h) -> int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
        return -1;
      };
      const int high = hex(value[i + 1U]);
      const int low = hex(value[i + 2U]);
      if (high >= 0 && low >= 0) {
        out.push_back(static_cast<char>((high << 4) | low));
        i += 2U;
      } else {
        out.push_back(c);
      }
    } else {
      out.push_back(c);
    }
  }
  return out;
}

bool form_value(const std::string &body, const std::string &key, std::string *out) {
  size_t pos = 0;
  while (pos <= body.size()) {
    const size_t end = body.find('&', pos);
    const std::string pair = body.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    const size_t equals = pair.find('=');
    const std::string name = equals == std::string::npos ? pair : pair.substr(0, equals);
    if (name == key) {
      *out = equals == std::string::npos ? std::string() : url_decode(pair.substr(equals + 1U));
      return true;
    }
    if (end == std::string::npos) break;
    pos = end + 1U;
  }
  return false;
}

bool read_body(httpd_req_t *request, std::string *body) {
  const size_t total = request->content_len;
  if (total == 0U || total > kMaxBodyBytes) {
    return false;
  }
  body->resize(total);
  size_t received = 0;
  while (received < total) {
    const int result = httpd_req_recv(request, body->data() + received, total - received);
    if (result <= 0) {
      return false;
    }
    received += static_cast<size_t>(result);
  }
  return true;
}

/** Kconfig defaults with any stored device configuration layered on top. */
espectre::EspectreDeviceConfig effective_device_config() {
  espectre::EspectreDeviceConfig config;
  config.device_label = CONFIG_ESPECTRE_DEVICE_LABEL;
  config.mqtt_scheme = CONFIG_ESPECTRE_MQTT_SCHEME;
  config.mqtt_host = CONFIG_ESPECTRE_MQTT_HOST;
  config.mqtt_port = CONFIG_ESPECTRE_MQTT_PORT;
  config.mqtt_username = CONFIG_ESPECTRE_MQTT_USERNAME;
  config.mqtt_password = CONFIG_ESPECTRE_MQTT_PASSWORD;
  config.topic_prefix = CONFIG_ESPECTRE_TOPIC_PREFIX;
  config.device_id = espectre::derive_runtime_device_id();
  bool has_saved = false;
  if (espectre::load_stored_device_config(&config, &has_saved) != ESP_OK) {
    ESP_LOGW(kTag, "Stored device config unreadable; using defaults");
  }
  return config;
}

// ─── Wi-Fi events ─────────────────────────────────────────────────────────────

void wifi_event_handler(void *, esp_event_base_t base, int32_t id, void *) {
  PortalState *state = g_state;
  if (state == nullptr || state->events == nullptr) {
    return;
  }
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED && state->verifying.load()) {
    if (state->verify_attempts.fetch_add(1) < kVerifyMaxAttempts) {
      (void) esp_wifi_connect();
    } else {
      xEventGroupSetBits(state->events, kBitVerifyFailed);
    }
  }
}

void ip_event_handler(void *, esp_event_base_t base, int32_t id, void *) {
  PortalState *state = g_state;
  if (state != nullptr && state->events != nullptr && base == IP_EVENT &&
      id == IP_EVENT_STA_GOT_IP && state->verifying.load()) {
    xEventGroupSetBits(state->events, kBitConnected);
  }
}

// ─── HTTP handlers ────────────────────────────────────────────────────────────

esp_err_t root_handler(httpd_req_t *request) {
  const size_t size = static_cast<size_t>(portal_html_end - portal_html_start);
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, reinterpret_cast<const char *>(portal_html_start),
                         static_cast<ssize_t>(size));
}

esp_err_t status_handler(httpd_req_t *request) {
  PortalState *state = g_state;
  const espectre::EspectreDeviceConfig config = effective_device_config();
  std::string body = "{";
  body += "\"device_id\":\"" + json_escape(state != nullptr ? state->device_id : "") + "\",";
  body += "\"ap_ssid\":\"" + json_escape(state != nullptr ? state->ap_ssid : "") + "\",";
  body += "\"ap_password\":\"" + json_escape(state != nullptr ? state->ap_password : "") + "\",";
  body += "\"label\":\"" + json_escape(config.device_label) + "\",";
  body += "\"mqtt_scheme\":\"" + json_escape(config.mqtt_scheme) + "\",";
  body += "\"mqtt_host\":\"" + json_escape(config.mqtt_host) + "\",";
  body += "\"mqtt_port\":" + std::to_string(config.mqtt_port) + ",";
  body += "\"mqtt_username\":\"" + json_escape(config.mqtt_username) + "\",";
  body += "\"topic_prefix\":\"" + json_escape(config.topic_prefix) + "\"";
  body += "}";
  send_json(request, "200 OK", body);
  return ESP_OK;
}

esp_err_t scan_handler(httpd_req_t *request) {
  wifi_scan_config_t scan_config = {};
  scan_config.show_hidden = false;
  const esp_err_t result = esp_wifi_scan_start(&scan_config, true);
  if (result != ESP_OK) {
    send_error(request, "Wi-Fi scan failed");
    return ESP_OK;
  }

  uint16_t count = 0;
  (void) esp_wifi_scan_get_ap_num(&count);
  if (count > 32U) {
    count = 32U;
  }
  std::vector<wifi_ap_record_t> records(count);
  if (count > 0U) {
    (void) esp_wifi_scan_get_ap_records(&count, records.data());
  }

  // Keep the strongest access point per SSID.
  std::vector<wifi_ap_record_t> best;
  for (uint16_t i = 0; i < count; ++i) {
    const wifi_ap_record_t &record = records[i];
    const std::string ssid(record.ssid, record.ssid + strnlen(reinterpret_cast<const char *>(record.ssid), sizeof(record.ssid)));
    bool merged = false;
    for (wifi_ap_record_t &existing : best) {
      if (ssid == std::string(existing.ssid, existing.ssid + strnlen(reinterpret_cast<const char *>(existing.ssid), sizeof(existing.ssid)))) {
        if (record.rssi > existing.rssi) existing = record;
        merged = true;
        break;
      }
    }
    if (!merged) best.push_back(record);
  }

  std::string body = "{\"ok\":true,\"networks\":[";
  bool first = true;
  for (const wifi_ap_record_t &record : best) {
    if (record.ssid[0] == 0) {
      continue;
    }
    const std::string ssid(record.ssid,
                           record.ssid + strnlen(reinterpret_cast<const char *>(record.ssid), sizeof(record.ssid)));
    if (!first) body += ",";
    first = false;
    body += "{\"ssid\":\"" + json_escape(ssid) + "\",";
    body += "\"rssi\":" + std::to_string(record.rssi) + ",";
    body += "\"channel\":" + std::to_string(record.primary) + ",";
    body += "\"secure\":";
    body += record.authmode == WIFI_AUTH_OPEN ? "false" : "true";
    body += "}";
  }
  body += "]}";
  send_json(request, "200 OK", body);
  return ESP_OK;
}

bool store_advanced_settings(const std::string &label, const std::string &mqtt_host,
                             const std::string &mqtt_port, const std::string &mqtt_scheme,
                             const std::string &mqtt_username, const std::string &mqtt_password,
                             const std::string &topic_prefix) {
  const bool advanced = !label.empty() || !mqtt_host.empty() || !topic_prefix.empty();
  if (!advanced) {
    return true;
  }
  espectre::EspectreDeviceConfig config = effective_device_config();
  if (!label.empty()) {
    config.device_label = label;
  }
  if (!mqtt_host.empty()) {
    config.mqtt_host = mqtt_host;
    if (!mqtt_port.empty()) {
      config.mqtt_port = static_cast<uint16_t>(std::atoi(mqtt_port.c_str()));
    }
    if (!mqtt_scheme.empty()) {
      config.mqtt_scheme = mqtt_scheme;
    }
    if (!mqtt_username.empty()) {
      config.mqtt_username = mqtt_username;
    }
    if (!mqtt_password.empty()) {
      config.mqtt_password = mqtt_password;
    }
    if (!topic_prefix.empty()) {
      config.topic_prefix = topic_prefix;
    }
  }
  return espectre::save_stored_device_config(config) == ESP_OK;
}

esp_err_t save_handler(httpd_req_t *request) {
  PortalState *state = g_state;
  if (state == nullptr) {
    send_error(request, "Setup is not running");
    return ESP_OK;
  }

  std::string body;
  if (!read_body(request, &body)) {
    send_error(request, "Malformed request", "400 Bad Request");
    return ESP_OK;
  }

  std::string ssid;
  std::string password;
  std::string label;
  std::string mqtt_host;
  std::string mqtt_port;
  std::string mqtt_scheme;
  std::string mqtt_username;
  std::string mqtt_password;
  std::string topic_prefix;
  (void) form_value(body, "ssid", &ssid);
  (void) form_value(body, "password", &password);
  (void) form_value(body, "label", &label);
  (void) form_value(body, "mqtt_host", &mqtt_host);
  (void) form_value(body, "mqtt_port", &mqtt_port);
  (void) form_value(body, "mqtt_scheme", &mqtt_scheme);
  (void) form_value(body, "mqtt_username", &mqtt_username);
  (void) form_value(body, "mqtt_password", &mqtt_password);
  (void) form_value(body, "topic_prefix", &topic_prefix);

  if (!portal_logic::valid_ssid(ssid)) {
    send_error(request, "Choose a Wi-Fi network first");
    return ESP_OK;
  }
  if (!portal_logic::valid_wifi_password(password)) {
    send_error(request, "Wi-Fi passwords are 8 to 63 characters");
    return ESP_OK;
  }
  if (!portal_logic::valid_mqtt_scheme(mqtt_scheme) || !portal_logic::valid_mqtt_port(mqtt_port)) {
    send_error(request, "Check the advanced broker settings");
    return ESP_OK;
  }

  wifi_config_t station_config = {};
  std::memcpy(station_config.sta.ssid, ssid.c_str(), std::min(ssid.size(), sizeof(station_config.sta.ssid)));
  std::memcpy(station_config.sta.password, password.c_str(),
              std::min(password.size(), sizeof(station_config.sta.password)));
  station_config.sta.threshold.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

  esp_wifi_disconnect();
  state->verify_attempts.store(0);
  xEventGroupClearBits(state->events, kBitConnected | kBitVerifyFailed);
  state->verifying.store(true);
  if (esp_wifi_set_config(WIFI_IF_STA, &station_config) != ESP_OK ||
      esp_wifi_connect() != ESP_OK) {
    state->verifying.store(false);
    send_error(request, "Could not start the connection");
    return ESP_OK;
  }

  const EventBits_t bits = xEventGroupWaitBits(state->events, kBitConnected | kBitVerifyFailed,
                                               pdTRUE, pdFALSE, pdMS_TO_TICKS(kVerifyTimeoutMs));
  state->verifying.store(false);
  if ((bits & kBitConnected) == 0) {
    esp_wifi_disconnect();
    send_error(request, "Could not connect. Check the password and try again.");
    return ESP_OK;
  }

  espectre::StoredWifiConfig stored;
  stored.ssid = ssid;
  stored.password = password;
  stored.has_saved_config = true;
  const esp_err_t wifi_result = espectre::save_stored_wifi_config(stored);
  if (wifi_result != ESP_OK) {
    send_error(request, "Could not save the Wi-Fi settings");
    return ESP_OK;
  }
  if (!store_advanced_settings(label, mqtt_host, mqtt_port, mqtt_scheme, mqtt_username,
                               mqtt_password, topic_prefix)) {
    ESP_LOGW(kTag, "Advanced settings could not be saved");
  }

  ESP_LOGI(kTag, "Provisioned for SSID '%s'; restarting into sensing mode", ssid.c_str());
  send_json(request, "200 OK", "{\"ok\":true}");
  vTaskDelay(pdMS_TO_TICKS(1200));
  esp_restart();
  return ESP_OK;
}

esp_err_t captive_redirect_handler(httpd_req_t *request) {
  httpd_resp_set_status(request, "302 Found");
  httpd_resp_set_hdr(request, "Location", "http://192.168.4.1/");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, nullptr, 0);
}

bool start_http_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.max_uri_handlers = 8;
  config.lru_purge_enable = true;

  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &config) != ESP_OK) {
    return false;
  }

  httpd_uri_t root_uri = {};
  root_uri.uri = "/";
  root_uri.method = HTTP_GET;
  root_uri.handler = root_handler;

  httpd_uri_t status_uri = {};
  status_uri.uri = "/status";
  status_uri.method = HTTP_GET;
  status_uri.handler = status_handler;

  httpd_uri_t scan_uri = {};
  scan_uri.uri = "/scan";
  scan_uri.method = HTTP_GET;
  scan_uri.handler = scan_handler;

  httpd_uri_t save_uri = {};
  save_uri.uri = "/save";
  save_uri.method = HTTP_POST;
  save_uri.handler = save_handler;

  httpd_uri_t redirect_uri = {};
  redirect_uri.uri = "/*";
  redirect_uri.method = HTTP_GET;
  redirect_uri.handler = captive_redirect_handler;

  return httpd_register_uri_handler(server, &root_uri) == ESP_OK &&
         httpd_register_uri_handler(server, &status_uri) == ESP_OK &&
         httpd_register_uri_handler(server, &scan_uri) == ESP_OK &&
         httpd_register_uri_handler(server, &save_uri) == ESP_OK &&
         httpd_register_uri_handler(server, &redirect_uri) == ESP_OK;
}

/** Minimal captive-portal DNS responder: answer every A query with 192.168.4.1. */
void dns_task(void *) {
  const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGW(kTag, "Captive DNS socket unavailable");
    vTaskDelete(nullptr);
    return;
  }
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(53);
  if (bind(sock, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
    close(sock);
    ESP_LOGW(kTag, "Captive DNS bind failed");
    vTaskDelete(nullptr);
    return;
  }

  uint8_t query[512];
  uint8_t response[512];
  for (;;) {
    sockaddr_in from = {};
    socklen_t from_length = sizeof(from);
    const int length = recvfrom(sock, query, sizeof(query), 0, reinterpret_cast<sockaddr *>(&from), &from_length);
    if (length < 12 || length > static_cast<int>(sizeof(query)) - 16) {
      continue;
    }
    // Walk past the (single) question name.
    size_t question_end = 12;
    while (question_end < static_cast<size_t>(length) && query[question_end] != 0U) {
      question_end += static_cast<size_t>(query[question_end]) + 1U;
    }
    question_end += 5U;  // null label + QTYPE + QCLASS
    if (question_end > static_cast<size_t>(length)) {
      continue;
    }

    std::memcpy(response, query, question_end);
    response[2] = 0x81U;  // QR=1, RD=1
    response[3] = 0x80U;  // RA=1
    response[6] = 0x00U;
    response[7] = 0x01U;  // ANCOUNT = 1
    size_t out = question_end;
    response[out++] = 0xC0U;  // name pointer to the question
    response[out++] = 0x0CU;
    response[out++] = 0x00U; response[out++] = 0x01U;  // TYPE A
    response[out++] = 0x00U; response[out++] = 0x01U;  // CLASS IN
    response[out++] = 0x00U; response[out++] = 0x00U; response[out++] = 0x00U; response[out++] = 60U;
    response[out++] = 0x00U; response[out++] = 0x04U;  // RDLENGTH
    response[out++] = 192U; response[out++] = 168U; response[out++] = 4U; response[out++] = 1U;
    (void) sendto(sock, response, out, 0, reinterpret_cast<sockaddr *>(&from), from_length);
  }
}

}  // namespace

bool provisioning_required() {
  espectre::StoredWifiConfig stored;
  const esp_err_t result = espectre::load_stored_wifi_config(&stored);
  const bool has_build_ssid = CONFIG_ESPECTRE_WIFI_SSID[0] != '\0';
  if (result != ESP_OK) {
    return !has_build_ssid;
  }
  return !stored.has_saved_config && !has_build_ssid;
}

void run_provisioning_portal() {
  PortalState state;
  state.device_id = espectre::derive_runtime_device_id_string();
  state.ap_ssid = portal_logic::make_ap_ssid(state.device_id);
  state.ap_password = load_or_create_ap_password();
  state.events = xEventGroupCreate();
  g_state = &state;

  ESP_LOGI(kTag, "Setup mode: hotspot '%s' password '%s' at http://%s/",
           state.ap_ssid.c_str(), state.ap_password.c_str(), kApAddress);
  ESP_LOGI(kTag, "Device ID %s - claim it in the Spectr app after setup", state.device_id.c_str());

  esp_netif_init();
  esp_event_loop_create_default();
  (void) esp_netif_create_default_wifi_ap();
  // The station interface must exist so the DHCP client runs while we verify
  // the candidate network; without it the STA associates but never gets an IP.
  (void) esp_netif_create_default_wifi_sta();

  wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
  if (esp_wifi_init(&init_config) != ESP_OK) {
    ESP_LOGE(kTag, "Wi-Fi init failed");
    return;
  }
  (void) esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr);
  (void) esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, nullptr, nullptr);

  wifi_config_t ap_config = {};
  std::memcpy(ap_config.ap.ssid, state.ap_ssid.c_str(),
              std::min(state.ap_ssid.size(), sizeof(ap_config.ap.ssid)));
  ap_config.ap.ssid_len = static_cast<uint8_t>(state.ap_ssid.size());
  std::memcpy(ap_config.ap.password, state.ap_password.c_str(),
              std::min(state.ap_password.size(), sizeof(ap_config.ap.password)));
  ap_config.ap.channel = 1;
  ap_config.ap.max_connection = 4;
  ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
  ap_config.ap.pmf_cfg.required = false;

  if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK ||
      esp_wifi_set_config(WIFI_IF_AP, &ap_config) != ESP_OK ||
      esp_wifi_start() != ESP_OK) {
    ESP_LOGE(kTag, "Could not start the setup hotspot");
    return;
  }

  if (!start_http_server()) {
    ESP_LOGE(kTag, "Could not start the setup portal");
    return;
  }
  xTaskCreate(dns_task, "spectr_dns", 4096, nullptr, 4, nullptr);

  ESP_LOGI(kTag, "Portal ready; waiting for Wi-Fi credentials");
  vTaskDelay(portMAX_DELAY);
}

}  // namespace spectr
