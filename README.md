# Gate Reed Control 2.4GHz

## Overview

Dual-board ESP32 project for gate door-state telemetry over ESP-NOW.

- Xiao ESP32-C3 acts as the transmitter
- ESP32-S3 acts as the receiver with Telegram bot push notifications
- Framework: ESP-IDF via PlatformIO
- Transport: ESP-NOW (2.4 GHz) + HTTPS (Telegram Bot API)

Each firmware keeps `src/main.cpp` minimal and places runtime logic in `src/application.cpp`.

## Demo

[![Watch the demo](Media/Gate_reed.jpg)](https://github.com/Maksym-UA/Gate_reed_control_2.4ghz/releases/download/v1.0.1/Reed_operation.mp4)

[Video walkthrough](https://github.com/Maksym-UA/Gate_reed_control_2.4ghz/releases/download/v1.0.1/Reed_operation.mp4)

_Fallback (repo file): [Media/Reed_operation.mp4](Media/Reed_operation.mp4)_

## Hardware

| Board | Role | Key GPIO |
|---|---|---|
| ESP32-C3 (Seeed XIAO ESP32C3) | Reed sensor reader + ESP-NOW TX + deep sleep | Reed: GPIO4, LED: GPIO20 |
| ESP32-S3 (ESP32-S3-DevKitC-1) | ESP-NOW RX + WiFi STA + Telegram bot | No sensor GPIO; WiFi used for Telegram HTTPS |

### Wiring Notes

- Use an external 100k pull-up resistor on ESP32-C3 GPIO4 for the reed input line for more efficient power management.
- Add an MTS-101 switch on the battery path so battery power can be disconnected while the ESP32-C3 is powered from USB.

## Communication Model

ESP32-C3 publishes gate state messages:

- `DOOR:OPEN`
- `DOOR:CLOSED`
- `BATT:X.XXV` (periodic battery voltage, e.g. `BATT:3.92V`)

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

## Telegram Bot Setup

The ESP32-S3 connects to a Telegram bot to deliver door-state push notifications and respond to admin commands.

### Credentials

Credentials are stored in `ESP32-S3/lib/esp_now/telegram/secrets.h`, which is gitignored.

1. Copy the template:
   ```bash
   cp ESP32-S3/lib/esp_now/telegram/secrets.h.template \
      ESP32-S3/lib/esp_now/telegram/secrets.h
   ```
2. Fill in the values:
   ```c
   #define WIFI_SSID      "your_wifi_ssid"
   #define WIFI_PASSWORD  "your_wifi_password"
   #define TG_BOT_TOKEN   "123456:ABC-..."
   #define TG_CHAT_ID     "123456789"   // chat to receive notifications
   #define TG_ADMIN_ID    "123456789"   // user ID allowed to run admin commands
   ```
3. Create a bot with [@BotFather](https://t.me/BotFather) and copy the token.
4. Retrieve your chat/user ID via [@userinfobot](https://t.me/userinfobot) or `getUpdates`.

### Notifications

| Event | Telegram message |
|---|---|
| Door opened | 🚪 Двері ВІДЧИНЕНІ |
| Door closed | ✅ Двері ЗАЧИНЕНІ |
| Door still open (every 5 min) | ⚠️ Двері все ще ВІДЧИНЕНІ: Xхв Yс |

### Admin Commands

| Command / Button | Response |
|---|---|
| `/voltage` text message | Battery voltage or "no data yet" |
| `Check Voltage` inline button | Same as `/voltage` |

Only `TG_ADMIN_ID` receives voltage data; all others get an unauthorized reply.

### Transport

The bot uses **long-polling** (`getUpdates` with `timeout=25`). No webhook or public IP is required. All HTTPS calls run inside a dedicated `tg_task` FreeRTOS task so the main door-state loop never blocks on network I/O.

## Runtime Flow

ESP32-C3:

1. Initializes ESP-NOW, reed input, LED, and ADC voltage divider.
2. Reads current door state and transmits initial state (`BOOT` TX kind on first cold boot).
3. Blinks LED 3 times for OPEN, 1 time for CLOSED on state change.
4. On timer wake with unchanged state, transmits `KEEPALIVE` once the periodic keep-alive threshold is reached.
5. On timer wake, spawns a one-shot voltage task that reads the battery ADC and broadcasts `BATT:X.XXV` when the battery TX interval threshold is reached.
6. Waits in boot grace window (15 s) on cold boot to allow monitoring/flashing.
7. On wake from GPIO/timer, stays active briefly (3.5 s) to catch quick follow-up transitions.
8. Enters deep sleep; wakes on reed pin level change (GPIO wakeup) with a timer fallback wake.

ESP32-S3:

1. Initializes ESP-NOW, connects to WiFi, and starts the Telegram bot client.
2. Receives `DOOR:OPEN` / `DOOR:CLOSED` / `BATT:X.XXV` packets via ESP-NOW.
3. Tracks known/unknown remote door state; marks it stale after a timeout without packets.
4. Enqueues Telegram notifications: door opened, door closed, and a repeat reminder every 5 minutes while the door remains open.
5. Runs a dedicated `tg_task` (FreeRTOS) for all HTTPS operations — the main loop never blocks on network I/O.
6. Long-polls Telegram (25 s server timeout) for admin commands: `/voltage` text message and `CHECK_VOLTAGE` inline button.
7. Logs a periodic heartbeat with the current door state.

## System Architecture

This project follows a layered architecture across two boards:

- **ESP32-C3 transmitter** — reads the reed sensor, measures battery voltage, drives the status LED, and sends ESP-NOW packets.
- **ESP32-S3 receiver** — receives ESP-NOW packets, tracks remote state, and reports status through logs.

### GPIO Map

| Board | GPIO / Resource | Function |
|---|---|---|
| ESP32-C3 (Seeed XIAO ESP32C3) | GPIO4 | Reed sensor input |
| ESP32-C3 (Seeed XIAO ESP32C3) | GPIO2 | Battery voltage ADC input |
| ESP32-C3 (Seeed XIAO ESP32C3) | GPIO20 | Green LED output |
| ESP32-C3 (Seeed XIAO ESP32C3) | ESP-NOW / Wi-Fi | Wireless transmission |
| ESP32-C3 (Seeed XIAO ESP32C3) | Deep sleep wakeup | Wake by GPIO or timer |
| ESP32-S3 (ESP32-S3-DevKitC-1) | ESP-NOW / Wi-Fi | Wireless reception |
| ESP32-S3 (ESP32-S3-DevKitC-1) | UART | Debug and monitoring output |

### Peripheral List

ESP32-C3 transmitter:
- Reed switch sensor
- External 100k pull-up resistor on GPIO4
- RC input filter: 100Ω + 100nF
- Battery voltage divider on GPIO2: 220k / 220k
- ADC filter capacitor: 100nF
- Green LED with 470Ω resistor
- 18650 Li-ion battery
- Power switch
- ESP-NOW wireless interface

ESP32-S3 receiver:
- ESP-NOW wireless interface (2.4 GHz)
- WiFi STA (WPA2, used for Telegram HTTPS)
- UART logging interface

### Layer Model

#### Peripheral Layer
Handles low-level hardware setup.

ESP32-C3:
- Reed GPIO initialization
- LED GPIO initialization
- ADC initialization for battery sensing
- ESP-NOW initialization
- Deep sleep and wakeup source setup

ESP32-S3:
- ESP-NOW initialization
- UART/logger initialization

#### Logic Layer
Implements the application behavior.

ESP32-C3:
- Reed state reading
- Debounce/filtering
- Door state evaluation
- Keepalive scheduling
- Battery reporting scheduling
- Wake reason handling
- Sleep decision logic

ESP32-S3:
- Packet parsing
- Door state tracking
- Battery status update
- Timeout and stale-state detection

#### Transport / Output Layer
Handles communication and visible output.

ESP32-C3:
- ESP-NOW transmission of:
  - `DOOR:OPEN`
  - `DOOR:CLOSED`
  - `BATT:X.XXV`
- LED blink indication

ESP32-S3:
- ESP-NOW packet reception
- WiFi STA connection (WPA2)
- Telegram push notifications via HTTPS (`sendMessage`)
- Telegram long-poll command handler (`/voltage`, `CHECK_VOLTAGE` inline button)
- UART log output

#### Reliability Layer
Improves robustness and fault tolerance.

ESP32-C3:
- ADC averaging
- ADC settle delay before sampling
- ESP-NOW retry handling
- Wait for TX completion before sleep
- Deep sleep power saving
- GPIO/timer wakeup recovery

ESP32-S3:
- Timeout detection
- Stale-state handling
- Recovery after missed packets

### High-Level Architecture Diagram

```mermaid
flowchart LR
    subgraph TX["ESP32-C3 Transmitter"]
        RS["Reed Sensor"]
        BAT["18650 Battery"]
        SW["Power Switch"]
        LED["Green LED"]

        subgraph P1["Peripheral Layer"]
            GPIO["GPIO4 Reed Input"]
            ADC["GPIO2 ADC Battery Input"]
            LEDGPIO["GPIO20 LED Output"]
            ESPNOW1["ESP-NOW Init"]
            SLEEP1["Deep Sleep / Wakeup Init"]
        end

        subgraph L1["Logic Layer"]
            REEDLOGIC["Reed Read + Debounce"]
            FSM1["Door State FSM"]
            BATTLOGIC["Battery Read Scheduler"]
            KEEP["Keepalive Logic"]
        end

        subgraph O1["Transport / Output Layer"]
            TXMSG["ESP-NOW TX\nDOOR:OPEN / DOOR:CLOSED / BATT:X.XXV"]
            LEDPAT["LED Blink Patterns"]
        end

        subgraph R1["Reliability Layer"]
            AVG["ADC Averaging"]
            TXRETRY["TX Retry / Wait Complete"]
            WAKE["GPIO / Timer Wake Logic"]
        end

        RS --> GPIO
        BAT --> SW --> ADC
        BAT --> SW --> SLEEP1
        GPIO --> REEDLOGIC --> FSM1
        ADC --> BATTLOGIC
        FSM1 --> TXMSG
        KEEP --> TXMSG
        BATTLOGIC --> TXMSG
        FSM1 --> LEDPAT
        LEDPAT --> LEDGPIO --> LED

        ESPNOW1 --> TXMSG
        SLEEP1 --> WAKE
        AVG --> BATTLOGIC
        TXRETRY --> TXMSG
        WAKE --> FSM1
    end

    LINK["2.4 GHz ESP-NOW Link"]

    subgraph RX["ESP32-S3 Receiver"]
        subgraph P2["Peripheral Layer"]
            ESPNOW2["ESP-NOW Init"]
            WIFI["WiFi STA Init"]
            UART["UART / Serial Log Init"]
        end

        subgraph L2["Logic Layer"]
            PARSE["Packet Parser"]
            FSM2["Receiver State FSM"]
            STALE["Timeout / Stale Detection"]
            BATT2["Battery Status Update"]
            TG_QUEUE["Telegram Queue"]
            TG_TASK["tg_task\n(FreeRTOS)"]
        end

        subgraph O2["Transport / Output Layer"]
            LOG["UART Logs / Monitoring"]
            TG["Telegram Bot API\n(HTTPS long-poll + POST)"]
        end

        subgraph R2["Reliability Layer"]
            REC["Recovery / Timeout Handling"]
        end

        ESPNOW2 --> PARSE
        WIFI --> TG_TASK
        PARSE --> FSM2
        PARSE --> BATT2
        FSM2 --> TG_QUEUE
        BATT2 --> LOG
        TG_QUEUE --> TG_TASK
        TG_TASK --> TG
        FSM2 --> LOG
        STALE --> FSM2
        REC --> STALE
        UART --> LOG
    end

    TXMSG --> LINK --> ESPNOW2
```

### Transmitter Runtime FSM

```mermaid
stateDiagram-v2
    [*] --> BOOT
    BOOT --> INIT
    INIT --> CHECK_REED

    CHECK_REED --> SEND_EVENT : door changed
    CHECK_REED --> SEND_KEEPALIVE : keepalive due
    CHECK_REED --> READ_BATTERY : battery report due
    CHECK_REED --> PREPARE_SLEEP : no update needed

    SEND_EVENT --> BLINK_LED
    BLINK_LED --> WAIT_TX_DONE
    SEND_KEEPALIVE --> WAIT_TX_DONE
    READ_BATTERY --> SEND_BATTERY
    SEND_BATTERY --> WAIT_TX_DONE

    WAIT_TX_DONE --> PREPARE_SLEEP
    PREPARE_SLEEP --> DEEP_SLEEP
    DEEP_SLEEP --> WAKE

    WAKE --> CHECK_REED
```

### Receiver Runtime FSM

```mermaid
stateDiagram-v2
    [*] --> INIT
    INIT --> LISTENING

    LISTENING --> PARSE_PACKET : packet received
    PARSE_PACKET --> STATE_OPEN : DOOR:OPEN
    PARSE_PACKET --> STATE_CLOSED : DOOR:CLOSED
    PARSE_PACKET --> UPDATE_BATTERY : BATT:X.XXV

    STATE_OPEN --> ENQUEUE_NOTIFY : state changed
    STATE_OPEN --> LISTENING : keepalive (no change)
    STATE_CLOSED --> ENQUEUE_NOTIFY : state changed
    STATE_CLOSED --> LISTENING : keepalive (no change)
    ENQUEUE_NOTIFY --> LISTENING
    UPDATE_BATTERY --> LISTENING

    LISTENING --> ENQUEUE_REMIND : door open ≥ 5 min
    ENQUEUE_REMIND --> LISTENING

    LISTENING --> STATE_STALE : packet timeout
    STATE_STALE --> LISTENING : next valid packet
```

### Queue and Mutex Notes

- A **queue** is used when one task sends commands to another task.
- In this project, the LED control path is a queue-based interaction between application logic and the LED task.
- A **mutex** is needed when multiple tasks access the same shared resource.
- If multiple tasks transmit through the same ESP-NOW path, a mutex may be used to protect the shared transmission resource.

## Project Structure

```text
ESP32-C3/
  include/
    application.h
  lib/
    esp_now/
    led/
    reed/
    voltage/
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
      esp_now.cpp
      espnow_service.h
      telegram/
        telegram.cpp
        telegram.h
        secrets.h          ← gitignored; copy from secrets.h.template
        secrets.h.template
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
- `ESP32-S3/lib/esp_now/telegram/secrets.h` is gitignored. Copy `secrets.h.template` to `secrets.h` and fill in your credentials before building the S3 firmware.
- KiCAd Xiao ESP32-C3 footprint from https://github.com/VectorSpaceHQ/XIAO_ESP32C3
- Seeed Xiao ESP32-C3 Wiki https://wiki.seeedstudio.com/XIAO_ESP32C3_Getting_Started

## Contact

Feedback: max.savin3@gmail.com
