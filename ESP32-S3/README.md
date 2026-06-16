# ESP32-S3 BME280 MQTT Publisher

## Overview

Firmware for the ESP32-S3-DevKitC-1 that connects to Wi-Fi, reads BME280 environmental data, and publishes the results over MQTT.

- Platform: ESP32-S3-DevKitC-1
- Framework: ESP-IDF via PlatformIO
- Sensor: BME280 on I2C
- Transport: MQTT over Wi-Fi
- SSD1306 128x64 OLED

`src/main.cpp` stays minimal and only calls `application_init()`. The runtime logic lives in `src/application.cpp`.

## Hardware

| Component | Value / Details |
|---|---|
| MCU | ESP32-S3-DevKitC-1 |
| Sensor | BME280 |
| BME280 SDA | GPIO8 |
| BME280 SCL | GPIO9 |
| Status LED | GPIO16 |

## Wi-Fi And MQTT

The firmware connects to the Wi-Fi network defined in `lib/credentials/credentials.h` and then starts the MQTT client in `lib/mqtt/`.

- Broker: `mqtt://broker.hivemq.com:1883`
- Topic prefix: `module59_11/`
- Commands: `module59_11/commands`
- Status: `module59_11/status`

Published sensor topics:

- `module59_11/temperature`
- `module59_11/humidity`
- `module59_11/pressure`

## Build, Upload, Monitor

Build:

```bash
pio run
```

Upload:

```bash
pio run -t upload
```

Serial monitor:

```bash
pio device monitor -b 115200
```

## Runtime Flow

1. `main.cpp` calls `application_init()`.
2. `Application::start()` enters the app runtime.
3. `Application::run()` initializes NVS, GPIO, Wi-Fi, and MQTT.
4. The BME280 is initialized over I2C.
5. The app waits for MQTT to connect.
6. Every 15 seconds it reads temperature, humidity, and pressure.
7. Each value is published to its MQTT topic.

## Project Structure

```text
include/
  app_config.h
  application.h

lib/
  bm280/
    bm280.cpp
    bm280.h
  credentials/
    credentials.h
  mqtt/
    mqtt.cpp
    mqtt.h
  wifi/
    wifi.cpp
    wifi.h

src/
  application.cpp
  CMakeLists.txt
  main.cpp

CMakeLists.txt
platformio.ini
sdkconfig.esp32-s3-devkitc-1
```

## Notes

- MQTT publish calls use QoS 1 and retain enabled, so the last value remains available in the broker.
- Verified with MQTTX: subscribing to `module59_11/#` shows live telemetry updates correctly.

## Contact

Feedback: max.savin3@gmail.com