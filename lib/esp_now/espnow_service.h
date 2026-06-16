#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

namespace espnow {

typedef void (*ReceiveCallback)(const uint8_t *fromMac, const uint8_t *data, size_t len);

esp_err_t init(uint8_t channel = 1);
esp_err_t sendBroadcast(const uint8_t *data, size_t len);
esp_err_t sendToPeer(const uint8_t *peerMac, const uint8_t *data, size_t len);
void setReceiveCallback(ReceiveCallback callback);

}  // namespace espnow
