# Gate Reed Control 2.4GHz

## Overview

Dual-board ESP32 project for gate door-state telemetry over ESP-NOW.

- ESP32-C3 acts as the transmitter
- ESP32-S3 acts as the receiver
- Framework: ESP-IDF via PlatformIO
- Transport: ESP-NOW (2.4GHz)

Each firmware keeps `src/main.cpp` minimal and places runtime logic in `src/application.cpp`.

## Hardware

| Board | Role | Key GPIO |
|---|---|---|
| ESP32-C3 (Seeed XIAO ESP32C3) | Reed sensor reader + ESP-NOW TX + deep sleep | Reed: GPIO4, LED: GPIO20 |
| ESP32-S3 (ESP32-S3-DevKitC-1) | ESP-NOW RX + state monitoring | No required sensor GPIO in current receiver build |

### Wiring Notes

- Use an external 100k pull-up resistor on ESP32-C3 GPIO4 for the reed input line for more efficient power management.
- Add an MTS-101 switch on the TP4056 Type-C battery path so battery power can be disconnected while the ESP32-C3 is powered from USB.

## Communication Model

ESP32-C3 publishes gate state messages:

- `DOOR:OPEN`
- `DOOR:CLOSED`

ESP32-S3 listens on ESP-NOW, updates remote door state, and marks state as stale if no packet arrives for a timeout window.

## Build, Upload, Monitor

ESP32-C3:

```bash
cd ESP32-C3
pio run
pio run -t upload
pio device monitor -b 115200
```

ESP32-S3:

```bash
cd ESP32-S3
pio run
pio run -t upload
pio device monitor -b 115200
```

## Runtime Flow

ESP32-C3:

1. Initializes ESP-NOW, reed input, and LED.
2. Reads current door state and transmits initial state.
3. Blinks LED 3 times when state is OPEN.
4. Waits in boot grace window to allow monitoring/flashing.
5. On wake from GPIO/timer, stays active briefly to catch quick follow-up transitions.
6. Enters deep sleep, wakes on reed pin level change, and also uses a timer fallback wake.

ESP32-S3:

1. Initializes ESP-NOW in receiver mode.
2. Receives `DOOR:OPEN` / `DOOR:CLOSED` packets.
3. Tracks known/unknown remote door state.
4. Logs periodic heartbeat with current state.
5. Marks door state stale after timeout without packets.
6. Emits periodic push notification logs while door remains OPEN.

## Project Structure

```text
ESP32-C3/
  include/
    application.h
  lib/
    esp_now/
    led/
    reed/
  src/
    application.cpp
    main.cpp
  CMakeLists.txt
  platformio.ini

ESP32-S3/
  include/
    application.h
  lib/
    esp_now/
  src/
    application.cpp
    main.cpp
  CMakeLists.txt
  platformio.ini

Gate_reed_control_2.4ghz.code-workspace
.gitignore
README.md
src/

```

## Notes

- ESP-NOW is connectionless and best-effort; RF environment affects reliability.
- Deep sleep on ESP32-C3 may make USB serial disappear until the next wake/reset.
- If VS Code shows stale Problems after successful builds, run a clean build and reload the window.

## Contact

Feedback: max.savin3@gmail.com
