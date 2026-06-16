#include "led.h"

#include "driver/gpio.h"

namespace led {

  static gpio_num_t s_pin = GPIO_NUM_NC;
  static bool s_is_on = false;

  void init(gpio_num_t pin) {
    s_pin = pin;

    const gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    gpio_config(&led_cfg);
   
    set_led(false);
  }

  void set_led(bool on) {
    s_is_on = on;
    if (s_pin != GPIO_NUM_NC) {
      gpio_set_level(s_pin, s_is_on ? 1 : 0);
    }
  }

  void toggle() {
    set_led(!s_is_on);
  }

  bool state() {
    return s_is_on;
  }

}  // namespace led
