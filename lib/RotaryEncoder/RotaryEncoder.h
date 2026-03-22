#pragma once
#include <Arduino.h>

class RotaryEncoder {
public:
    void begin(uint8_t pinA, uint8_t pinB);
    int8_t read();  // returns delta since last call, resets to 0
    uint32_t interruptCount() const;

private:
    static void IRAM_ATTR isr();
    uint8_t _pinA, _pinB;

    static volatile int8_t   _delta;
    static volatile uint32_t _interruptCount;
    static volatile uint8_t  _lastState;
};
