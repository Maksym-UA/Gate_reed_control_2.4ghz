#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool has_request;
    bool is_admin;
    bool wants_voltage;
    char pending_callback_id[128]; // non-empty when answerCallbackQuery must be sent
} telegram_command_result_t;

/**
 * Connect to WiFi and prepare HTTPS.
 */
esp_err_t telegram_init(void);

/**
 * Send a plain text message to the configured chat.
 */
esp_err_t telegram_send_message(const char *text);

/**
 * Send admin menu with inline button "Check voltage".
 */
esp_err_t telegram_send_admin_menu(void);

/**
 * Poll Telegram Bot API getUpdates and parse new commands/callbacks.
 * Non-blocking except for the HTTPS request itself.
 */
esp_err_t telegram_poll_updates(telegram_command_result_t *result);

/**
 * Answer admin with the currently saved voltage.
 */
esp_err_t telegram_send_voltage_reply(float voltage);

/**
 * Optional helper for unauthorized attempts.
 */
esp_err_t telegram_send_unauthorized_reply(void);

/**
 * Acknowledge a callback_query (must be called within 30 s of receiving it).
 */
esp_err_t telegram_ack_callback(const char *callback_query_id, bool is_admin);

#ifdef __cplusplus
}
#endif
