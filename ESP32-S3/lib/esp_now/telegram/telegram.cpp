#include "telegram.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

/* ── Credentials ──────────────────────────────────────────────────────────── */
/* Defined in secrets.h (gitignored). Copy secrets.h.template → secrets.h.  */
#include "secrets.h"

static const char *TAG = "telegram";

/* ── WiFi STA connection ──────────────────────────────────────────────────── */

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRIES   10

static EventGroupHandle_t s_wifiEventGroup = nullptr;
static int s_wifiRetryNum = 0;
static bool s_wifiConnected = false;
static int64_t s_lastUpdateId = 0;

static char s_httpResponse[2048];
static int s_httpResponseLen = 0;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = static_cast<wifi_event_sta_disconnected_t *>(event_data);
        ESP_LOGW(TAG, "WiFi disconnected, reason=%d", disc ? (int)disc->reason : -1);
        if (s_wifiRetryNum < WIFI_MAX_RETRIES) {
            esp_wifi_connect();
            s_wifiRetryNum++;
            ESP_LOGW(TAG, "WiFi reconnect attempt %d/%d", s_wifiRetryNum, WIFI_MAX_RETRIES);
        } else {
            xEventGroupSetBits(s_wifiEventGroup, WIFI_FAIL_BIT);
        }
        s_wifiConnected = false;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = static_cast<ip_event_got_ip_t *>(event_data);
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifiRetryNum = 0;
        s_wifiConnected = true;
        xEventGroupSetBits(s_wifiEventGroup, WIFI_CONNECTED_BIT);
    }
}

esp_err_t telegram_init(void)
{
    if (s_wifiConnected) {
        return ESP_OK;
    }

    s_wifiEventGroup = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, nullptr));

    wifi_config_t wifi_config = {};
    strncpy(reinterpret_cast<char *>(wifi_config.sta.ssid), WIFI_SSID,
            sizeof(wifi_config.sta.ssid) - 1);
    strncpy(reinterpret_cast<char *>(wifi_config.sta.password), WIFI_PASSWORD,
            sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifiEventGroup,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP: %s", WIFI_SSID);
        // S3 is mains-powered — disable modem sleep to reduce post-longpoll TX latency.
        esp_wifi_set_ps(WIFI_PS_NONE);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to connect to AP: %s", WIFI_SSID);
    return ESP_FAIL;
}

/* ── HTTP helpers ─────────────────────────────────────────────────────────── */

static void reset_http_response_buffer()
{
    s_httpResponseLen = 0;
    s_httpResponse[0] = '\0';
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        int remaining = (int)sizeof(s_httpResponse) - 1 - s_httpResponseLen;
        int copy_len = evt->data_len < remaining ? evt->data_len : remaining;
        if (copy_len > 0) {
            memcpy(&s_httpResponse[s_httpResponseLen], evt->data, copy_len);
            s_httpResponseLen += copy_len;
            s_httpResponse[s_httpResponseLen] = '\0';
        }
    }
    return ESP_OK;
}

