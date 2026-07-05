#include "application.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "espnow_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "telegram.h"

namespace app {

  static const char *TAG = "gate_reed";
  static Config s_config = {};
  static bool s_initialized = false;
  static bool s_remoteDoorStateKnown = false;
  static bool s_remoteDoorOpen = false;
  static int64_t s_doorOpenStateDurationMs = 0;
  static portMUX_TYPE s_stateMux = portMUX_INITIALIZER_UNLOCKED;
  static int64_t s_lastHeartbeatMs = 0;
  static const int64_t kHeartbeatIntervalClosedMs = 5 * 60 * 1000;
  static const int64_t kHeartbeatIntervalOpenMs = 30000;
  static int64_t s_lastPushNotificationMs = 0;
  static int64_t s_doorOpenSinceMs = 0;
  static int64_t s_lastDoorPacketMs = 0;
  static const int64_t kDoorStateTimeoutOpenMs   = 45000;
  static const int64_t kDoorStateTimeoutClosedMs = 6 * 60 * 1000;
  static volatile bool s_notifyDoorOpened = false;
  static volatile bool s_notifyDoorClosed = false;

  static float s_lastBatteryVoltage = 0.0f;
  static bool s_hasBatteryVoltage = false;

  // Telegram task — message queue + task handle
  static const size_t kTgMsgMaxLen = 160;
  static QueueHandle_t s_tgQueue = nullptr;

  // Enqueue a Telegram message from any task. Non-blocking; drops if queue full.
  static void tg_enqueue(const char *text) {
    if (s_tgQueue == nullptr) return;
    char buf[kTgMsgMaxLen] = {};
    strncpy(buf, text, kTgMsgMaxLen - 1);
    if (xQueueSend(s_tgQueue, buf, 0) != pdTRUE) {
      ESP_LOGW(TAG, "Telegram queue full, message dropped");
    }
  }

  // Dedicated Telegram task: drains the outbound queue, then long-polls for commands.
  // All HTTPS calls happen here, so the main task never blocks on network I/O.
  static void tg_task(void *) {
    char msg[kTgMsgMaxLen];
    while (true) {
      // Send all queued notifications before starting the next long-poll.
      while (xQueueReceive(s_tgQueue, msg, 0) == pdTRUE) {
        telegram_send_message(msg);
      }

      // Long-poll: blocks up to 25 s waiting for a Telegram update.
      telegram_command_result_t cmd = {};
      if (telegram_poll_updates(&cmd) == ESP_OK && cmd.has_request && cmd.wants_voltage) {
        if (cmd.pending_callback_id[0] != '\0') {
          telegram_ack_callback(cmd.pending_callback_id, cmd.is_admin);
        }

        float voltage = 0.0f;
        bool hasVoltage = false;
        taskENTER_CRITICAL(&s_stateMux);
        voltage    = s_lastBatteryVoltage;
        hasVoltage = s_hasBatteryVoltage;
        taskEXIT_CRITICAL(&s_stateMux);

        if (!cmd.is_admin) {
          telegram_send_unauthorized_reply();
        } else if (!hasVoltage) {
          telegram_send_message("\xF0\x9F\x94\x8B No saved voltage yet");
        } else {
          telegram_send_voltage_reply(voltage);
        }
      }

      // Brief yield so the scheduler can service the main task between poll cycles.
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }

  static void on_esp_now_receive(const uint8_t *fromMac, const uint8_t *data, size_t len) {    if (fromMac == nullptr || data == nullptr || len == 0) {
      return;
    }

    if (len == 9 && memcmp(data, "DOOR:OPEN", 9) == 0) {
      const int64_t nowMs = esp_timer_get_time() / 1000;
      taskENTER_CRITICAL(&s_stateMux);
      bool wasAlreadyOpen = s_remoteDoorOpen;
      s_remoteDoorOpen = true;
      s_remoteDoorStateKnown = true;
      s_lastDoorPacketMs = nowMs;
      if (!wasAlreadyOpen) {
        s_doorOpenSinceMs = nowMs;
        s_lastPushNotificationMs = nowMs;
        s_notifyDoorOpened = true;
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
        s_notifyDoorClosed = true;
      }
      taskEXIT_CRITICAL(&s_stateMux);
      ESP_LOGI(TAG, "ESP-NOW RX: DOOR CLOSED");
      return;
    }

    if (len > 5 && memcmp(data, "BATT:", 5) == 0) {
      char batt_buf[32] = {0};
      size_t copy_len = len < sizeof(batt_buf) - 1 ? len : sizeof(batt_buf) - 1;
      memcpy(batt_buf, data, copy_len);
      batt_buf[copy_len] = '\0';

      float voltage = 0.0f;
      if (sscanf(batt_buf, "BATT:%fV", &voltage) == 1) {
        taskENTER_CRITICAL(&s_stateMux);
        s_lastBatteryVoltage = voltage;
        s_hasBatteryVoltage = true;
        taskEXIT_CRITICAL(&s_stateMux);
        ESP_LOGI(TAG, "ESP-NOW RX: battery %.2f V", voltage);
        return;
      }
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

    if (telegram_init() != ESP_OK) {
      ESP_LOGW(TAG, "Telegram init failed — notifications will be skipped");
    } else {
      telegram_send_message("🤖 Gate monitor online");
      telegram_send_admin_menu();
    }

    // Create Telegram queue and start the dedicated poll/send task.
    // All HTTPS calls are serialised inside tg_task — main loop never blocks on network.
    s_tgQueue = xQueueCreate(8, kTgMsgMaxLen);
    xTaskCreate(tg_task, "tg_task", 10240, nullptr, 2, nullptr);

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

      if (s_notifyDoorOpened) {
        s_notifyDoorOpened = false;
        tg_enqueue("\xF0\x9F\x9A\xAA Двері ВІДЧИНЕНІ");
      }
      if (s_notifyDoorClosed) {
        s_notifyDoorClosed = false;
        tg_enqueue("\xE2\x9C\x85 Двері ЗАЧИНЕНІ");
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
        ESP_LOGI(TAG, "HEARTBEAT: ESP-NOW door=%s",
                 remoteDoorStateKnown ? (remoteDoorOpen ? "OPEN" : "CLOSED") : "UNKNOWN");
      }

      if (remoteDoorStateKnown && remoteDoorOpen &&
          (currentMillis - lastPushNotificationMs) >= s_config.pushNotificationIntervalMs) {
        taskENTER_CRITICAL(&s_stateMux);
        s_lastPushNotificationMs = currentMillis;
        taskEXIT_CRITICAL(&s_stateMux);

        s_doorOpenStateDurationMs = currentMillis - doorOpenSinceMs;
        const int64_t doorOpenMinutes = s_doorOpenStateDurationMs / 60000;
        const int64_t doorOpenSeconds = (s_doorOpenStateDurationMs % 60000) / 1000;
        char msg[kTgMsgMaxLen];
        snprintf(msg, sizeof(msg),
                 "\xE2\x9A\xA0 Двері все ще ВІДЧИНЕНІ: %lldхв %lldс. Закрийте двері!",
                 doorOpenMinutes, doorOpenSeconds);
        ESP_LOGI(TAG, "%s", msg);
        tg_enqueue(msg);
      }

      vTaskDelay(pdMS_TO_TICKS(s_config.loopDelayMs));
    }
  }

}  // namespace app