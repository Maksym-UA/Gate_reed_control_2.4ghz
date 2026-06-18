#pragma once

#include "driver/gpio.h"

namespace reed {

    void init(gpio_num_t pin);
    bool is_door_open();

}  // namespace reed
