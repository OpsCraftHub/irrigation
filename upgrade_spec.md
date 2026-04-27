# Upgrade Sprint Spec

Four improvements targeting reliability, maintainability, and extensibility.

---

## 1. SPIFFS to LittleFS Migration

### Scope

Replace all SPIFFS usage with LittleFS across the codebase. LittleFS has journaling that survives power loss mid-write — SPIFFS does not.

### Files to Change

**platformio.ini** (1 change):
- `board_build.filesystem = spiffs` → `board_build.filesystem = littlefs`

**Headers** — replace `#include <SPIFFS.h>` with `#include <LittleFS.h>`:
- `include/IrrigationController.h`
- `include/HomeAssistantIntegration.h`
- `include/WiFiManager.h`

**Source files** — find-replace `SPIFFS.` → `LittleFS.` and update include:
- `src/IrrigationController.cpp` (8 calls)
- `src/NodeManager.cpp` (6 calls)
- `src/HomeAssistantIntegration.cpp` (3 calls)
- `src/WiFiManager.cpp` (13 calls)
- `src/main.cpp` (6 calls)

### Notes

- LittleFS API is identical to SPIFFS — pure find-replace, no logic changes
- Existing devices require one-time USB reflash to reformat the partition (OTA cannot change filesystem type)
- `board_build.partitions = min_spiffs.csv` partition name stays the same — it refers to the partition layout, not the filesystem driver

---

## 2. Constructor-Based Dependency Injection

### Problem

Components are wired via setter methods after construction. Forgetting a setter compiles fine but crashes at runtime with a null pointer dereference. The current setup in `main.cpp` has 12 setter calls spread across 100 lines, with conditional branches making it easy to miss one.

### Scope

Move required dependencies into constructor parameters. Remove setter methods that exist only for DI wiring.

### Setters to Convert to Constructor Parameters

**WiFiManager:**
- `setController(IrrigationController*)` → constructor param
- `setHomeAssistant(HomeAssistantIntegration*)` → constructor param (nullable)
- `setNodeManager(NodeManager*)` → constructor param (nullable)

**HomeAssistantIntegration:**
- Constructor already takes `IrrigationController*` — no change needed
- `setNodeManager(NodeManager*)` → constructor param (nullable)

**NodeManager:**
- `setController(IrrigationController*)` → constructor param
- `setNodeId(const char*)` → constructor param
- `setRole(uint8_t)` → constructor param
- `setNodeName(const char*)` → constructor param

**DisplayManager:**
- Constructor already takes `IrrigationController*` — no change needed

### Setters to Keep (runtime behaviour, not DI wiring)

These are callbacks and runtime state, not construction-time dependencies:
- `setRemoteValveCallback()` — set after NodeManager exists
- `setPairRequestCallback()` — set after NodeManager exists
- `setPairResponseCallback()` — set after DisplayManager exists
- `setTimeUpdateCallback()` — set after WiFiManager exists
- `setManualMode()`, `setSystemEnabled()`, `setChannelInverted()`, `setChannelEnabled()` — runtime state
- `setRemoteChannelStatus()`, `setCurrentTime()` — runtime updates

### Construction Order in main.cpp

Order must respect dependencies (each object only depends on objects above it):

```
1. IrrigationController()                          — no deps
2. DisplayManager(irrigationController)             — needs 1
3. HomeAssistantIntegration(irrigationController)   — needs 1
4. NodeManager(irrigationController, nodeId, role, nodeName) — needs 1
5. WiFiManager(irrigationController, homeAssistant, nodeManager) — needs 1,3,4
```

Nullable params (homeAssistant, nodeManager) passed as `nullptr` when features are disabled.

---

## 3. Polymorphic Valve Interface

### Problem

`IrrigationController::activateValve()` uses `if (idx < NUM_LOCAL_CHANNELS)` to branch between GPIO control and remote UDP dispatch. Adding a third valve type (I2C relay, RS-485) means another branch in the same function.

### Scope

Define a `Valve` base class. Derive `LocalValve` (GPIO) and `RemoteValve` (NodeManager callback). The controller holds an array of `Valve*` and calls a uniform interface.

### New Files

**`include/Valve.h`:**
```cpp
class Valve {
public:
    virtual void activate(bool state, uint16_t duration) = 0;
    virtual bool isActive() const = 0;
    virtual ~Valve() = default;
};

class LocalValve : public Valve {
public:
    LocalValve(uint8_t pin, bool inverted = false);
    void activate(bool state, uint16_t duration) override;
    bool isActive() const override;
private:
    uint8_t _pin;
    bool _inverted;
    bool _active = false;
};

class RemoteValve : public Valve {
public:
    using Callback = void (*)(uint8_t channel, bool state, uint16_t duration);
    RemoteValve(uint8_t channel, Callback cb);
    void activate(bool state, uint16_t duration) override;
    bool isActive() const override;
private:
    uint8_t _channel;
    Callback _cb;
    bool _active = false;
};
```

