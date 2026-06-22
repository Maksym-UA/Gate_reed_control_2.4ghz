#include "application.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "espnow_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led.h"
#include "reed.h"

namespace app {

static const char *TAG = "gate_reed";
static const int kBootUsbGraceMs = 15000;
static const int64_t kSafetyWakeupIntervalMs = 3000;
static const int kPostWakeActiveMs = 2000;

static const char *wakeup_cause_to_string(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_UNDEFINED:
      return "UNDEFINED";
    case ESP_SLEEP_WAKEUP_EXT0:
      return "EXT0";
    case ESP_SLEEP_WAKEUP_EXT1:
      return "EXT1";
    case ESP_SLEEP_WAKEUP_TIMER:
      return "TIMER";
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
      return "TOUCHPAD";
    case ESP_SLEEP_WAKEUP_ULP:
      return "ULP";
    case ESP_SLEEP_WAKEUP_GPIO:
      return "GPIO";
    case ESP_SLEEP_WAKEUP_UART:
      return "UART";
    default:
      return "OTHER";
  }
}

static Config s_config = {};
static bool s_initialized = false;
static esp_sleep_wakeup_cause_t s_wakeCause = ESP_SLEEP_WAKEUP_UNDEFINED;
RTC_DATA_ATTR static bool s_lastPublishedDoorOpen = false;
RTC_DATA_ATTR static bool s_lastPublishedDoorOpenValid = false;

static void blink_open_feedback();

static void print_door_state(bool open) {
  if (open) {
    ESP_LOGI(TAG, "DOOR: OPEN");
    blink_open_feedback();
  } else {
    ESP_LOGI(TAG, "DOOR: CLOSED");
  }
}

static void blink_open_feedback() {
  for (int i = 0; i < 3; ++i) {
    led::set_led(true);
    vTaskDelay(pdMS_TO_TICKS(80));
    led::set_led(false);
    vTaskDelay(pdMS_TO_TICKS(80));
  }
}

static bool publish_door_state(bool open) {
  const char *message = open ? "DOOR:OPEN" : "DOOR:CLOSED";
  ESP_LOGI(TAG, "ESP-NOW TX: %s", message);

  for (int attempt = 0; attempt < 3; ++attempt) {
    esp_err_t sendRet = espnow::send_broadcast(
        reinterpret_cast<const uint8_t *>(message),
        strlen(message));
    if (sendRet == ESP_OK) {
      s_lastPublishedDoorOpen = open;
      s_lastPublishedDoorOpenValid = true;
      return true;
    }

    ESP_LOGW(TAG, "ESP-NOW send failed (attempt %d): %s",
             attempt + 1, esp_err_to_name(sendRet));
    vTaskDelay(pdMS_TO_TICKS(40));
  }

  ESP_LOGW(TAG, "ESP-NOW TX dropped after retries: %s", message);
  return false;
}

static void enter_deep_sleep_for_next_change() {
  const gpio_config_t sensor_cfg = {
      .pin_bit_mask = (1ULL << s_config.sensorPin),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&sensor_cfg));

  int level = gpio_get_level(s_config.sensorPin);
  vTaskDelay(pdMS_TO_TICKS(s_config.debounceMs));
  level = gpio_get_level(s_config.sensorPin);

  const uint64_t wakeMask = (1ULL << s_config.sensorPin);
  const bool wakePinValid = esp_sleep_is_valid_wakeup_gpio(s_config.sensorPin);
  if (wakePinValid) {
    const esp_deepsleep_gpio_wake_up_mode_t wakeMode =
        level ? ESP_GPIO_WAKEUP_GPIO_LOW : ESP_GPIO_WAKEUP_GPIO_HIGH;
    ESP_ERROR_CHECK(esp_deep_sleep_enable_gpio_wakeup(wakeMask, wakeMode));
  } else {
    ESP_LOGW(TAG,
             "GPIO%d is not valid for deep sleep wakeup; timer fallback only",
             static_cast<int>(s_config.sensorPin));
  }

  if (!wakePinValid) {
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(kSafetyWakeupIntervalMs * 1000));
  }
  ESP_LOGI(TAG,
           "Entering deep sleep; GPIO%d=%d, gpioWakeValid=%s, timer fallback: %s%lld ms",
           static_cast<int>(s_config.sensorPin),
           level,
           wakePinValid ? "yes" : "no",
           wakePinValid ? "off/" : "on/",
           kSafetyWakeupIntervalMs);

  esp_deep_sleep_start();
}

