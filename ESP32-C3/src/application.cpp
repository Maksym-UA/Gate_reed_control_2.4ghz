#include "application.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "espnow_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "led.h"
#include "reed.h"
#include "voltage.h"

namespace app {

  static const char *TAG = "gate_reed";
  static const int kBootUsbGraceMs = 15000;
  static const int64_t kFallbackWakeupIntervalMs = 3000;
  static const int64_t kOpenStateWakeupIntervalMs = 30000;
  static const int64_t kKeepAliveWakeupIntervalMs = 5 * 60 * 1000; // 5 min when closed — GPIO wakeup handles state changes
  static const int64_t kPeriodicKeepAliveMs = 30000;               // keepalive fires every timer wake in both states
  static const int kPostWakeActiveMs = 3500;


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
  RTC_DATA_ATTR static bool s_lastPublishedDoorOpen = false; // RTC_DATA_ATTR retains value across deep sleep cycles
  RTC_DATA_ATTR static bool s_lastPublishedDoorOpenValid = false;
  RTC_DATA_ATTR static uint32_t s_unchangedTimerWakeCount = 0;

  // LED blink command posted to s_ledQueue by the main task.
  struct LedCmd {
    int count;
    int on_ms;
    int off_ms;
  };

  // FreeRTOS objects for LED blink and voltage TX tasks.
  // The main task waits for both to finish before entering deep sleep.
  static QueueHandle_t      s_ledQueue      = nullptr;
  static EventGroupHandle_t s_ledEvents     = nullptr;
  static TaskHandle_t       s_ledTaskHandle = nullptr;
  static const EventBits_t  kLedIdleBit     = BIT0;
  static const EventBits_t  kVoltIdleBit    = BIT1;
  static const EventBits_t  kAllIdleBits    = BIT0 | BIT1;

  static void blink_state_feedback(bool open);
  static void led_task(void *arg);
  static void voltage_task(void *arg);

  static void print_door_state(bool open) {
    if (open) {
      ESP_LOGI(TAG, "DOOR: OPEN");
    } else {
      ESP_LOGI(TAG, "DOOR: CLOSED");
    }
  }

  // Posts a blink command to the LED task queue (non-blocking).
  // The LED task executes the sequence at priority 3 so the main sensor
  // loop (priority 5) keeps polling the reed switch during blink delays.
  static void blink_state_feedback(bool open) {
    if (s_ledQueue == nullptr) return;
    const LedCmd cmd = open ? LedCmd{3, 80, 80} : LedCmd{1, 250, 0};
    xQueueSend(s_ledQueue, &cmd, pdMS_TO_TICKS(200));
  }

  // Low-priority LED task: drives GPIO based on commands from s_ledQueue.
  // Signals idle via kLedIdleBit so enter_deep_sleep_for_next_change() can
  // wait for any in-progress blink to finish before sleeping.
  static void led_task(void *) {
    while (true) {
      LedCmd cmd;
      xQueueReceive(s_ledQueue, &cmd, portMAX_DELAY);
      xEventGroupClearBits(s_ledEvents, kLedIdleBit);  // busy
      for (int i = 0; i < cmd.count; ++i) {
        led::set_led(true);
        vTaskDelay(pdMS_TO_TICKS(cmd.on_ms));
        led::set_led(false);
        if (cmd.off_ms > 0) vTaskDelay(pdMS_TO_TICKS(cmd.off_ms));
      }
      led::set_led(false);
      vTaskDelay(pdMS_TO_TICKS(50));
      xEventGroupSetBits(s_ledEvents, kLedIdleBit);    // idle
    }
  }

