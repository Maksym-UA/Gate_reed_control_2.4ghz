#pragma once

#include <stdint.h>

#include "driver/gpio.h"

namespace app {

    struct Config {
        gpio_num_t sensorPin;
        gpio_num_t ledPin;
        int64_t blinkIntervalMs;
        int debounceMs;
        int loopDelayMs;
    };

    void init(const Config &config);
    void run();

}  // namespace app
