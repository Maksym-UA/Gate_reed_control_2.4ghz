#include "application.h"

extern "C" void app_main(void) {
  const app::Config config = {
      .debounceMs = 20,
      .loopDelayMs = 10,
  };

  app::init(config);
  app::run();
}
