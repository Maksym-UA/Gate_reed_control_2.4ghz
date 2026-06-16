#pragma once

#include "driver/gpio.h"

namespace reed {

void init(gpio_num_t pin, bool enablePullup = true);
bool isDoorOpen();

}  // namespace reed
