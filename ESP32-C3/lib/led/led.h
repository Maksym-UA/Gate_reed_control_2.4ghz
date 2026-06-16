#pragma once

#include "driver/gpio.h"

namespace led {

    void init(gpio_num_t pin);
    void set_led(bool on);
    void toggle();
    bool state();

}  // namespace led
