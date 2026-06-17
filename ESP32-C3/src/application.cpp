#include "application.h"

#include <string.h>

#include "esp_log.h"
#include "esp_sleep.h"
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
  for (int attempt = 0; attempt < 3; ++attempt) {
    esp_err_t sendRet = espnow::send_broadcast(
        reinterpret_cast<const uint8_t *>(message),
        strlen(message));
    if (sendRet == ESP_OK) {
      return;
    }
    ESP_LOGW(TAG, "ESP-NOW send failed (attempt %d): %s", attempt + 1, esp_err_to_name(sendRet));
    vTaskDelay(pdMS_TO_TICKS(40));
  }
}

static void enter_deep_sleep_for_next_change(bool currentDoorOpen) {
  const int wakeLevel = currentDoorOpen ? 0 : 1;
  ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(s_config.sensorPin, wakeLevel));
  ESP_LOGI(TAG, "Entering deep sleep; wake when GPIO%d == %d", static_cast<int>(s_config.sensorPin), wakeLevel);
  esp_deep_sleep_start();
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

  vTaskDelay(pdMS_TO_TICKS(s_config.debounceMs));
  const bool currentDoorOpen = reed::is_door_open();

  ESP_LOGI(TAG, "Boot");
  print_door_state(currentDoorOpen);
  publish_door_state(currentDoorOpen);

  if (!currentDoorOpen) {
    led::set_led(false);
  }
}

void run() {
  if (!s_initialized) {
    ESP_LOGE(TAG, "app::init must be called before app::run");
    return;
  }

  const bool currentDoorOpen = reed::is_door_open();
  enter_deep_sleep_for_next_change(currentDoorOpen);
}

}  // namespace app
