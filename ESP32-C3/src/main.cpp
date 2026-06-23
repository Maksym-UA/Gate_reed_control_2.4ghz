#include "application.h"

extern "C" void app_main(void) {
  const app::Config config = {
      .sensorPin = GPIO_NUM_4,
      .ledPin = GPIO_NUM_20,
      .voltagePin = GPIO_NUM_2,
      .debounceMs = 20,
      .loopDelayMs = 10,
  };

  app::init(config);
  app::run();
}