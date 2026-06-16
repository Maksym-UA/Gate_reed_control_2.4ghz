#include "espnow_service.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_now.h"

namespace espnow {

  static const char *TAG = "espnow";
  static bool s_initialized = false;
  static uint8_t s_channel = 1;
  static ReceiveCallback s_receiveCallback = nullptr;
  static const uint8_t s_broadcastMac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

  /// @brief Ensure that NVS is initialized, as it's required for Wi-Fi and ESP-NOW.
  static void ensure_nvs() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_INITIALIZED) {
      ESP_ERROR_CHECK(ret);
    }
}

/// @brief Ensure Wi-Fi is already initialized by the STA stack before ESP-NOW starts using it.
static void ensure_wifi_ready() {
  wifi_mode_t currentMode = WIFI_MODE_NULL;
  esp_err_t ret = esp_wifi_get_mode(&currentMode);

  if (ret == ESP_ERR_WIFI_NOT_INIT) {
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
      ESP_ERROR_CHECK(ret);
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
      ESP_ERROR_CHECK(ret);
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    return;
  }
  ESP_ERROR_CHECK(ret);

  if (currentMode == WIFI_MODE_NULL) {
    ESP_LOGW(TAG, "Wi-Fi is not started yet; ESP-NOW will not receive until STA is up");
  }
}

/// @brief Ensure that the broadcast peer is added, as it's required for ESP-NOW broadcast.
static void ensure_broadcast_peer() {
  if (!esp_now_is_peer_exist(s_broadcastMac)) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, s_broadcastMac, ESP_NOW_ETH_ALEN);
    peer.channel = s_channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
  }
}

static void on_send(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
  (void)tx_info;
  ESP_LOGI(TAG, "send status=%s", status == ESP_NOW_SEND_SUCCESS ? "ok" : "fail");
}

static void on_recv(const esp_now_recv_info_t *recvInfo, const uint8_t *data, int len) {
  if (recvInfo == nullptr || recvInfo->src_addr == nullptr || data == nullptr || len <= 0) {
    return;
  }

  ESP_LOGI(
      TAG,
      "recv from %02X:%02X:%02X:%02X:%02X:%02X len=%d",
      recvInfo->src_addr[0],
      recvInfo->src_addr[1],
      recvInfo->src_addr[2],
      recvInfo->src_addr[3],
      recvInfo->src_addr[4],
      recvInfo->src_addr[5],
      len);

  if (s_receiveCallback != nullptr) {
    s_receiveCallback(recvInfo->src_addr, data, static_cast<size_t>(len));
  }
}


esp_err_t init(uint8_t channel) {
  if (s_initialized) {
    return ESP_OK;
  }

  s_channel = channel;
  ensure_nvs();
  ensure_wifi_ready();

  ESP_ERROR_CHECK(esp_now_init());
  ESP_ERROR_CHECK(esp_now_register_send_cb(on_send));
  ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));

  ensure_broadcast_peer();

  s_initialized = true;
  ESP_LOGI(TAG, "initialized on channel %u", static_cast<unsigned>(s_channel));
  return ESP_OK;
}

esp_err_t send_broadcast(const uint8_t *data, size_t len) {
  if (!s_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  if (data == nullptr || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  return esp_now_send(s_broadcastMac, data, static_cast<int>(len));
}

esp_err_t send_to_peer(const uint8_t *peerMac, const uint8_t *data, size_t len) {
  if (!s_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  if (peerMac == nullptr || data == nullptr || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  if (!esp_now_is_peer_exist(peerMac)) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, peerMac, ESP_NOW_ETH_ALEN);
    peer.channel = s_channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_err_t addRet = esp_now_add_peer(&peer);
    if (addRet != ESP_OK && addRet != ESP_ERR_ESPNOW_EXIST) {
      return addRet;
    }
  }

  return esp_now_send(peerMac, data, static_cast<int>(len));
}

void set_receive_callback(ReceiveCallback callback) {
  s_receiveCallback = callback;
}

}  // namespace espnow