static esp_err_t telegram_http_post_json(const char *url, const char *body)
{
    if (!s_wifiConnected) {
        ESP_LOGW(TAG, "WiFi not connected, skipping Telegram request");
        return ESP_ERR_INVALID_STATE;
    }

    reset_http_response_buffer();

    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = http_event_handler;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 20000;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Telegram POST OK, status=%d", esp_http_client_get_status_code(client));
    } else {
        ESP_LOGE(TAG, "Telegram POST failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t telegram_http_get(const char *url)
{
    if (!s_wifiConnected) {
        ESP_LOGW(TAG, "WiFi not connected, skipping Telegram request");
        return ESP_ERR_INVALID_STATE;
    }

    reset_http_response_buffer();

    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = http_event_handler;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 35000; // 25 s long-poll + 10 s margin

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_GET);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Telegram GET OK, status=%d", esp_http_client_get_status_code(client));
    } else {
        ESP_LOGE(TAG, "Telegram GET failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}

/* ── Telegram send helpers ────────────────────────────────────────────────── */

esp_err_t telegram_send_message(const char *text)
{
    static const char url[] =
        "https://api.telegram.org/bot" TG_BOT_TOKEN "/sendMessage";

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "chat_id", TG_CHAT_ID);
    cJSON_AddStringToObject(obj, "text",    text);
    char *body = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!body) return ESP_ERR_NO_MEM;

    esp_err_t err = telegram_http_post_json(url, body);
    cJSON_free(body);
    return err;
}

esp_err_t telegram_send_admin_menu(void)
{
    static const char url[] =
        "https://api.telegram.org/bot" TG_BOT_TOKEN "/sendMessage";

    static char body[768];
    snprintf(body, sizeof(body),
             "{\"chat_id\":\"%s\","
             "\"text\":\"Admin menu\","
             "\"reply_markup\":{"
                 "\"inline_keyboard\":[["
                     "{\"text\":\"Check voltage\",\"callback_data\":\"CHECK_VOLTAGE\"}"
                 "]]"
             "}"
             "}",
             TG_CHAT_ID);

    return telegram_http_post_json(url, body);
}

static esp_err_t telegram_answer_callback(const char *callback_query_id, const char *text)
{
    static const char url[] =
        "https://api.telegram.org/bot" TG_BOT_TOKEN "/answerCallbackQuery";

    static char body[512];
    snprintf(body, sizeof(body),
             "{\"callback_query_id\":\"%s\",\"text\":\"%s\"}",
             callback_query_id, text);

    return telegram_http_post_json(url, body);
}

esp_err_t telegram_send_voltage_reply(float voltage)
{
    char msg[128];
    snprintf(msg, sizeof(msg), "🔋 Saved voltage: %.2f V", voltage);
    return telegram_send_message(msg);
}

esp_err_t telegram_send_unauthorized_reply(void)
{
    return telegram_send_message("⛔ Unauthorized");
}

esp_err_t telegram_ack_callback(const char *callback_query_id, bool is_admin)
{
    return telegram_answer_callback(callback_query_id,
                                    is_admin ? "Reading saved voltage..." : "Unauthorized");
}

/* ── Update parsing ───────────────────────────────────────────────────────── */

esp_err_t telegram_poll_updates(telegram_command_result_t *result)
{
    if (result == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    result->has_request = false;
    result->is_admin = false;
    result->wants_voltage = false;

    static char url[512];
    snprintf(url, sizeof(url),
             "https://api.telegram.org/bot%s/getUpdates?offset=%lld&timeout=25&allowed_updates=[\"message\",\"callback_query\"]",
             TG_BOT_TOKEN,
             (long long)(s_lastUpdateId + 1));

    esp_err_t err = telegram_http_get(url);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_ParseWithLength(s_httpResponse, (size_t)s_httpResponseLen);
    if (!root) {
        ESP_LOGW(TAG, "cJSON parse failed");
        return ESP_OK;
    }

    cJSON *result_arr = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (!cJSON_IsArray(result_arr) || cJSON_GetArraySize(result_arr) == 0) {
        cJSON_Delete(root);
        return ESP_OK;
    }

    // Process only the first update; advance offset so it is not returned again.
    cJSON *update = cJSON_GetArrayItem(result_arr, 0);
    cJSON *uid    = cJSON_GetObjectItemCaseSensitive(update, "update_id");
    if (cJSON_IsNumber(uid)) {
        s_lastUpdateId = (int64_t)uid->valuedouble;
    }

    // /voltage text command
    cJSON *message = cJSON_GetObjectItemCaseSensitive(update, "message");
    if (message) {
        cJSON *text = cJSON_GetObjectItemCaseSensitive(message, "text");
        cJSON *from = cJSON_GetObjectItemCaseSensitive(message, "from");
        cJSON *fid  = from ? cJSON_GetObjectItemCaseSensitive(from, "id") : nullptr;

        if (cJSON_IsString(text) && cJSON_IsNumber(fid) &&
            strcmp(text->valuestring, "/voltage") == 0) {
            result->has_request   = true;
            result->wants_voltage = true;
            char buf[32];
            snprintf(buf, sizeof(buf), "%.0f", fid->valuedouble);
            result->is_admin = (strcmp(buf, TG_ADMIN_ID) == 0);
        }
    }

    // CHECK_VOLTAGE inline button callback
    cJSON *cb = cJSON_GetObjectItemCaseSensitive(update, "callback_query");
    if (cb) {
        cJSON *cb_id = cJSON_GetObjectItemCaseSensitive(cb, "id");
        cJSON *data  = cJSON_GetObjectItemCaseSensitive(cb, "data");
        cJSON *from  = cJSON_GetObjectItemCaseSensitive(cb, "from");
        cJSON *fid   = from ? cJSON_GetObjectItemCaseSensitive(from, "id") : nullptr;

        if (cJSON_IsString(data) && strcmp(data->valuestring, "CHECK_VOLTAGE") == 0) {
            result->has_request   = true;
            result->wants_voltage = true;

            if (cJSON_IsNumber(fid)) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%.0f", fid->valuedouble);
                result->is_admin = (strcmp(buf, TG_ADMIN_ID) == 0);
            }
            if (cJSON_IsString(cb_id)) {
                strncpy(result->pending_callback_id, cb_id->valuestring,
                        sizeof(result->pending_callback_id) - 1);
                result->pending_callback_id[sizeof(result->pending_callback_id) - 1] = '\0';
            }
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}