  // One-shot battery voltage task: reads ADC, broadcasts BATT message, then waits
  // for ESP-NOW TX to complete (send_broadcast is async) before signalling idle
  // and self-deleting. Runs at priority 2.
  static void voltage_task(void *) {
    // Allow the RC voltage-divider node to settle after wake before sampling.
    vTaskDelay(pdMS_TO_TICKS(20));
    const float v = voltage::read_voltage();
    char msg[16];
    snprintf(msg, sizeof(msg), "BATT:%.2fV", v);
    ESP_LOGI(TAG, "Battery voltage: %.2fV", v);
    espnow::send_broadcast(reinterpret_cast<const uint8_t *>(msg), strlen(msg));
    // esp_now_send() is async; wait for the WiFi driver to complete the TX
    // before signalling idle, otherwise deep sleep kills the packet in flight.
    vTaskDelay(pdMS_TO_TICKS(200));
    if (s_ledEvents != nullptr) {
      xEventGroupSetBits(s_ledEvents, kVoltIdleBit);
    }
    vTaskDelete(nullptr);
  }

  static bool publish_door_state(bool open, const char *txKind) {
    const char *message = open ? "DOOR:OPEN" : "DOOR:CLOSED";
    ESP_LOGI(TAG, "ESP-NOW TX [%s]: %s", txKind, message);

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

    ESP_LOGW(TAG, "ESP-NOW TX [%s] dropped after retries: %s", txKind, message);
    return false;
  }

// Prepares for deep sleep until the next door state change. Configures GPIO wakeup
// if supported, otherwise falls back to a timer wakeup. Waits for any in-progress
// LED blink or voltage TX to finish before sleeping. Holds the LED pin LOW across
// deep sleep to prevent ghost dim blinks on wake.
  static void enter_deep_sleep_for_next_change(bool currentDoorOpen) {
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

    // Use the debounced logical state from the run loop for timer policy.
    // A single raw sample taken right before sleep can be biased by wiring/leakage.
    const bool doorOpenNow = currentDoorOpen;
    const int64_t timerWakeupIntervalMs = !wakePinValid
        ? kFallbackWakeupIntervalMs
        : (doorOpenNow ? kOpenStateWakeupIntervalMs : kKeepAliveWakeupIntervalMs);
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(timerWakeupIntervalMs * 1000));

    // Hold LED pin LOW across deep sleep so GPIO20 never floats during wake-up,
    // eliminating the ghost dim blink caused by a floating pin before led::init() runs.
    led::set_led(false);
    // Wait for both the LED blink and voltage TX to complete before sleeping.
    // Voltage task adds 200ms post-send delay to ensure the radio TX finishes.
    // Total timeout 1.5s covers the worst case (blink ~580ms + volt TX ~200ms).
    if (s_ledEvents != nullptr) {
      xEventGroupWaitBits(s_ledEvents, kAllIdleBits,
                          pdFALSE, pdTRUE, pdMS_TO_TICKS(1500));
    }
    // Clean up FreeRTOS objects; they are recreated after the next wake.
    if (s_ledTaskHandle != nullptr) {
      vTaskDelete(s_ledTaskHandle);
      s_ledTaskHandle = nullptr;
    }
    if (s_ledQueue != nullptr) {
      vQueueDelete(s_ledQueue);
      s_ledQueue = nullptr;
    }
    if (s_ledEvents != nullptr) {
      vEventGroupDelete(s_ledEvents);
      s_ledEvents = nullptr;
    }
    gpio_hold_en(s_config.ledPin);

    ESP_LOGI(TAG,
            "Entering deep sleep; GPIO%d=%d, doorOpen=%s, gpioWakeValid=%s, timer wake: %lld ms",
            static_cast<int>(s_config.sensorPin),
            level,
            doorOpenNow ? "yes" : "no",
            wakePinValid ? "yes" : "no",
            timerWakeupIntervalMs);

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

    // Boost this task's priority above the LED (3) and voltage (2) tasks so the
    // sensor polling loop always preempts them on single-core ESP32-C3.
    vTaskPrioritySet(nullptr, 5);