void init(const Config &config) {
  s_config = config;
  s_initialized = true;

  s_wakeCause = esp_sleep_get_wakeup_cause();
  ESP_LOGI(TAG,
           "Wake cause: %d (%s)",
           static_cast<int>(s_wakeCause),
           wakeup_cause_to_string(s_wakeCause));
  ESP_LOGI(TAG,
           "Sensor GPIO%d deep-sleep wake capable: %s",
           static_cast<int>(s_config.sensorPin),
           esp_sleep_is_valid_wakeup_gpio(s_config.sensorPin) ? "yes" : "no");

  esp_log_level_set(TAG, ESP_LOG_INFO);
  esp_log_level_set("espnow", ESP_LOG_INFO);

  ESP_ERROR_CHECK(espnow::init(1));

  reed::init(s_config.sensorPin);
  led::init(s_config.ledPin);

  vTaskDelay(pdMS_TO_TICKS(s_config.debounceMs));
  const bool currentDoorOpen = reed::is_door_open();

  ESP_LOGI(TAG, "Boot");
  print_door_state(currentDoorOpen);
  const bool wakeFromSleep =
      (s_wakeCause == ESP_SLEEP_WAKEUP_GPIO || s_wakeCause == ESP_SLEEP_WAKEUP_TIMER);
  const bool stateChangedSinceLastPublish =
      (!s_lastPublishedDoorOpenValid || s_lastPublishedDoorOpen != currentDoorOpen);

  if (!wakeFromSleep || stateChangedSinceLastPublish) {
    publish_door_state(currentDoorOpen);
  } else {
    ESP_LOGI(TAG, "Skipping boot TX (unchanged from last published state)");
  }
}

void run() {
  if (!s_initialized) {
    ESP_LOGE(TAG, "app::init must be called before app::run");
    return;
  }

  bool currentDoorOpen = reed::is_door_open();

  if (s_wakeCause != ESP_SLEEP_WAKEUP_GPIO && s_wakeCause != ESP_SLEEP_WAKEUP_TIMER) {
    ESP_LOGI(TAG, "Boot grace %d ms before deep sleep", kBootUsbGraceMs);

    const int64_t graceStartMs = esp_timer_get_time() / 1000;
    while ((esp_timer_get_time() / 1000 - graceStartMs) < kBootUsbGraceMs) {
      bool sampledDoorOpen = reed::is_door_open();
      if (sampledDoorOpen != currentDoorOpen) {
        vTaskDelay(pdMS_TO_TICKS(s_config.debounceMs));
        sampledDoorOpen = reed::is_door_open();
        if (sampledDoorOpen != currentDoorOpen) {
          currentDoorOpen = sampledDoorOpen;
          print_door_state(currentDoorOpen);
          publish_door_state(currentDoorOpen);
        }
      }

      vTaskDelay(pdMS_TO_TICKS(s_config.loopDelayMs));
    }
  } else {
    ESP_LOGI(TAG,
             "Post-wake active window %d ms (cause=%s)",
             kPostWakeActiveMs,
             wakeup_cause_to_string(s_wakeCause));

    const int64_t activeStartMs = esp_timer_get_time() / 1000;
    while ((esp_timer_get_time() / 1000 - activeStartMs) < kPostWakeActiveMs) {
      bool sampledDoorOpen = reed::is_door_open();
      if (sampledDoorOpen != currentDoorOpen) {
        vTaskDelay(pdMS_TO_TICKS(s_config.debounceMs));
        sampledDoorOpen = reed::is_door_open();
        if (sampledDoorOpen != currentDoorOpen) {
          currentDoorOpen = sampledDoorOpen;
          print_door_state(currentDoorOpen);
          publish_door_state(currentDoorOpen);
        }
      }

      vTaskDelay(pdMS_TO_TICKS(s_config.loopDelayMs));
    }
  }

  enter_deep_sleep_for_next_change();
}

}  // namespace app