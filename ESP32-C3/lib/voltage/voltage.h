#pragma once

#include "driver/gpio.h"

namespace voltage {

    void init(gpio_num_t pin);
    float read_voltage();

}  // namespace voltage
