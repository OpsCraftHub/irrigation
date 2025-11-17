# GEMINI.md

## Project Overview

This project is a comprehensive, single-channel irrigation controller built on the ESP32 platform. It is designed to be a standalone device capable of running scheduled irrigation cycles even without an internet connection. The controller features a 20x4 LCD display and physical buttons for local configuration and monitoring.

The project is built using PlatformIO and the Arduino framework. It is written in C++ and is well-structured with a modular design, separating concerns into distinct classes for irrigation logic, display management, WiFi connectivity, and Home Assistant integration.

The controller integrates tightly with Home Assistant via MQTT, supporting auto-discovery of entities for seamless remote control and monitoring. It also features Over-the-Air (OTA) firmware updates from a GitHub repository, allowing for easy maintenance and upgrades.

## Building and Running

### Prerequisites

*   **PlatformIO:** You need to have PlatformIO Core installed. You can install it via pip:
    ```bash
    pip install platformio
    ```
    Alternatively, you can use the PlatformIO IDE extension for VSCode.

### Configuration

1.  **Clone the repository:**
    ```bash
    git clone <repository-url>
    cd irrigation-controller
    ```
2.  **Edit `include/Config.h`:** This file contains all the main configuration settings for the project. You will need to update the following sections:
    *   WiFi credentials (`WIFI_SSID` and `WIFI_PASSWORD`)
    *   MQTT broker details (`MQTT_BROKER`, `MQTT_USER`, `MQTT_PASSWORD`)
    *   GitHub repository for OTA updates (`GITHUB_REPO_OWNER`, `GITHUB_REPO_NAME`)

### Build and Upload

1.  **Upload the filesystem:** The project uses SPIFFS to store configuration files. You need to upload the filesystem image before the first run.
    ```bash
    pio run --target uploadfs
    ```
2.  **Compile and upload the firmware:**
    ```bash
    pio run --target upload
    ```
3.  **Monitor the device:** You can view the serial output from the device using the following command:
    ```bash
    pio device monitor
    ```

## Development Conventions

*   **Modular Architecture:** The codebase is organized into a set of classes, each with a specific responsibility. This makes the code easier to understand, maintain, and extend.
    *   `IrrigationController`: Core irrigation logic.
    *   `DisplayManager`: Manages the LCD and button inputs.
    *   `WiFiManager`: Handles WiFi, NTP, and OTA updates.
    *   `HomeAssistantIntegration`: Manages MQTT communication with Home Assistant.
*   **Configuration:** All major configuration settings are centralized in the `include/Config.h` file. This makes it easy to adapt the project to different hardware setups and environments.
*   **Home Assistant Integration:** The project follows Home Assistant's MQTT discovery protocol, allowing for zero-configuration integration. The `home-assistant/irrigation.yaml` file provides a complete set of configurations for creating a rich user experience in Home Assistant.
*   **Documentation:** The project is extensively documented, with a detailed `README.md`, a `PROJECT_SUMMARY.md`, and other supporting documents. This makes it easy for new developers to get started with the project.
