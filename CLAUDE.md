# CLAUDE.md — ESP32 Breathing Light

Agent rulebook for this repo. Follow everything here; flag anything that forces a deviation.

## What this project is

A hardware breath pacer on a **YD-ESP32-23** (ESP32-S3, 16 MB flash, 8 MB PSRAM). An IRLZ44N-driven 12 V LED strip breathes in sync with inhale/exhale WAVs played through a PCM5102 I2S DAC. An SH1106 OLED + rotary encoder + two push buttons drive a settings menu; settings persist in NVS via `Preferences`.

- LED brightness envelopes are pre-computed from the WAVs by `tools/gen_led_envelope.py` and emitted as lookup tables in `include/led_envelope.h`.
- WAVs live in `data/` (SPIFFS). After changing them, re-run the generator **and** `pio run -t uploadfs`.
- Full pin map: `modules_connections.md`.

## Environment

- Framework: Arduino via PlatformIO
- Primary env: `yd_esp32_23` (board `esp32-s3-devkitc-1`, QIO/OPI flash, OPI PSRAM, 240 MHz)
- `pio` is on PATH — always use it for PlatformIO operations

## platformio.ini invariants

Do not change these in `[env:yd_esp32_23]`:

```ini
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
board_build.arduino.memory_type = qio_opi
board_build.flash_mode = qio
board_build.psram_type = opi
board_upload.flash_size = 16MB
board_upload.maximum_size = 16777216
board_build.partitions = partitions/partitions_16MB_psram.csv
board_build.extra_flags = -DBOARD_HAS_PSRAM
monitor_speed = 115200
```

Add libraries via `lib_deps`. Never vendor third-party code into `src/` or `lib/` unless there is no PlatformIO registry entry.

## Project layout

```
src/          application code (main.cpp)
include/      headers, including auto-generated led_envelope.h
lib/          local libraries
data/         SPIFFS root (breath_in.wav, breath_out.wav)
tools/        gen_led_envelope.py + generated envelope_viz.html
test/         Unity tests (test_<name>/ per folder, exactly one .cpp each)
partitions/   custom partition CSV (do not edit)
docs/         envelope visualization PNGs referenced by README
```

New source goes in `src/`, new libs in `lib/`, new tests in `test/test_<name>/`. Do not restructure.

## Partition table

`partitions/partitions_16MB_psram.csv` — do not edit, do not replace:

```csv
nvs,      data, nvs,     0x9000,   0x5000,
otadata,  data, ota,     0xe000,   0x2000,
app0,     app,  ota_0,   0x10000,  0x640000,
app1,     app,  ota_1,   0x650000, 0x640000,
spiffs,   data, spiffs,  0xc90000, 0x360000,
coredump, data, coredump,0xFF0000, 0x10000,
```

- `otadata` at `0xe000` is mandatory — `esptool` flashes `boot_app0.bin` there regardless of the partition table; removing it corrupts boot.
- `coredump` is required by the S3 crash handler.
- Do not shrink `app0`/`app1` — both OTA slots must stay at 6.25 MB for the standard bootloader.

## Commands

| Task | Command |
|---|---|
| Build | `pio run` |
| Upload firmware | `pio run -t upload` |
| Upload SPIFFS (WAVs) | `pio run -t uploadfs` |
| Serial monitor | `pio device monitor` |
| Board tests | `pio test -e yd_esp32_23` |
| Native tests (if `[env:native]` defined) | `pio test -e native` |
| Filter tests | `pio test --filter "test_<name>"` |
| Regenerate LED envelope | `python3 tools/gen_led_envelope.py` |

Always build after non-trivial changes; fix all warnings and errors before declaring work done.

## Framework constraints

Stay inside the Arduino framework. Do not use:

- Direct FreeRTOS primitives (`freertos/` tasks, queues, semaphores)
- Direct register manipulation
- Custom bootloader code
- Manual IRAM/DRAM/PSRAM attributes
- Low-level WiFi/BT stack configuration
- DMA or crypto-accelerator APIs

If a requirement pushes past these, note the limitation rather than work around it.

## Testing

| Type | Env | Board? |
|---|---|---|
| Logic, algorithms, pure data | `native` | No |
| GPIO, Serial, I2S, hardware | `yd_esp32_23` | Yes, on `UART` USB-C |

Each `test/test_<name>/` contains exactly one `.cpp` with both entry points:

```cpp
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}
void test_something(void) { TEST_ASSERT_EQUAL(4, 2 + 2); }

#ifndef NATIVE
#include <Arduino.h>
void setup() { delay(2000); UNITY_BEGIN(); RUN_TEST(test_something); UNITY_END(); }
void loop() {}
#else
int main(void) { UNITY_BEGIN(); RUN_TEST(test_something); return UNITY_END(); }
#endif
```

Rules:

- Never `#include <Arduino.h>` at the top level — only inside `#ifndef NATIVE`.
- `test_` prefix for test functions.
- One folder = one concern; don't bundle unrelated tests.
- A `native` env must define `build_flags = -DNATIVE` and must **not** set `framework = arduino`.
- Separate hardware-dependent code from business logic so native tests can cover the logic path.

See `how_to_unit_test.md` for the long-form guide.

## Coding principles

- `setup()`/`loop()` architecture — no threading unless the framework explicitly supports it.
- `Serial` for debug output.
- **Never call `Serial.flush()`** in code that can run without a connected monitor. With `ARDUINO_USB_CDC_ON_BOOT=1` it blocks forever waiting for a reader.
- Shared state between `loop()` and any background work is `volatile`; document memory ordering (see the `__sync_synchronize()` fences in `updateBreath()` in `src/main.cpp`).
- Settings that survive reboot go through `Preferences` (`loadConfig()` / `saveConfig()` in `src/main.cpp`).

## Breath loop specifics

- `BREATH_IN_MS`, `BREATH_OUT_MS`, `LED_ENV_IN_N`, `LED_ENV_OUT_N`, and `LED_ENV_WINDOW_MS` are emitted into `include/led_envelope.h` by `tools/gen_led_envelope.py` — they derive from the WAV lengths. To change cadence: replace `data/breath_in.*` / `data/breath_out.*` (any format; the generator auto-converts via ffmpeg), run the generator, then `pio run -t upload && pio run -t uploadfs`. No source edits needed.
- LED PWM: GPIO 1, 25 kHz, 10-bit, with a slew limiter (`LED_MAX_STEP`) and a minimum non-zero duty (`LED_MIN_DUTY`) to avoid MOSFET linear-region flicker. Preserve these when refactoring.
- I2S: BCLK=2, WS=41, DOUT=42, 16 kHz — matches the WAV format the envelope generator expects.
- Bend profiles are selected at runtime via the OLED menu and persisted per-phase (`bendIdxIn`, `bendIdxOut` in NVS); keep both indices bounded by `LED_ENV_NUM_PROFILES`.
