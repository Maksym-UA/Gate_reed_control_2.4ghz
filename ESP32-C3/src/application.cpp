#include "application.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "espnow_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led.h"
#include "reed.h"

namespace app {

static const char *TAG = "gate_reed";
static void on_esp_now_receive(const uint8_t *fromMac, const uint8_t *data, size_t len) {
  (void)fromMac;
  ESP_LOGI(TAG, "ESP-NOW RX: %.*s", static_cast<int>(len), reinterpret_cast<const char *>(data));
}

static Config s_config = {};
static bool s_initialized = false;
static bool s_lastDoorOpen = true;
static int64_t s_lastBlinkTimeMs = 0;
static int64_t s_lastStatusLogMs = 0;
static const int64_t kStatusLogIntervalMs = 10000;

static void print_door_state(bool open) {
  if (open) {
    ESP_LOGI(TAG, "DOOR: OPEN");
  } else {
    ESP_LOGI(TAG, "DOOR: CLOSED");
  }
}

static void publish_door_state(bool open) {
  const char *message = open ? "DOOR:OPEN" : "DOOR:CLOSED";
  ESP_LOGI(TAG, "ESP-NOW TX: %s", message);
  esp_err_t sendRet = espnow::send_broadcast(
      reinterpret_cast<const uint8_t *>(message),
      strlen(message));
  if (sendRet != ESP_OK) {
    ESP_LOGW(TAG, "ESP-NOW send failed: %s", esp_err_to_name(sendRet));
  }
}

void init(const Config &config) {
  s_config = config;
  s_initialized = true;

  // Keep global WARN level for smaller firmware, but show INFO for app modules.
  esp_log_level_set(TAG, ESP_LOG_INFO);
  esp_log_level_set("espnow", ESP_LOG_INFO);

  ESP_ERROR_CHECK(espnow::init(1));
  espnow::set_receive_callback(on_esp_now_receive);

  reed::init(s_config.sensorPin, true);
  led::init(s_config.ledPin);

  vTaskDelay(pdMS_TO_TICKS(500));

  const bool currentDoorOpen = reed::is_door_open();
  s_lastDoorOpen = currentDoorOpen;

  ESP_LOGI(TAG, "Boot");
  print_door_state(currentDoorOpen);
  publish_door_state(currentDoorOpen);
  s_lastStatusLogMs = esp_timer_get_time() / 1000;

  if (!currentDoorOpen) {
    led::set_led(false);
  }
}

void run() {
  if (!s_initialized) {
    ESP_LOGE(TAG, "app::init must be called before app::run");
    return;
  }

  while (true) {
    const int64_t currentMillis = esp_timer_get_time() / 1000;
    bool currentDoorOpen = reed::is_door_open();

    if (currentDoorOpen != s_lastDoorOpen) {
      vTaskDelay(pdMS_TO_TICKS(s_config.debounceMs));
      currentDoorOpen = reed::is_door_open();

      if (currentDoorOpen != s_lastDoorOpen) {
        s_lastDoorOpen = currentDoorOpen;
        print_door_state(currentDoorOpen);
        publish_door_state(currentDoorOpen);

        if (!currentDoorOpen) {
          led::set_led(false);
        }
      }
    }

    if (currentDoorOpen) {
      if ((currentMillis - s_lastBlinkTimeMs) >= s_config.blinkIntervalMs) {
        s_lastBlinkTimeMs = currentMillis;
        led::toggle();
      }
    }

    if ((currentMillis - s_lastStatusLogMs) >= kStatusLogIntervalMs) {
      s_lastStatusLogMs = currentMillis;
      ESP_LOGI(TAG, "HEARTBEAT: DOOR=%s", currentDoorOpen ? "OPEN" : "CLOSED");
    }

    vTaskDelay(pdMS_TO_TICKS(s_config.loopDelayMs));
  }
}

}  // namespace app
