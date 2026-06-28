# Technical Report

## Abstract

This project implements a simple wireless gate monitoring system using two ESP32 boards and ESP-NOW communication. The ESP32-C3 transmitter reads a reed switch, measures battery voltage, and sends status packets. The ESP32-S3 receiver listens for these packets, updates the remote gate state, and reports the current status through logs.

The system was designed for low-power battery operation. The transmitter uses deep sleep, GPIO wakeup, timer wakeup fallback, and periodic battery reporting. The software is structured into four layers: Peripheral Layer, Logic Layer, Transport / Output Layer, and Reliability Layer. This makes the design easier to understand, test, and maintain.

Testing confirmed correct gate state detection, correct wireless transmission, and correct battery measurement after fixing the physical ADC wiring. The final result is a compact embedded system suitable for simple remote monitoring applications.

## Problem Statement

The goal of this project was to create a battery-powered embedded system that can detect whether a gate or door is open or closed and send this information wirelessly to another ESP32 board. The system also had to report battery voltage and support low-power operation.

The transmitter had to read a reed sensor, detect state changes, send updates through ESP-NOW, and spend most of its time in deep sleep to reduce power consumption. The receiver had to listen for incoming packets, track the last known state, and detect when communication becomes stale.

The solution had to remain simple, practical, and easy to test on real hardware.

## Review of Existing Solutions

A basic way to monitor a gate or door is to connect a reed switch directly to a wired alarm or controller input. This is reliable but requires cabling.

Wireless systems often use Wi-Fi, Bluetooth Low Energy, Zigbee, LoRa, or MQTT-based architectures. These solutions can be powerful, but they often require more infrastructure, more software complexity, or more power.

ESP-NOW is a lightweight wireless protocol supported by ESP32 devices. It does not require a router or access point and is well suited for simple ESP32-to-ESP32 communication. For this reason, it was selected as the communication method for this project.

Battery voltage measurement is commonly implemented with a resistor divider connected to an ADC pin. This project also uses that approach.

## Description of the Implemented System

The system consists of two nodes:

- **ESP32-C3 transmitter**
- **ESP32-S3 receiver**

### ESP32-C3 transmitter
The transmitter is powered by a single 18650 Li-ion battery. It reads the reed switch on GPIO4, measures battery voltage on GPIO2 through a resistor divider, and drives a green LED on GPIO20. It sends messages over ESP-NOW.

The transmitter sends:
- `DOOR:OPEN`
- `DOOR:CLOSED`
- `BATT:X.XXV`

To reduce power consumption, it enters deep sleep after completing active tasks. It wakes on reed state change or by timer.

### ESP32-S3 receiver
The receiver listens for ESP-NOW packets. It parses incoming messages, updates the current door state, stores the latest battery value, and detects stale communication if no packet arrives for a timeout period.

### Software structure
The software is divided into four layers:

- **Peripheral Layer** — GPIO, ADC, ESP-NOW, wakeup setup
- **Logic Layer** — reed processing, state decision, scheduling
- **Transport / Output Layer** — packet transmission, packet reception, LED indication, logging
- **Reliability Layer** — ADC averaging, retries, timeout handling, deep sleep recovery

This layered structure improves readability and maintenance.

## Results of Testing

The system was tested with real hardware.

### Reed sensor
The reed switch correctly changed the transmitted state between open and closed. The receiver correctly updated its monitored state.

### Battery measurement
At first, battery readings were incorrect because GPIO2 had been soldered to the wrong source. After the hardware fix, the measured battery voltage matched the expected value and the battery reporting worked correctly.

### Wireless communication
ESP-NOW packet transmission between the ESP32-C3 and ESP32-S3 worked correctly. The receiver successfully parsed door-state and battery messages.

### Power behavior
The transmitter successfully entered deep sleep and woke again on GPIO or timer events. This confirmed the low-power behavior required for battery operation.

## Conclusions

This project successfully implemented a simple wireless gate monitoring system using ESP32 boards and ESP-NOW.

The final system can:
- detect gate state using a reed switch,
- measure battery voltage,
- transmit telemetry wirelessly,
- receive and monitor remote state,
- operate in low-power sleep mode.

An important lesson from the project was that hardware validation is as important as firmware development. The ADC issue was caused by incorrect physical wiring, not by software logic. After fixing the hardware, the system worked as intended.

The project provides a solid foundation for future improvements such as stronger power optimization, additional status reporting, or integration with a larger monitoring system.

## Credits

### Frameworks and libraries
- **ESP-IDF** — Espressif IoT Development Framework
  License: Apache License 2.0
- **PlatformIO** — build and upload environment

### Documentation
- **Espressif Documentation**
  - ESP-NOW API
  - ADC configuration and calibration
  - GPIO configuration
  - Deep sleep and wakeup modes
- **Seeed Studio XIAO ESP32C3 documentation**
- **ESP32-S3-DevKitC-1 documentation**

### Hardware / PCB references
- **XIAO ESP32-C3 KiCad footprint/library**
  Source: VectorSpaceHQ GitHub repository
  License: verify repository license before final submission