    // Release GPIO hold set before previous deep sleep (hold keeps LED LOW while chip
    // is asleep and during the early boot phase, preventing the ghost dim blink).
    gpio_hold_dis(s_config.ledPin);
    led::init(s_config.ledPin);

    // Create LED queue and event group; start both bits idle so the pre-sleep
    // wait passes immediately when no blink or voltage TX is pending.
    s_ledQueue  = xQueueCreate(4, sizeof(LedCmd));
    s_ledEvents = xEventGroupCreate();
    xEventGroupSetBits(s_ledEvents, kAllIdleBits);
    xTaskCreate(led_task, "led_task", 2048, nullptr, 3, &s_ledTaskHandle);

    voltage::init(s_config.voltagePin);

    ESP_ERROR_CHECK(espnow::init(4));

    reed::init(s_config.sensorPin);

    vTaskDelay(pdMS_TO_TICKS(s_config.debounceMs));
    const bool currentDoorOpen = reed::is_door_open();

    ESP_LOGI(TAG, "Boot");
    print_door_state(currentDoorOpen);
    const bool wakeFromSleep =
        (s_wakeCause == ESP_SLEEP_WAKEUP_GPIO || s_wakeCause == ESP_SLEEP_WAKEUP_TIMER);
    const bool stateChangedSinceLastPublish =
        (!s_lastPublishedDoorOpenValid || s_lastPublishedDoorOpen != currentDoorOpen);
    const bool isTimerWake = (s_wakeCause == ESP_SLEEP_WAKEUP_TIMER);

    bool shouldPublish = !wakeFromSleep || stateChangedSinceLastPublish;
    if (isTimerWake && !stateChangedSinceLastPublish) {
      const int64_t currentTimerWakeIntervalMs =
          currentDoorOpen ? kOpenStateWakeupIntervalMs : kKeepAliveWakeupIntervalMs;
      const uint32_t keepAliveWakeThreshold =
          static_cast<uint32_t>((kPeriodicKeepAliveMs + currentTimerWakeIntervalMs - 1) /
                                currentTimerWakeIntervalMs);
      ++s_unchangedTimerWakeCount;
      shouldPublish = (s_unchangedTimerWakeCount >= keepAliveWakeThreshold);
    }

    if (shouldPublish) {
      const char *txKind = "CHANGE";
      if (!wakeFromSleep && !s_lastPublishedDoorOpenValid) {
        txKind = "BOOT";
      } else if (isTimerWake && !stateChangedSinceLastPublish) {
        txKind = "KEEPALIVE";
      }

      // Blink on genuine state changes detected at wake (before the run() loop starts).
      // Without this, only the ghost dim blink would be visible for between-sleep transitions.
      if (wakeFromSleep && stateChangedSinceLastPublish) {
        blink_state_feedback(currentDoorOpen);
      }

      const bool published = publish_door_state(currentDoorOpen, txKind);
      if (published) {
        s_unchangedTimerWakeCount = 0;
      }
    } else {
      ESP_LOGI(TAG, "Skipping boot TX (unchanged timer wake, keepalive not due)");
    }

    // Battery TX: measure and broadcast voltage once when the door first opens.
    if (currentDoorOpen && stateChangedSinceLastPublish) {
      ESP_LOGI(TAG, "Battery TX: door opened, spawning voltage_task");
      if (s_ledEvents != nullptr) {
        xEventGroupClearBits(s_ledEvents, kVoltIdleBit);
      }
      xTaskCreate(voltage_task, "volt_task", 4096, nullptr, 2, nullptr);
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
            blink_state_feedback(currentDoorOpen);
            publish_door_state(currentDoorOpen, "CHANGE");
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
            blink_state_feedback(currentDoorOpen);
            publish_door_state(currentDoorOpen, "CHANGE");
          }
        }

        vTaskDelay(pdMS_TO_TICKS(s_config.loopDelayMs));
      }
    }

    enter_deep_sleep_for_next_change(currentDoorOpen);
  }

}  // namespace app