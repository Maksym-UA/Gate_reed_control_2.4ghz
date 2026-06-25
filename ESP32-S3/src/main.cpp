#include "application.h"

extern "C" void app_main(void) {
  const app::Config config = {
      .loopDelayMs = 10,
      .pushNotificationIntervalMs = 10000,
  };

  app::init(config);
  app::run();
}