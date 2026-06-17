#include "application.h"

#include <string.h>

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
static const int kStateConfirmMs = 250;
static const int64_t kSafetyWakeupIntervalMs = 15000;
static const int kPostWakeActiveMs = 3000;

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

static void on_esp_now_receive(const uint8_t *fromMac, const uint8_t *data, size_t len) {
  (void)fromMac;
  ESP_LOGI(TAG, "ESP-NOW RX: %.*s", static_cast<int>(len), reinterpret_cast<const char *>(data));
}

static Config s_config = {};
static bool s_initialized = false;
static esp_sleep_wakeup_cause_t s_wakeCause = ESP_SLEEP_WAKEUP_UNDEFINED;

static void blink_open_feedback();

static bool confirm_state_stable(bool expectedOpen, int confirmMs) {
  const int64_t startMs = esp_timer_get_time() / 1000;
  while ((esp_timer_get_time() / 1000 - startMs) < confirmMs) {
    if (reed::is_door_open() != expectedOpen) {
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(s_config.loopDelayMs));
  }
  return true;
}

static bool read_stable_door_state() {
  bool sampled = reed::is_door_open();
  vTaskDelay(pdMS_TO_TICKS(s_config.debounceMs));
  sampled = reed::is_door_open();

  if (!confirm_state_stable(sampled, kStateConfirmMs)) {
    // If unstable, resample once and trust the new stable candidate.
    sampled = reed::is_door_open();
    vTaskDelay(pdMS_TO_TICKS(s_config.debounceMs));
    sampled = reed::is_door_open();
  }
  return sampled;
}

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
  (void)currentDoorOpen;

  // Reconfigure sensor pin right before sleep and sample twice for a stable baseline.
  const gpio_config_t sensor_cfg = {
    .pin_bit_mask = (1ULL << s_config.sensorPin),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
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
             "GPIO%d is not valid for deep sleep wakeup; using timer fallback only",
             static_cast<int>(s_config.sensorPin));
  }

  ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(kSafetyWakeupIntervalMs * 1000));
  ESP_LOGI(TAG,
           "Entering deep sleep; GPIO%d=%d, gpioWakeValid=%s, timer fallback: %lld ms",
           static_cast<int>(s_config.sensorPin),
           level,
           wakePinValid ? "yes" : "no",
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

  // Keep global WARN level for smaller firmware, but show INFO for app modules.
  esp_log_level_set(TAG, ESP_LOG_INFO);
  esp_log_level_set("espnow", ESP_LOG_INFO);

  ESP_ERROR_CHECK(espnow::init(1));
  espnow::set_receive_callback(on_esp_now_receive);

  reed::init(s_config.sensorPin, true);
  led::init(s_config.ledPin);

  const bool currentDoorOpen = read_stable_door_state();

  ESP_LOGI(TAG, "Boot");
  print_door_state(currentDoorOpen);
  publish_door_state(currentDoorOpen);
}

void run() {
  if (!s_initialized) {
    ESP_LOGE(TAG, "app::init must be called before app::run");
    return;
  }

  bool currentDoorOpen = read_stable_door_state();

  // After upload/reset, keep USB serial alive for a short time for monitoring/flashing.
  if (s_wakeCause != ESP_SLEEP_WAKEUP_GPIO && s_wakeCause != ESP_SLEEP_WAKEUP_TIMER) {
    ESP_LOGI(TAG, "Boot grace %d ms before deep sleep", kBootUsbGraceMs);

    const int64_t graceStartMs = esp_timer_get_time() / 1000;
    while ((esp_timer_get_time() / 1000 - graceStartMs) < kBootUsbGraceMs) {
      bool sampledDoorOpen = reed::is_door_open();
      if (sampledDoorOpen != currentDoorOpen) {
        if (confirm_state_stable(sampledDoorOpen, kStateConfirmMs)) {
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
        if (confirm_state_stable(sampledDoorOpen, kStateConfirmMs)) {
          currentDoorOpen = sampledDoorOpen;
          print_door_state(currentDoorOpen);
          publish_door_state(currentDoorOpen);
        }
      }

      vTaskDelay(pdMS_TO_TICKS(s_config.loopDelayMs));
    }
  }

  enter_deep_sleep_for_next_change(currentDoorOpen);
}

}  // namespace app