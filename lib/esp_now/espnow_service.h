#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

namespace espnow {

    typedef void (*ReceiveCallback)(const uint8_t *fromMac, const uint8_t *data, size_t len);

    esp_err_t init(uint8_t channel = 1);
    esp_err_t send_broadcast(const uint8_t *data, size_t len);
    esp_err_t send_to_peer(const uint8_t *peerMac, const uint8_t *data, size_t len);
    void set_receive_callback(ReceiveCallback callback);

}  // namespace espnow
