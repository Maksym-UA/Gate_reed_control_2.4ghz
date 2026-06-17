#include "application.h"

extern "C" void app_main(void) {
  const app::Config config = {
      .loopDelayMs = 10,
      .pushNotificationIntervalMs = 10000,
  };

  app::init(config);
  app::run();
}
//23:19:31.666 > I (553) wifi:mode : sta (ac:a7:04:26:e1:fc)