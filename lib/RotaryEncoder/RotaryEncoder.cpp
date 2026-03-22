#include "RotaryEncoder.h"

// Static member definitions
volatile int8_t   RotaryEncoder::_delta          = 0;
volatile uint32_t RotaryEncoder::_interruptCount = 0;
volatile uint8_t  RotaryEncoder::_lastState      = 0;

// Module-level instance pointer so the static ISR can access pin config
static RotaryEncoder* _instance = nullptr;

void RotaryEncoder::begin(uint8_t pinA, uint8_t pinB)
{
    _pinA = pinA;
    _pinB = pinB;
    _instance = this;

    pinMode(_pinA, INPUT_PULLUP);
    pinMode(_pinB, INPUT_PULLUP);
    _lastState = (digitalRead(_pinA) << 1) | digitalRead(_pinB);

    attachInterrupt(digitalPinToInterrupt(_pinA), isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(_pinB), isr, CHANGE);
}

int8_t RotaryEncoder::read()
{
    int8_t val;
    noInterrupts();
    val    = _delta;
    _delta = 0;
    interrupts();
    return val;
}

uint32_t RotaryEncoder::interruptCount() const
{
    return _interruptCount;
}

void IRAM_ATTR RotaryEncoder::isr()
{
    _interruptCount++;

    uint8_t curr   = (digitalRead(_instance->_pinA) << 1) | digitalRead(_instance->_pinB);
    uint8_t change = (_lastState << 2) | curr;
    _lastState     = curr;

    // Only count on arrival at the detent position (both pins HIGH = 0b11).
    // CW  arrives at 11 from 10 (change = 0b1011)
    // CCW arrives at 11 from 01 (change = 0b0111)
    if (change == 0b1011)
        _delta++;
    else if (change == 0b0111)
        _delta--;
}
