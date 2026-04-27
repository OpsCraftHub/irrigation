# Audit Feedback & Recommended Upgrades

## Note on the Original Audit

The audit in `audit.md` is directionally correct but written from a **desktop/enterprise C++ perspective**, not an embedded one. Recommendations like event buses, full hardware abstraction layers, and persistent flash logging make sense for server-side or desktop applications with gigabytes of RAM and no flash-wear concerns. On an ESP32 with 520KB RAM, 4MB flash, and a 2-board product line, the cost/benefit calculus is different. About half the recommendations apply well; the other half would add complexity without proportional benefit.

This document identifies the upgrades worth doing, in priority order, with embedded constraints in mind.

---

## Factual Corrections to the Audit

Before acting on recommendations, these inaccuracies should be noted:

| Audit Claim | Reality |
|---|---|
| OTA uses HTTP, should move to HTTPS | Already uses HTTPS (`https://raw.githubusercontent.com/...`) |
| SystemStatus is a "God Structure" | ~100-130 bytes, focused on irrigation state only. No methods, no business logic. Appropriate data container. |
| Board-specific `#ifdef` macros will become unmanageable | Only 5 total `#ifdef`s in the entire codebase. Firmware is nearly 100% unified between Board A and B. |

---

## Recommended Upgrades (Priority Order)

### 1. Migrate SPIFFS to LittleFS [HIGH]

**Why:** SPIFFS is deprecated in ESP-IDF and has poor power-loss resilience. A write interrupted by power loss can corrupt the entire partition. LittleFS is a drop-in replacement with journaling that handles this gracefully.

**Effort:** Low. Change `board_build.filesystem = littlefs` in `platformio.ini`, replace `#include <SPIFFS.h>` with `#include <LittleFS.h>`, and find-replace `SPIFFS.` with `LittleFS.` across source files. The API is identical.

**Risk:** Users with existing devices need a one-time SPIFFS-to-LittleFS migration (reformat + re-upload config). OTA cannot change the filesystem type — requires USB reflash.

---

### 2. Constructor-Based Dependency Injection [HIGH]

**Why:** The current setter pattern (`setController()`, `setHomeAssistant()`, `setNodeManager()`) is fragile. Forgetting a setter call compiles fine but crashes at runtime with a null pointer. Constructor injection makes dependencies explicit and catches missing wiring at compile time.

**What changes:**
- Each component declares its dependencies as constructor parameters
- `main.cpp` constructs objects in dependency order
- Remove all `setX()` methods and the associated null checks scattered through the code

**Example:**
```cpp
// Before (fragile)
auto* ha = new HomeAssistantIntegration();
ha->setController(controller);  // forget this and get a null deref

// After (compile-time safe)
auto* ha = new HomeAssistantIntegration(controller);
```

---

### 3. Polymorphic Valve Interface [MEDIUM]

**Why:** `IrrigationController::activateValve()` uses an `if (idx < NUM_LOCAL_CHANNELS)` branch to decide between GPIO and remote UDP control. This works for 2 paths but becomes unwieldy if a third valve type appears (e.g., I2C relay board, RS-485 actuator).

**What changes:**
```cpp
class Valve {
public:
    virtual void activate(bool state, uint16_t duration) = 0;
    virtual bool isActive() = 0;
    virtual ~Valve() = default;
};

class LocalValve : public Valve { /* digitalWrite */ };
class RemoteValve : public Valve { /* NodeManager callback */ };
```

The controller holds a `Valve* channels[MAX_CHANNELS]` array and calls `channels[idx]->activate()` without knowing the implementation. The existing `RemoteValveCallback` function pointer approach is already halfway there — this formalises it.

**Embedded note:** One vtable pointer (4 bytes) per valve object. Negligible overhead for 6-8 channels.

---

### 4. Extract Web API from WiFiManager [MEDIUM]

**Why:** WiFiManager currently contains ~180 lines of direct `IrrigationController` method calls inside web API route handlers (`/api/schedules`, `/api/channels`, etc.). This makes WiFiManager responsible for both network connectivity AND irrigation business logic orchestration — two unrelated concerns.

**What changes:**
- Extract web API handlers into a separate `WebAPIHandler` class
- WebAPIHandler receives IrrigationController and HomeAssistantIntegration via constructor
- WiFiManager only handles WiFi connection, reconnection, NTP sync, and OTA
- WiFiManager exposes `WebServer*` for WebAPIHandler to register routes on

This is the coupling problem the audit identified but didn't pinpoint.

---

### 5. Flash Encryption for Credentials [LOW]

**Why:** WiFi and MQTT credentials are stored as plaintext JSON on the filesystem. Anyone with physical access and a USB cable can dump them.

**What the audit got wrong:** It suggested ESP32 NVS as a solution. NVS is not encrypted by default — it offers the same physical security as SPIFFS/LittleFS. The real fix is ESP-IDF flash encryption, which encrypts the entire flash contents with a per-device key burned into eFuses.

**Trade-off:** Flash encryption is a one-way operation (eFuses are permanent), complicates development/debugging, and requires secure boot to be meaningful. For a home irrigation controller, this is likely overkill unless the device is deployed in untrusted environments.

**Pragmatic alternative:** Accept the risk for now. Document it as a known limitation. If needed later, enable flash encryption as a production build step.

---

## Recommendations from the Audit to Skip

### Event Bus / Observer Pattern — Skip

The audit suggests decoupling components via an event bus. On an embedded device this adds heap allocation for event queues, function pointer indirection, and makes control flow harder to trace with a debugger or serial log. The current direct-call pattern is predictable, zero-overhead, and easy to follow. The real fix for coupling is the Web API extraction above, not an architectural pattern swap.

### Full Hardware Abstraction Layer — Skip

With only 5 `#ifdef`s across the codebase for 2 board variants, a formal HAL is premature. Pin maps are inherently hardware-bound and belong at compile time. The audit's suggestion to make the same binary run on different boards by reading a "hardware profile" from flash is impractical — pin assignments, peripheral availability, and boot behaviour differ fundamentally between ESP32 and ESP32-C3.

### Persistent Log Buffer to Flash — Skip

This directly contradicts the project's flash-write strategy (writes only on explicit user action). Continuous logging to flash wears the memory and risks corruption. Better alternatives that already exist or are trivial to add:
- Serial/UART output (already implemented via `DEBUG_PRINTF`)
- MQTT log topic (publish to `irrigation/log` for Home Assistant to capture)
- RAM circular buffer exposed via web API endpoint (zero flash writes)

### State-Machine UI Refactor — Skip

The current display code works and the menu is small. A state-machine UI framework is warranted only if the menu grows significantly. Not worth the refactor cost today.

---

## Summary

| Upgrade | Priority | Effort | Benefit |
|---|---|---|---|
| SPIFFS to LittleFS | High | Low | Power-loss resilience, future-proofing |
| Constructor DI | High | Low | Compile-time safety, eliminates null derefs |
| Polymorphic Valve | Medium | Medium | Cleaner channel abstraction, extensibility |
| Extract Web API | Medium | Medium | Fixes the real coupling problem |
| Flash encryption | Low | High | Credential security (overkill for home use) |
| Event bus | Skip | — | Over-engineering for embedded |
| Full HAL | Skip | — | Premature for 2 boards with 5 ifdefs |
| Persistent logging | Skip | — | Violates flash-write strategy |
| UI state machine | Skip | — | Current menu is small enough |
