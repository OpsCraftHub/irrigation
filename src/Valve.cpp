#include "Valve.h"
#include "Config.h"

// ============================================================================
// LocalValve — GPIO-driven MOSFET/relay
// ============================================================================

LocalValve::LocalValve(uint8_t pin, bool inverted)
    : _pin(pin), _inverted(inverted), _active(false) {
}

void LocalValve::activate(bool state, uint16_t durationMinutes) {
    _active = state;
    bool pinState = _inverted ? !state : state;
    digitalWrite(_pin, pinState ? HIGH : LOW);
    DEBUG_PRINTF("LocalValve: GPIO %d %s (inv:%d)\n", _pin, state ? "ON" : "OFF", _inverted);
}

void LocalValve::setInverted(bool inverted) {
    _inverted = inverted;
    // Update pin state if currently off (maintain correct off level)
    if (!_active) {
        digitalWrite(_pin, _inverted ? HIGH : LOW);
    }
}

// ============================================================================
// RemoteValve — dispatched via callback to NodeManager
// ============================================================================

RemoteValve::RemoteValve(uint8_t channel, Callback cb)
    : _channel(channel), _cb(cb), _active(false) {
}

void RemoteValve::activate(bool state, uint16_t durationMinutes) {
    if (_cb) {
        _cb(_channel, state, durationMinutes);
        DEBUG_PRINTF("RemoteValve: Channel %d %s (remote)\n", _channel, state ? "ON" : "OFF");
    } else {
        DEBUG_PRINTF("RemoteValve: Channel %d has no callback, ignoring\n", _channel);
    }
    _active = state;
}
