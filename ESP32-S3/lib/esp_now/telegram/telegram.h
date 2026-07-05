#pragma once

#include "esp_err.h"

/**
 * @brief Connect to the configured WiFi AP and prepare the HTTPS stack.
 *        Must be called after espnow::init() (which creates the netif/event-loop).
 */
esp_err_t telegram_init(void);

/**
 * @brief Send a plain-text message to the configured Telegram chat.
 *        Blocks until the HTTP request completes. Call only from a task context,
 *        never from an ISR or ESP-NOW receive callback.
 */
esp_err_t telegram_send_message(const char *text);
