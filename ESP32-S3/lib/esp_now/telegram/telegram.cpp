
#include "telegram.h"

#include <string.h>
#include <stdio.h>

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
// IMPORTANT: Replace with your actual WiFi credentials before flashing.
#define WIFI_SSID     "TP-Link_E3AC"
#define WIFI_PASSWORD "I&Mmansion2021"

#define TG_BOT_TOKEN  "8896687319:AAGYa5iFIVp8ieFSaaFNcf7xC2CAlWDRBE8"
#define TG_CHAT_ID    "687176418"

static const char *TAG = "telegram";

/* ── WiFi STA connection ──────────────────────────────────────────────────── */

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRIES   10

static EventGroupHandle_t s_wifiEventGroup = nullptr;
static int s_wifiRetryNum = 0;
static bool s_wifiConnected = false;

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

    // espnow::init() already created the default STA netif (before esp_wifi_start).
    // Just register the event handlers here.
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, nullptr));

    wifi_config_t wifi_config = {};
    strncpy(reinterpret_cast<char *>(wifi_config.sta.ssid),     WIFI_SSID,     sizeof(wifi_config.sta.ssid)     - 1);
    strncpy(reinterpret_cast<char *>(wifi_config.sta.password), WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);
    // WPA_WPA2_PSK accepts both WPA and WPA2, including mixed-mode APs.
    // WPA2_PSK is stricter and will reject APs that advertise WPA2/WPA3 mixed.
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    // PMF: capable but not required — compatible with most home routers.
    // If the router enforces PMF (WPA3 transition mode), set required=true.
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(s_wifiEventGroup,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(15000));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP: %s", WIFI_SSID);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to connect to AP: %s", WIFI_SSID);
    return ESP_FAIL;
}

/* ── HTTP event handler ───────────────────────────────────────────────────── */

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        ESP_LOGD(TAG, "HTTP response: %.*s", evt->data_len, static_cast<const char *>(evt->data));
    }
    return ESP_OK;
}

/* ── Send message ─────────────────────────────────────────────────────────── */

esp_err_t telegram_send_message(const char *text)
{
    if (!s_wifiConnected) {
        ESP_LOGW(TAG, "WiFi not connected, skipping Telegram message");
        return ESP_ERR_INVALID_STATE;
    }

    // Use POST + JSON so arbitrary characters in 'text' don't break the URL.
    static const char url[] =
        "https://api.telegram.org/bot" TG_BOT_TOKEN "/sendMessage";

    char body[512];
    snprintf(body, sizeof(body),
             "{\"chat_id\":\"%s\",\"text\":\"%s\"}", TG_CHAT_ID, text);

    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = http_event_handler;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, static_cast<int>(strlen(body)));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Telegram message sent, HTTP status=%d",
                 esp_http_client_get_status_code(client));
    } else {
        ESP_LOGE(TAG, "Telegram send failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}
