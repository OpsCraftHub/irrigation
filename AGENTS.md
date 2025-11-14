# Repository Guidelines

## Project Structure & Module Organization
Firmware logic sits in `src/` with shared headers and configuration in `include/`—start hardware or feature changes in `include/Config.h`. Drop reusable helpers into `lib/`, keep SPIFFS payloads in `data/`, and version OTA artifacts under `firmware/`. Home Assistant examples stay in `home-assistant/`, and the narrative docs (`README.md`, `QUICKSTART.md`, `TECHNICAL.md`, `MULTI_CHANNEL_IMPLEMENTATION_PLAN.md`) must be updated whenever behavior or pin assignments move.

## Build, Flash, and Development Commands
Use PlatformIO end-to-end:
- `pio run` — compile `env:esp32dev`.
- `pio run --target upload` — flash firmware over USB.
- `pio run --target uploadfs` — upload SPIFFS assets from `data/`.
- `pio device monitor` or `python monitor_serial.py` — view logs at 115200 baud.
Treat `platformio.ini` as the single source for board, upload, and dependency settings.

## Coding Style & Naming Conventions
Follow Arduino-style C++: 4-space indentation, same-line braces, and `DEBUG_*` macros instead of raw `Serial`. Classes stay PascalCase (`WiFiManager`), functions camelCase, and constants/macros SCREAMING_SNAKE_CASE. Keep loops non-blocking so the watchdog remains happy, and update `Config.h` comments whenever pin maps or defaults move.

## Testing Guidelines
No automated suite exists yet—add PlatformIO Unity tests under `test/` whenever you touch logic and run them with `pio test -e esp32dev`. Hardware validation remains mandatory: flash the board, exercise LCD menus, trigger manual irrigation, and include serial logs showing WiFi/MQTT state transitions. When modifying `home-assistant/irrigation.yaml`, load the YAML into a HA dev instance and document the entities affected.

## Commit & Pull Request Guidelines
Git history favors short, imperative messages (e.g., `Implement multi-channel irrigation controller`) and tags releases as `Release vX.Y.Z [skip ci]`; follow that template and reference related docs or issues inline. Pull requests should supply a purpose summary, testing evidence (commands plus serial snippets or screenshots), configuration impacts (`data/`, `home-assistant/`), and migration notes for OTA users. Highlight breaking changes prominently and link the doc sections you refreshed.

## Security & Configuration Tips
Keep WiFi, MQTT, and OTA secrets out of Git; rely on placeholders in `Config.h` and untracked SPIFFS files for real values. Ensure `GITHUB_REPO_*` paths align with binaries in `firmware/`, and review firewall/certificate settings before enabling OTA or MQTT. Rotate credentials whenever logs show repeated failed connections.
