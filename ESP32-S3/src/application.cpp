#include "application.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "espnow_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "telegram.h"


namespace app {

  static const char *TAG = "gate_reed";
  static Config s_config = {};
  static bool s_initialized = false;
  static bool s_remoteDoorStateKnown = false;
  static bool s_remoteDoorOpen = false;
  static int64_t s_doorOpenStateDurationMs = 0;
  //portMUX_TYPE spinlock is a low-level, non-blocking lock. When a core tries to acquire
  // a spinlock that is already held, it busy-waits — it loops continuously checking the lock
  // until it becomes available, without ever yielding to the scheduler.
  static portMUX_TYPE s_stateMux = portMUX_INITIALIZER_UNLOCKED; // Protects access to shared state variables
  static int64_t s_lastHeartbeatMs = 0;
  static const int64_t kHeartbeatIntervalClosedMs = 5 * 60 * 1000;
  static const int64_t kHeartbeatIntervalOpenMs = 30000;
  static int64_t s_lastPushNotificationMs = 0;
  static int64_t s_doorOpenSinceMs = 0;
  static int64_t s_lastDoorPacketMs = 0;
  static const int64_t kDoorStateTimeoutOpenMs   = 45000;         // fast C3 failure detection when open
  static const int64_t kDoorStateTimeoutClosedMs = 6 * 60 * 1000; // > C3 5-min keepalive when closed
  // Flags set inside the ESP-NOW callback and consumed in the run() loop.
  // The run() loop is the only place where blocking Telegram HTTP calls are made.
  static volatile bool s_notifyDoorOpened = false;
  static volatile bool s_notifyDoorClosed = false;

  static void on_esp_now_receive(const uint8_t *fromMac, const uint8_t *data, size_t len) {
    if (fromMac == nullptr || data == nullptr || len == 0) {
      return;
    }

    if (len == 9 && memcmp(data, "DOOR:OPEN", 9) == 0) {
      const int64_t nowMs = esp_timer_get_time() / 1000;
      taskENTER_CRITICAL(&s_stateMux);// Critical section to protect shared state
      bool wasAlreadyOpen = s_remoteDoorOpen;
      s_remoteDoorOpen = true;
      s_remoteDoorStateKnown = true;
      // Always update lastDoorPacketMs to keep timeout window fresh
      s_lastDoorPacketMs = nowMs;
      // Only reset open-duration baseline and push timer on state transition.
      if (!wasAlreadyOpen) {
        s_doorOpenSinceMs = nowMs;
        s_lastPushNotificationMs = nowMs;
        s_notifyDoorOpened = true; // trigger Telegram notification from run() loop
      }
      taskEXIT_CRITICAL(&s_stateMux);
      ESP_LOGI(TAG, "ESP-NOW RX: DOOR OPEN");
      return;
    }

    if (len == 11 && memcmp(data, "DOOR:CLOSED", 11) == 0) {
      const int64_t nowMs = esp_timer_get_time() / 1000;
      taskENTER_CRITICAL(&s_stateMux);
      bool wasOpen = s_remoteDoorOpen;
      s_remoteDoorOpen = false;
      s_remoteDoorStateKnown = true;
      s_lastDoorPacketMs = nowMs;
      s_doorOpenSinceMs = 0;
      if (wasOpen) {
        s_notifyDoorClosed = true; // trigger Telegram notification from run() loop
      }
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

    // Connect to WiFi AP for internet access (needed by Telegram).
    // NOTE: ESP-NOW will use the AP's channel after connection — ensure the C3
    // sender is configured to broadcast on the same channel.
    if (telegram_init() != ESP_OK) {
      ESP_LOGW(TAG, "Telegram init failed — notifications will be skipped");
    }

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

      // Send state-change Telegram notifications (set in the ESP-NOW callback).
      if (s_notifyDoorOpened) {
        s_notifyDoorOpened = false;
        telegram_send_message("\xF0\x9F\x9A\xAA Двері ВІДЧИНЕНІ");
      }
      if (s_notifyDoorClosed) {
        s_notifyDoorClosed = false;
        telegram_send_message("\xE2\x9C\x85 Двері ЗАЧИНЕНІ");
      }

      const int64_t doorStateTimeoutMs = remoteDoorOpen ? kDoorStateTimeoutOpenMs : kDoorStateTimeoutClosedMs;
      if (remoteDoorStateKnown && (currentMillis - lastDoorPacketMs) >= doorStateTimeoutMs) {
        taskENTER_CRITICAL(&s_stateMux);
        s_remoteDoorStateKnown = false;
        s_remoteDoorOpen = false;
        s_doorOpenSinceMs = 0;
        taskEXIT_CRITICAL(&s_stateMux);
        ESP_LOGW(TAG, "Door state stale (timeout)");
        remoteDoorStateKnown = false;
        remoteDoorOpen = false;
      }

      const int64_t heartbeatIntervalMs = remoteDoorOpen ? kHeartbeatIntervalOpenMs : kHeartbeatIntervalClosedMs;
      if ((currentMillis - s_lastHeartbeatMs) >= heartbeatIntervalMs) {
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
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "\xE2\x9A\xA0 Двері все ще ВІДЧИНЕНІ: %lldхв %lldс. Закрийте двері!",
                 doorOpenMinutes, doorOpenSeconds);
        ESP_LOGI(TAG, "%s", msg);
        telegram_send_message(msg);
      }

      vTaskDelay(pdMS_TO_TICKS(s_config.loopDelayMs));
    }
  }

}  // namespace app
