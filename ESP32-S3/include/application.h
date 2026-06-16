#pragma once

#include <stdint.h>

#include "driver/gpio.h"

namespace app {

    struct Config {
        int debounceMs;
        int loopDelayMs;
    };

    void init(const Config &config);
    void run();

}  // namespace app
