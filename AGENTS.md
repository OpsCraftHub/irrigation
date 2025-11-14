# Repository Guidelines

## Project Layout
- `src/` holds the controllers, display logic, WiFi/Home Assistant glue; update hardware defaults and OTA paths in `include/Config.h` first.
- Drop shared helpers into `lib/`, keep SPIFFS payloads (config, secrets stubs) in `data/`, stash OTA binaries under `firmware/`, and store HA automations in `home-assistant/`.
- When behavior or wiring changes, refresh `README.md`, `QUICKSTART.md`, `TECHNICAL.md`, and `MULTI_CHANNEL_IMPLEMENTATION_PLAN.md` so builders stay aligned.

## Build & Flash
- `pio run` — compile `env:esp32dev` locally.
- `pio run --target upload` — flash firmware to the attached ESP32.
- `pio run --target uploadfs` — push `data/` into SPIFFS for runtime config.
- `pio device monitor` or `python monitor_serial.py` — follow logs at 115200 baud.
Treat `platformio.ini` as the single source of truth for board, upload, and dependency tweaks.

## Web API Endpoints
All endpoints hang off the status UI host (default `http://irrigation-esp32.local`). Replace the host/IP as needed.
- `GET /api/schedules` — list channels, pins, and enabled slots.
  ```bash
  curl http://irrigation-esp32.local/api/schedules | jq
  ```
- `POST /api/schedules` — add a schedule (weekdays default to every day).
  ```bash
  curl -X POST http://irrigation-esp32.local/api/schedules \
    -H 'Content-Type: application/json' \
    -d '{"channel":1,"hour":6,"minute":0,"duration":20}'
  ```
- `DELETE /api/schedules?id=<slot>` — remove by index returned from GET.
  ```bash
  curl -X DELETE "http://irrigation-esp32.local/api/schedules?id=2"
  ```
- `POST /system/check-updates` — trigger OTA check (uses public GitHub repo defined in `Config.h`).
  ```bash
  curl -X POST http://irrigation-esp32.local/system/check-updates
  ```
- `POST /mqtt/save|test|remove`, `POST /wifi/remove`, `POST /system/restart` follow the same pattern for automation scripts.

## Coding & Testing Expectations
- C++ files use 4-space indentation, same-line braces, PascalCase class names, camelCase methods, and SCREAMING_SNAKE macros; keep loops non-blocking and log through `DEBUG_*` macros.
- No unit suite is committed yet—add PlatformIO Unity tests under `test/` for new logic (`pio test -e esp32dev`) and always validate on hardware (LCD navigation, manual irrigation, WiFi/MQTT transitions recorded from serial).

## Commits, PRs & Configuration Hygiene
- Follow the existing Git history style: short imperative messages (`Add schedule API`) and release tags like `Release v1.0.6 [skip ci]`.
- PRs should list the reason for change, commands/logs/screenshots proving it works, and mention any files in `data/`, `home-assistant/`, or `firmware/` that changed.
- Never commit real WiFi/MQTT/OTA secrets; keep placeholders in Git and load actual values through untracked SPIFFS files. Verify `GITHUB_REPO_*` fields match the binaries you publish so OTA users stay on the public update track.
