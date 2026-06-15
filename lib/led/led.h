#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace led {

struct TaskConfig {
  uint8_t pin;  // GPIO pin number for the LED
  uint32_t intervalMs; // Blink interval in milliseconds
  const char* taskName; // Name of the FreeRTOS task
  BaseType_t core; // Core to run the task on (0 or 1)
  UBaseType_t priority; // Task priority
  uint32_t stackWords; // Stack size in words
};

void startTasks(const TaskConfig* configs, size_t count);

}  // namespace led
