# Rotary Encoder on ESP32 — Reliable Implementation

## Hardware

- **Encoder type**: EC11 (or compatible) — mechanical rotary encoder with push button
- **Pins used** (adjust to your board):
  - `PIN_TRA` (CLK / A) — e.g. GPIO 9
  - `PIN_TRB` (DT  / B) — e.g. GPIO 18
  - Push button pin handled separately as a regular debounced button
- **Pull-ups**: both encoder pins use `INPUT_PULLUP` (no external resistors needed)
- **Resting state**: at any detent (click position), both CLK and DT read HIGH (`0b11`)

## How It Works

Uses a **Gray-code state machine** triggered on `CHANGE` of **both** pins. Instead of counting every transition, it only registers a count when the encoder snaps into a detent (both pins return to HIGH). This fires **exactly once per physical click** with no debounce needed — noise and intermediate transitions are naturally ignored because they don't match the detent-arrival pattern.

### Quadrature sequence (EC11, pull-ups)

```
Detent → CW turn → Detent
  11  →  10  →  00  →  01  →  11
                               ↑ count here (arrived from 01 → 11 = change 0b0111)

Detent → CCW turn → Detent
  11  →  01  →  00  →  10  →  11
                               ↑ count here (arrived from 10 → 11 = change 0b1011)
```

The `change` word is built as `(lastState << 2) | currState` — a 4-bit value encoding the full transition.

## Code

### Globals

```cpp
volatile int8_t  encoderDelta     = 0;
volatile uint32_t encoderInterruptCount = 0;
volatile uint8_t  lastEncoderState = 0;
```

### ISR

```cpp
void IRAM_ATTR encoderISR()
{
    encoderInterruptCount++;

    uint8_t curr   = (digitalRead(PIN_TRA) << 1) | digitalRead(PIN_TRB);
    uint8_t change = (lastEncoderState << 2) | curr;
    lastEncoderState = curr;

    // CW:  arrived at detent from 01  (0b0111)
    // CCW: arrived at detent from 10  (0b1011)
    if (change == 0b0111)       // CCW
        encoderDelta--;
    else if (change == 0b1011)  // CW
        encoderDelta++;
}
```

> **Direction note**: swap `++` and `--` to invert rotation direction.

### Setup

```cpp
pinMode(PIN_TRA, INPUT_PULLUP);
pinMode(PIN_TRB, INPUT_PULLUP);
// Seed state from current pin levels before attaching interrupts
lastEncoderState = (digitalRead(PIN_TRA) << 1) | digitalRead(PIN_TRB);
attachInterrupt(digitalPinToInterrupt(PIN_TRA), encoderISR, CHANGE);
attachInterrupt(digitalPinToInterrupt(PIN_TRB), encoderISR, CHANGE);
```

### Loop (reading the value)

```cpp
int8_t delta = 0;
noInterrupts();
delta = encoderDelta;
encoderDelta = 0;
interrupts();

if (delta != 0)
{
    // delta is ±1 per physical click
    // Example: drive a 0–100 value with 5% steps
    int newVal = currentValue + (delta * 5);
    currentValue = (newVal < 0) ? 0 : (newVal > 100) ? 100 : newVal;
}
```

## Key Design Decisions

| Decision | Reason |
|---|---|
| CHANGE on both pins | Captures all quadrature transitions so the state machine has full context |
| Count only at detent arrival | Eliminates double-counts and mid-turn false triggers without any time-based debounce |
| No debounce timer | Time-based debounce would block legitimate fast transitions; state filtering is sufficient |
| Seed `lastEncoderState` before attaching interrupts | Prevents a spurious count on the first edge after boot |
| `noInterrupts()` guard in loop | Prevents tearing of the `int8_t` between ISR write and loop read |

## Troubleshooting

- **Encoder barely responds**: detent resting state might not be `0b11`. Check via serial: print `digitalRead(PIN_TRA)` and `digitalRead(PIN_TRB)` when at rest and update the two `change` match values accordingly.
- **Direction wrong**: swap `++` and `--` in the ISR.
- **Still getting double counts**: verify both interrupts are attached to the correct pins and `lastEncoderState` is seeded before `attachInterrupt`.
