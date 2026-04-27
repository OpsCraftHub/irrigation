# Class Design Spec — Polymorphic Valve & Web API Extraction

This document describes the class architecture after the v1.0.38 refactoring sprint:
LittleFS migration, constructor DI, polymorphic Valve interface, and Web API extraction.

---

## 1. Valve Abstraction (Polymorphic Interface)

### Problem

The original `IrrigationController::activateValve()` used an if/else chain:

```cpp
// BEFORE — procedural dispatch
void activateValve(uint8_t channel, bool state, bool manual) {
    if (channel <= NUM_LOCAL_CHANNELS) {
        digitalWrite(CHANNEL_PINS[channel-1], state ? HIGH : LOW);
    } else if (_remoteValveCallback) {
        _remoteValveCallback(channel, state, duration);
    }
}
```

Every time a new valve type is added (e.g. a battery-powered ESP-NOW valve), the
controller needs another `else if` branch. The controller also needs to know
implementation details about GPIO pins, inversion logic, and callback routing.

### Solution — Abstract Base Class

```
include/Valve.h
├── Valve           (abstract base — pure virtual interface)
├── LocalValve      (GPIO-driven MOSFET/relay)
└── RemoteValve     (UDP dispatch via callback to NodeManager)
```

**Valve** — the interface contract:
```cpp
class Valve {
public:
    virtual void activate(bool state, uint16_t durationMinutes) = 0;
    virtual bool isActive() const = 0;
    virtual ~Valve() = default;
};
```

Two methods. That's the entire contract. Any valve type that can be turned on/off
and report its state can plug into the system.

**LocalValve** — GPIO output to MOSFET gate:
```cpp
class LocalValve : public Valve {
    uint8_t _pin;       // GPIO number
    bool _inverted;     // Active-low flag (some relays are active-low)
    bool _active;       // Current state

    void activate(bool state, uint16_t durationMinutes) override {
        _active = state;
        bool pinState = _inverted ? !state : state;
        digitalWrite(_pin, pinState ? HIGH : LOW);
    }
};
```

Owns its pin number and inversion flag. Also exposes `getPin()`, `setInverted()`,
`isInverted()` for channel configuration.

**RemoteValve** — dispatches to NodeManager via callback:
```cpp
class RemoteValve : public Valve {
    uint8_t _channel;   // 1-based virtual channel number
    Callback _cb;       // void(*)(uint8_t, bool, uint16_t)
    bool _active;

    void activate(bool state, uint16_t durationMinutes) override {
        if (_cb) _cb(_channel, state, durationMinutes);
        _active = state;
    }
};
```

The callback is set later via `setCallback()` because RemoteValve objects are
created in `IrrigationController::begin()`, before NodeManager exists.

### How IrrigationController Uses It

```
IrrigationController
    _valves[MAX_CHANNELS]    ← array of Valve* pointers
    │
    ├── [0..NUM_LOCAL_CHANNELS-1]     → LocalValve instances
    └── [NUM_LOCAL_CHANNELS..MAX-1]   → RemoteValve instances
```

**Construction** (`begin()`):
```cpp
// Local channels: one LocalValve per GPIO pin
for (uint8_t i = 0; i < NUM_LOCAL_CHANNELS; i++) {
    _valves[i] = new LocalValve(CHANNEL_PINS[i], _status.channelInverted[i]);
}
// Virtual channels: RemoteValve placeholders (callback set later)
for (uint8_t i = NUM_LOCAL_CHANNELS; i < MAX_CHANNELS; i++) {
    _valves[i] = new RemoteValve(i + 1);
}
```

**Dispatch** (`activateValve()`) — now type-agnostic:
```cpp
void activateValve(uint8_t channel, bool state, bool manual) {
    uint8_t idx = channel - 1;

    // Remote scheduled starts: slave handles its own schedules locally
    if (idx >= NUM_LOCAL_CHANNELS && !manual && state) return;

    if (_valves[idx]) {
        _valves[idx]->activate(state, _status.channelDuration[idx]);
    }
}
```

No if/else for valve types. The `_valves[idx]->activate()` call dispatches to
the correct implementation via virtual method dispatch.

**Special case**: When a schedule fires on a remote channel, the slave already has
a copy of that schedule and starts itself. The master only sends manual commands
(web UI, MQTT, button press) to remote valves. Hence the early return for
`!manual && state` on remote channels.

### Late Binding of Remote Callback

The callback chain for remote valves:

```
main.cpp setup()
  └─ irrigationController->setRemoteValveCallback(remoteValveHandler)
       └─ for each RemoteValve: rv->setCallback(cb)

remoteValveHandler(channel, state, duration)
  └─ nodeManager->sendStart(channel, duration)
     or nodeManager->sendStop(channel)
```

`setRemoteValveCallback()` iterates all RemoteValve instances and sets their
callback function pointer. This is called once after NodeManager is successfully
initialized.

### Adding a New Valve Type (Future)

To add e.g. a BatteryValve that uses ESP-NOW:

1. Create `class BatteryValve : public Valve`
2. Implement `activate()` and `isActive()`
3. Instantiate in `IrrigationController::begin()` for the relevant channel range
4. Done. No changes to `activateValve()` or any other method.

---

## 2. Web API Extraction

### Problem

`WiFiManager.cpp` was ~3050 lines — half of that was 23 API route handlers that
had nothing to do with WiFi management. The class violated single responsibility:
WiFi connection management + NTP + OTA + web server + JSON API endpoints.

