#include "application.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "espnow_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


namespace app {

static const char *TAG = "gate_reed";
static Config s_config = {};
static bool s_initialized = false;
static bool s_remoteDoorStateKnown = false;
static bool s_remoteDoorOpen = false;
static bool s_remoteDoorStateDirty = false;
static int64_t s_lastHeartbeatMs = 0;
static const int64_t kHeartbeatIntervalMs = 10000;
static int64_t s_lastDoorPacketMs = 0;
static const int64_t kDoorStateTimeoutMs = 90000;

static void show_remote_door_state_if_needed() {
  if (!s_remoteDoorStateKnown || !s_remoteDoorStateDirty) {
    return;
  }

  s_remoteDoorStateDirty = false;
}

static void on_esp_now_receive(const uint8_t *fromMac, const uint8_t *data, size_t len) {
  if (fromMac == nullptr || data == nullptr || len == 0) {
    return;
  }

  if (len == 9 && memcmp(data, "DOOR:OPEN", 9) == 0) {
    s_remoteDoorOpen = true;
    s_remoteDoorStateKnown = true;
    s_remoteDoorStateDirty = true;
    s_lastDoorPacketMs = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "ESP-NOW RX: DOOR OPEN");
    return;
  }

  if (len == 11 && memcmp(data, "DOOR:CLOSED", 11) == 0) {
    s_remoteDoorOpen = false;
    s_remoteDoorStateKnown = true;
    s_remoteDoorStateDirty = true;
    s_lastDoorPacketMs = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "ESP-NOW RX: DOOR CLOSED");
    return;
  }

  ESP_LOGI(TAG, "ESP-NOW RX: %.*s", static_cast<int>(len), reinterpret_cast<const char *>(data));
}

void init(const Config &config) {
  s_config = config;
  s_initialized = true;

  esp_log_level_set(TAG, ESP_LOG_INFO);
  esp_log_level_set("espnow", ESP_LOG_WARN);

  ESP_ERROR_CHECK(espnow::init(1));
  espnow::set_receive_callback(on_esp_now_receive);



  s_lastHeartbeatMs = esp_timer_get_time() / 1000;
  ESP_LOGI(TAG, "S3 receiver ready");
}

void run() {
  if (!s_initialized) {
    ESP_LOGE(TAG, "app::init must be called before app::run");
    return;
  }

  while (true) {
    show_remote_door_state_if_needed();

    const int64_t currentMillis = esp_timer_get_time() / 1000;
    if (s_remoteDoorStateKnown && (currentMillis - s_lastDoorPacketMs) >= kDoorStateTimeoutMs) {
      s_remoteDoorStateKnown = false;
      ESP_LOGW(TAG, "Door state stale (timeout)");
    }

    if ((currentMillis - s_lastHeartbeatMs) >= kHeartbeatIntervalMs) {
      s_lastHeartbeatMs = currentMillis;
      ESP_LOGI(TAG, "HEARTBEAT: ESP-NOW door=%s", s_remoteDoorStateKnown ? (s_remoteDoorOpen ? "OPEN" : "CLOSED") : "UNKNOWN");
    }

    vTaskDelay(pdMS_TO_TICKS(s_config.loopDelayMs));
  }
}

}  // namespace app
