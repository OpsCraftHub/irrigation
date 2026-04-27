#ifndef VALVE_H
#define VALVE_H

#include <Arduino.h>

// Abstract valve interface — uniform control for local GPIO and remote UDP channels
class Valve {
public:
    virtual void activate(bool state, uint16_t durationMinutes) = 0;
    virtual bool isActive() const = 0;
    virtual ~Valve() = default;
};

// Local GPIO-driven valve (MOSFET/relay on-board)
class LocalValve : public Valve {
public:
    LocalValve(uint8_t pin, bool inverted = false);
    void activate(bool state, uint16_t durationMinutes) override;
    bool isActive() const override { return _active; }

    void setInverted(bool inverted);
    bool isInverted() const { return _inverted; }
    uint8_t getPin() const { return _pin; }

private:
    uint8_t _pin;
    bool _inverted;
    bool _active;
};

// Remote valve dispatched via callback to NodeManager
class RemoteValve : public Valve {
public:
    using Callback = void (*)(uint8_t channel, bool state, uint16_t duration);

    RemoteValve(uint8_t channel, Callback cb = nullptr);
    void activate(bool state, uint16_t durationMinutes) override;
    bool isActive() const override { return _active; }

    void setCallback(Callback cb) { _cb = cb; }
    void setActive(bool active) { _active = active; }

private:
    uint8_t _channel;  // 1-based channel number
    Callback _cb;
    bool _active;
};

#endif // VALVE_H