### Solution — WebAPIHandler

```
WiFiManager                     WebAPIHandler
├── WiFi connect/reconnect      ├── /api/schedules (GET/POST/DELETE)
├── NTP time sync               ├── /api/channels/* (status, start, stop, invert, enable)
├── OTA updates                 ├── /api/schedule/* (skip, unskip)
├── Config portal (AP mode)     ├── /api/nodes/* (pending, accept, reject, rename, unpair)
├── Web server lifecycle        ├── /api/config (GET/POST)
│   ├── GET /  (status page)    ├── /mqtt/* (save, test, remove)
│   └── POST /system/check-updates  ├── /wifi/remove
└── getWebServer() accessor     └── /system/restart
```

WiFiManager creates and owns the `WebServer` object. WebAPIHandler borrows a
pointer to it and registers its routes.

### Class Signature

```cpp
class WebAPIHandler {
public:
    WebAPIHandler(WebServer* server,
                  IrrigationController* controller,
                  HomeAssistantIntegration* ha = nullptr,
                  NodeManager* nm = nullptr,
                  WiFiManager* wm = nullptr);

    void begin();  // Registers all /api/* routes on the server
    void setNodeManager(NodeManager* nm) { _nm = nm; }
};
```

**Dependencies** (all injected via constructor):
- `WebServer*` — the shared web server (owned by WiFiManager)
- `IrrigationController*` — schedule/channel CRUD
- `HomeAssistantIntegration*` — publish state changes to MQTT after web actions
- `NodeManager*` — node pairing, slave management, schedule sync
- `WiFiManager*` — credential clearing (`clearCredentials()`)

### Wiring in main.cpp

```cpp
// After all components are initialized:
if (!wifiManager->isConfigMode() && wifiManager->getWebServer()) {
    WebAPIHandler* webApi = new WebAPIHandler(
        wifiManager->getWebServer(), irrigationController,
        homeAssistant, nodeManager, wifiManager);
    webApi->begin();
}
```

The WebAPIHandler is only created when WiFi is connected (not in AP config mode).
Route handler lambdas capture `this`, so the object must live for the lifetime of
the program (heap-allocated, never freed — standard embedded pattern).

---

## 3. Constructor Dependency Injection

### Problem

The original code used post-construction setter injection:

```cpp
// BEFORE — temporal coupling
wifiManager = new WiFiManager();
wifiManager->setController(irrigationController);   // easy to forget
wifiManager->setHomeAssistant(homeAssistant);        // order matters
```

### Solution

Dependencies are passed to constructors. The object is valid from the moment it's
constructed:

```cpp
// AFTER — all dependencies at construction
wifiManager = new WiFiManager(irrigationController, homeAssistant);
nodeManager = new NodeManager(irrigationController, nodeId.c_str(), nmRole, nodeName.c_str());
```

**Exception**: `NodeManager` is created after `WiFiManager` and can fail. Components
that need NodeManager (WiFiManager, HomeAssistantIntegration) keep a `setNodeManager()`
setter for late binding after NodeManager is successfully initialized.

### Affected Constructors

| Class | Constructor Parameters |
|-------|----------------------|
| WiFiManager | `IrrigationController*`, `HomeAssistantIntegration*`, `NodeManager*` |
| HomeAssistantIntegration | `IrrigationController*`, `NodeManager*` |
| NodeManager | `IrrigationController*`, `const char* nodeId`, `uint8_t role`, `const char* nodeName` |
| WebAPIHandler | `WebServer*`, `IrrigationController*`, `HomeAssistantIntegration*`, `NodeManager*`, `WiFiManager*` |

---

## 4. Initialization Order

The construction order matters because of dependencies:

```
1. IrrigationController        ← no dependencies
2. HomeAssistantIntegration     ← needs IrrigationController
3. WiFiManager                  ← needs IrrigationController, HomeAssistantIntegration
4. WiFiManager::begin()         ← connects WiFi, creates WebServer
5. NodeManager                  ← needs IrrigationController (conditional on feature flag)
6. Late-bind NodeManager        ← wifiManager->setNodeManager(nm)
                                   homeAssistant->setNodeManager(nm)
                                   irrigationController->setRemoteValveCallback(handler)
7. WebAPIHandler                ← needs all of the above
8. WebAPIHandler::begin()       ← registers routes on WebServer
```

---

## 5. File Map

| File | Lines | Responsibility |
|------|-------|---------------|
| `include/Valve.h` | ~50 | Abstract Valve, LocalValve, RemoteValve declarations |
| `src/Valve.cpp` | ~44 | LocalValve GPIO logic, RemoteValve callback dispatch |
| `include/WebAPIHandler.h` | ~60 | WebAPIHandler declaration, 23 handler method signatures |
| `src/WebAPIHandler.cpp` | ~876 | All 23 route handler implementations |
| `include/IrrigationController.h` | ~100 | Controller with `Valve* _valves[]` array |
| `src/IrrigationController.cpp` | ~430 | Valve creation in `begin()`, polymorphic `activateValve()` |
| `include/WiFiManager.h` | ~110 | WiFi management, `getWebServer()` accessor |
| `src/WiFiManager.cpp` | ~2260 | WiFi, NTP, OTA, status page (no API routes) |
| `src/main.cpp` | ~440 | Component wiring, construction order |