### Changes to IrrigationController

- Add `Valve* _valves[MAX_CHANNELS]` array
- Populate in `begin()` — LocalValve for channels 0..NUM_LOCAL_CHANNELS-1, RemoteValve for the rest
- Replace the `activateValve()` if/else block with:
  ```cpp
  if (_valves[idx]) _valves[idx]->activate(state, duration);
  ```
- Remove `_remoteValveCallback` member — it moves into RemoteValve

### Overhead

One vtable pointer (4 bytes) per valve. 32 bytes total for 8 channels.

---

## 4. Extract Web API from WiFiManager

### Problem

WiFiManager contains 28 web route handlers that directly call IrrigationController, HomeAssistantIntegration, and NodeManager methods. WiFiManager is responsible for both network connectivity AND irrigation business logic — two unrelated concerns in one 2400+ line file.

### Scope

Move all `/api/*` route handlers into a new `WebAPIHandler` class. WiFiManager retains WiFi connection management, NTP sync, OTA updates, and the WiFi setup portal (`/`, `/scan`, `/save`).

### New Files

**`include/WebAPIHandler.h`** and **`src/WebAPIHandler.cpp`:**

```cpp
class WebAPIHandler {
public:
    WebAPIHandler(
        WebServer* server,
        IrrigationController* controller,
        HomeAssistantIntegration* ha,  // nullable
        NodeManager* nm               // nullable
    );
    void begin();  // registers all /api/* routes on the server
private:
    WebServer* _server;
    IrrigationController* _controller;
    HomeAssistantIntegration* _ha;
    NodeManager* _nm;

    void handleGetSchedules();
    void handlePostSchedule();
    void handleDeleteSchedule();
    void handleChannelStatus();
    void handleChannelStart();
    void handleChannelStop();
    // ... etc
};
```

### Routes That Move to WebAPIHandler (21 routes)

- `/api/schedules` (GET, POST, DELETE)
- `/api/channels/status` (GET)
- `/api/channels/available` (GET)
- `/api/channel/invert` (POST)
- `/api/channel/enable` (POST)
- `/api/channel/start` (POST)
- `/api/channel/stop` (POST)
- `/api/schedule/skip` (POST)
- `/api/schedule/unskip` (POST)
- `/api/nodes/pending` (GET)
- `/api/nodes/accept` (POST)
- `/api/nodes/reject` (POST)
- `/api/nodes/rename` (POST)
- `/api/nodes/unpair` (POST)
- `/api/config` (GET, POST)
- `/mqtt/save` (POST)
- `/mqtt/test` (POST)
- `/mqtt/remove` (POST)
- `/wifi/remove` (POST)
- `/system/restart` (POST)
- `/system/check-updates` (POST)

### Routes That Stay in WiFiManager (7 routes)

- `/` (GET) — WiFi setup portal and status page
- `/scan` (GET) — WiFi network scan
- `/save` (POST) — WiFi credential save
- Static file serving
- Captive portal redirects

### Wiring in main.cpp

```cpp
auto* webAPI = new WebAPIHandler(
    wifiManager->getWebServer(),
    irrigationController,
    homeAssistant,
    nodeManager
);
webAPI->begin();
```

WiFiManager needs one new method: `WebServer* getWebServer()` to expose the server instance.

---

## Sprint Checklist

- [ ] **1a.** Replace `SPIFFS` with `LittleFS` in platformio.ini
- [ ] **1b.** Update includes and method calls across 8 files
- [ ] **1c.** Build and verify both board targets compile
- [ ] **2a.** Add constructor parameters to WiFiManager, NodeManager, HomeAssistantIntegration
- [ ] **2b.** Remove replaced setter methods from headers
- [ ] **2c.** Update main.cpp construction order
- [ ] **2d.** Build and verify no null pointer issues
- [ ] **3a.** Create `include/Valve.h` with base class and two derived classes
- [ ] **3b.** Create `src/Valve.cpp` with implementations
- [ ] **3c.** Refactor `IrrigationController::activateValve()` to use Valve array
- [ ] **3d.** Remove `RemoteValveCallback` typedef and member from IrrigationController
- [ ] **4a.** Create `include/WebAPIHandler.h` and `src/WebAPIHandler.cpp`
- [ ] **4b.** Move 21 route handlers from WiFiManager to WebAPIHandler
- [ ] **4c.** Add `getWebServer()` to WiFiManager
- [ ] **4d.** Wire WebAPIHandler in main.cpp
- [ ] **4e.** Verify all web API endpoints still work
- [ ] **Final.** Build both board targets, flash Board A, smoke test
