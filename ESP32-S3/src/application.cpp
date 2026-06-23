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
  static int64_t s_doorOpenStateDurationMs = 0;
  static portMUX_TYPE s_stateMux = portMUX_INITIALIZER_UNLOCKED;
  static int64_t s_lastHeartbeatMs = 0;
  static const int64_t kHeartbeatIntervalMs = 10000;
  static int64_t s_lastPushNotificationMs = 0;
  static int64_t s_doorOpenSinceMs = 0;
  static int64_t s_lastDoorPacketMs = 0;
  static const int64_t kDoorStateTimeoutMs = 45000;

  static void on_esp_now_receive(const uint8_t *fromMac, const uint8_t *data, size_t len) {
    if (fromMac == nullptr || data == nullptr || len == 0) {
      return;
    }

    if (len == 9 && memcmp(data, "DOOR:OPEN", 9) == 0) {
      const int64_t nowMs = esp_timer_get_time() / 1000;
      taskENTER_CRITICAL(&s_stateMux);
      bool wasAlreadyOpen = s_remoteDoorOpen;
      s_remoteDoorOpen = true;
      s_remoteDoorStateKnown = true;
      // Always update lastDoorPacketMs to keep timeout window fresh
      s_lastDoorPacketMs = nowMs;
      // Only reset open-duration baseline and push timer on state transition.
      if (!wasAlreadyOpen) {
        s_doorOpenSinceMs = nowMs;
        s_lastPushNotificationMs = nowMs;
      }
      taskEXIT_CRITICAL(&s_stateMux);
      ESP_LOGI(TAG, "ESP-NOW RX: DOOR OPEN");
      return;
    }

    if (len == 11 && memcmp(data, "DOOR:CLOSED", 11) == 0) {
      const int64_t nowMs = esp_timer_get_time() / 1000;
      taskENTER_CRITICAL(&s_stateMux);
      s_remoteDoorOpen = false;
      s_remoteDoorStateKnown = true;
      s_lastDoorPacketMs = nowMs;
      s_doorOpenSinceMs = 0;
      taskEXIT_CRITICAL(&s_stateMux);
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
      const int64_t currentMillis = esp_timer_get_time() / 1000;
      bool remoteDoorStateKnown = false;
      bool remoteDoorOpen = false;
      int64_t lastDoorPacketMs = 0;
      int64_t doorOpenSinceMs = 0;
      int64_t lastPushNotificationMs = 0;

      taskENTER_CRITICAL(&s_stateMux);
      remoteDoorStateKnown = s_remoteDoorStateKnown;
      remoteDoorOpen = s_remoteDoorOpen;
      lastDoorPacketMs = s_lastDoorPacketMs;
      doorOpenSinceMs = s_doorOpenSinceMs;
      lastPushNotificationMs = s_lastPushNotificationMs;
      taskEXIT_CRITICAL(&s_stateMux);

      if (remoteDoorStateKnown && (currentMillis - lastDoorPacketMs) >= kDoorStateTimeoutMs) {
        taskENTER_CRITICAL(&s_stateMux);
        s_remoteDoorStateKnown = false;
        s_remoteDoorOpen = false;
        s_doorOpenSinceMs = 0;
        taskEXIT_CRITICAL(&s_stateMux);
        ESP_LOGW(TAG, "Door state stale (timeout)");
        remoteDoorStateKnown = false;
        remoteDoorOpen = false;
      }

      if ((currentMillis - s_lastHeartbeatMs) >= kHeartbeatIntervalMs) {
        s_lastHeartbeatMs = currentMillis;
        ESP_LOGI(TAG, "HEARTBEAT: ESP-NOW door=%s", remoteDoorStateKnown ? (remoteDoorOpen ? "OPEN" : "CLOSED") : "UNKNOWN");
      }

      // Push notification is sent once the door is open as long as pushNotificationIntervalMs
      if (remoteDoorStateKnown && remoteDoorOpen && (currentMillis - lastPushNotificationMs) >= s_config.pushNotificationIntervalMs) {
        taskENTER_CRITICAL(&s_stateMux);
        s_lastPushNotificationMs = currentMillis;
        taskEXIT_CRITICAL(&s_stateMux);

        s_doorOpenStateDurationMs = currentMillis - doorOpenSinceMs;
        const int64_t doorOpenMinutes = s_doorOpenStateDurationMs / 60000;
        const int64_t doorOpenSeconds = (s_doorOpenStateDurationMs % 60000) / 1000;
        ESP_LOGI(TAG, "PUSH NOTIFICATION: ESP-NOW door is open for %lldm %llds. Close the door!", doorOpenMinutes, doorOpenSeconds);
      }

      vTaskDelay(pdMS_TO_TICKS(s_config.loopDelayMs));
    }
  }

}  // namespace app
