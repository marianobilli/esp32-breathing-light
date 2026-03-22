# AGENTS.md - ESP32 PlatformIO Agent Guidelines

## Target Board

**YD-ESP32-23** — ESP32-S3 DevKitC1 clone with 16MB flash and 8MB PSRAM.

- Framework: Arduino (via PlatformIO)
- Primary environment name: `yd_esp32_23`
- `pio` is available on the terminal PATH — always use it for all PlatformIO operations

---

## Project Structure

```
src/          # Application code (main .cpp file with setup() and loop())
lib/          # Local libraries
test/         # Unity test suites (test_ prefix per folder)
partitions/   # Custom partition table CSV
platformio.ini
```

Do not restructure this layout. Place new source files in `src/`, new libraries in `lib/`, and new tests in `test/test_<name>/`.

---

## platformio.ini Rules

Always preserve the following in `[env:yd_esp32_23]`:

```ini
[env:yd_esp32_23]
platform = espressif32
board = esp32s3box
framework = arduino
board_build.partitions = partitions/partitions_16MB_psram.csv
monitor_speed = 115200
upload_speed = 921600
```

Add libraries under `lib_deps`, never by modifying the source directly.

---

## Build Commands

| Task | Command |
|------|---------|
| Build all | `pio run` |
| Build specific env | `pio run -e yd_esp32_23` |
| Upload to board | `pio run -e yd_esp32_23 --target upload` |
| Open serial monitor | `pio device monitor` |
| Run all tests | `pio test` |
| Run native tests only | `pio test -e native` |
| Run board tests only | `pio test -e yd_esp32_23` |
| Filter by test name | `pio test --filter "test_<name>"` |
| List tests | `pio test --list-tests` |
| Verbose test output | `pio test -v` |

Always build before assuming code is correct. If a build fails, fix all errors before proceeding.

---

## Partition Table

Use the custom partition table at `partitions/partitions_16MB_psram.csv`. Do not change or replace it.

```csv
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x5000,
otadata,  data, ota,     0xe000,   0x2000,
app0,     app,  ota_0,   0x10000,  0x640000,
app1,     app,  ota_1,   0x650000, 0x640000,
spiffs,   data, spiffs,  0xc90000, 0x360000,
coredump, data, coredump,0xFF0000, 0x10000,
```

**Critical constraints:**
- `otadata` at `0xe000` is mandatory — `esptool` always flashes `boot_app0.bin` there regardless of the partition table; removing it corrupts the boot process
- `coredump` is required — without it the board may reboot instead of running user code
- Do not shrink `app0`/`app1` — both OTA slots must remain at 6.25MB for the standard bootloader

---

## Arduino Framework Constraints

The Arduino framework is used for its simplicity and portability. Within this framework, do not attempt to use:

- Direct RTOS primitives (tasks, queues, semaphores via `freertos/` headers)
- Direct hardware register manipulation
- Custom bootloader code
- Manual IRAM/DRAM/PSRAM allocation attributes
- Low-level WiFi/BT stack configuration
- DMA or cryptographic hardware accelerator APIs

If a feature requires these, note the limitation rather than working around the framework boundary.

---

## Testing Strategy

### Environments

| Test type | Environment | When to use |
|-----------|-------------|-------------|
| Logic, algorithms, data structures | `native` | No board required |
| GPIO, Serial, WiFi, PSRAM, hardware | `yd_esp32_23` | Board must be connected |
| Both | Both | When logic and hardware both need validation |

### Test File Structure

Each test folder under `test/` contains exactly one `.cpp` file. That file compiles for both environments using `#ifndef NATIVE` guards:

```cpp
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

void test_something(void) {
    TEST_ASSERT_EQUAL(4, 2 + 2);
}

#ifndef NATIVE
// Board entry point (Arduino framework)
#include <Arduino.h>

void setup() {
    delay(2000); // allow serial monitor to connect
    UNITY_BEGIN();
    RUN_TEST(test_something);
    UNITY_END();
}

void loop() {}

#else
// Native entry point
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_something);
    return UNITY_END();
}
#endif
```

### Test Writing Rules

- Never `#include <Arduino.h>` at the top level of a test file — only inside `#ifndef NATIVE`
- Name test functions with the `test_` prefix
- One test folder = one concern; do not bundle unrelated tests
- The `native` environment must define `build_flags = -DNATIVE` and must **not** set `framework = arduino`
- Keep tests independent and repeatable — no shared mutable state between test functions
- Separate hardware-dependent code from business logic so native tests can cover the logic path

---

## Coding Principles

- **Prefer `setup()`/`loop()` architecture** — do not introduce threading unless the Arduino framework explicitly supports it for this board
- **Use `Serial` for all debug output**
- **Do not call `Serial.flush()` in code that runs without a connected monitor**
- **Validate with a build after every non-trivial change** — run `pio run -e yd_esp32_23` and fix all warnings and errors before considering a change complete
- **Add libraries through `lib_deps` in `platformio.ini`** — never vendor third-party code into `src/` or `lib/` manually unless there is no PlatformIO registry entry
