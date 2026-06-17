#pragma once

#include <stdint.h>

#include "driver/gpio.h"

namespace app {

    struct Config {
        int loopDelayMs;
        int64_t pushNotificationIntervalMs;
    };

    void init(const Config &config);
    void run();

}  // namespace app
