#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const gpio_num_t SENSOR_PIN = GPIO_NUM_4;
static const gpio_num_t LED_PIN = GPIO_NUM_20;

static bool last_door_open = true;
static int led_state = 0;
static int64_t last_blink_time_ms = 0;
static const int64_t blink_interval_ms = 300;
static const char *TAG = "gate_reed";

// Returns true if door is open.
static bool read_door_open(void) {
  return gpio_get_level(SENSOR_PIN) == 1;
}

static void print_door_state(bool open) {
  if (open) {
    ESP_LOGI(TAG, "DOOR: OPEN");
  } else {
    ESP_LOGI(TAG, "DOOR: CLOSED");
  }
}

extern "C" void app_main(void) {
  const gpio_config_t sensor_cfg = {
      .pin_bit_mask = (1ULL << SENSOR_PIN),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };

  const gpio_config_t led_cfg = {
      .pin_bit_mask = (1ULL << LED_PIN),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };

  gpio_config(&sensor_cfg);
  gpio_config(&led_cfg);

  vTaskDelay(pdMS_TO_TICKS(500));

  bool current_door_open = read_door_open();
  last_door_open = current_door_open;

  ESP_LOGI(TAG, "Boot");
  print_door_state(current_door_open);

  if (!current_door_open) {
    gpio_set_level(LED_PIN, 0);
  }

  while (true) {
    const int64_t current_millis = esp_timer_get_time() / 1000;
    current_door_open = read_door_open();

    // Check for door state changes (debounced).
    if (current_door_open != last_door_open) {
      vTaskDelay(pdMS_TO_TICKS(20));
      current_door_open = read_door_open();

      if (current_door_open != last_door_open) {
        last_door_open = current_door_open;
        print_door_state(current_door_open);

        // If the door just closed, force the LED OFF immediately.
        if (!current_door_open) {
          gpio_set_level(LED_PIN, 0);
          led_state = 0;
        }
      }
    }

    // Handle LED blinking (only runs if the door is open).
    if (current_door_open) {
      if ((current_millis - last_blink_time_ms) >= blink_interval_ms) {
        last_blink_time_ms = current_millis;
        led_state = (led_state == 0) ? 1 : 0;
        gpio_set_level(LED_PIN, led_state);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
