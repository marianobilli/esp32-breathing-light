# Unit Testing with PlatformIO (ESP32 + Arduino framework)

PlatformIO's built-in test runner supports two environments. Choosing the right one depends on whether your test needs real hardware.

> Make sure you have completed the initial setup in [README.md](README.md) before running device tests — particularly the USB connector and serial port configuration.

---

## Table of Contents

1. [Native vs device — which environment to use](#1-native-vs-device--which-environment-to-use)
2. [How tests are structured](#2-how-tests-are-structured)
3. [Writing tests](#3-writing-tests)
   - [Native only](#native-only)
   - [Device only](#device-only)
   - [Both environments](#both-environments)
4. [Hardware output tests (smoke tests)](#4-hardware-output-tests-smoke-tests)
5. [Command reference](#5-command-reference)

---

## 1. Native vs device — which environment to use

| | Native (`native`) | Device (`yd_esp32_23`) |
|---|---|---|
| Runs on | Your host machine | Physical ESP32 board |
| Board required | No | Yes — connected via `UART` USB connector |
| Speed | Fast | Slow (build → flash → execute) |
| Best for | Logic, algorithms, data structures | Hardware peripherals (GPIO, LEDs, WiFi, PSRAM) |
| Arduino APIs available | No | Yes |

**Rule of thumb:** if the test does not touch hardware, run it native. If it does, run it on the device.

Start with native tests when possible — they are much faster and require no board connection.

---

## 2. How tests are structured

Each test suite is a **folder** inside `test/` containing a single `.cpp` file. The folder name must start with `test_`.

```
test/
├── test_basic/
│   └── test_basic.cpp      # logic tests, runs native and on device
└── test_rgb_led/
    └── test_rgb_led.cpp    # hardware test, runs on device only
```

All tests use the [Unity](https://github.com/ThrowTheSwitch/Unity) testing framework, which is lightweight and designed for embedded systems. Test functions must follow the naming convention `test_*` and use Unity assertion macros like `TEST_ASSERT_EQUAL`, `TEST_ASSERT_TRUE`, etc.

Every test file has the same three-part structure:

```
1. Unity includes and shared test functions (always compiled)
2. #ifndef NATIVE block  → Arduino entry point (setup / loop)
3. #else block           → host machine entry point (main)
```

This lets a single file compile correctly for both environments.

---

## 3. Writing tests

### Native only

Use this pattern when the test has no hardware dependencies. The entry point is a plain `main()`.

```cpp
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

void test_addition(void) {
    TEST_ASSERT_EQUAL(4, 2 + 2);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_addition);
    return UNITY_END();
}
```

Run it:

```bash
pio test -e native --filter "test_basic"
```

---

### Device only

Use this pattern when the test calls Arduino APIs (`delay`, `neopixelWrite`, `Serial`, etc.). The entry point is `setup()` / `loop()`.

Wrap everything that touches hardware or Arduino APIs inside `#ifndef NATIVE` so the native compiler never sees it.

```cpp
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

#ifndef NATIVE
#include <Arduino.h>

void test_led_on(void) {
    neopixelWrite(48, 64, 0, 0); // drive onboard LED red
    delay(500);
    TEST_ASSERT_TRUE(true); // confirms the call completed without crashing
}

void setup() {
    delay(2000); // give the serial monitor time to connect
    UNITY_BEGIN();
    RUN_TEST(test_led_on);
    UNITY_END();
}

void loop() {}

#else
int main(void) {
    UNITY_BEGIN();
    // no hardware tests to run on native
    return UNITY_END();
}
#endif
```

Run it:

```bash
pio test -e yd_esp32_23 --filter "test_rgb_led"
```

> The board must be connected to the `UART` USB connector. See [README.md — Physical setup](README.md#3-physical-setup--which-usb-port-to-use).

---

### Both environments

Pure logic tests can run on both. The test functions themselves are written once (outside any `#ifdef`). Only the entry point differs.

```cpp
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

// This test function compiles for both native and device
void test_multiply(void) {
    TEST_ASSERT_EQUAL(12, 3 * 4);
}

#ifndef NATIVE
#include <Arduino.h>

void setup() {
    delay(2000);
    UNITY_BEGIN();
    RUN_TEST(test_multiply);
    UNITY_END();
}

void loop() {}

#else

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_multiply);
    return UNITY_END();
}
#endif
```

Run it on both:

```bash
pio test -e native --filter "test_basic"
pio test -e yd_esp32_23 --filter "test_basic"
```

---

## 4. Hardware output tests (smoke tests)

Some hardware outputs — LEDs, motors, signals — cannot be read back in software. There is no way to assert "the LED is actually red." The standard pattern for these is to assert that the call completed without crashing or hanging:

```cpp
TEST_ASSERT_TRUE(true); // call completed without crash or hang
```

This is intentional. It verifies the code path executes without throwing, hanging, or causing a reboot. If the board crashes or reboots, the test runner will report a timeout or connection failure — a clear signal that something went wrong.

See `test/test_rgb_led/` for a real example. It drives the onboard NeoPixel through white, red, green, blue, and off, with one test case per color.

---

## 5. Command reference

```bash
# Run all suites on the host machine (no board needed)
pio test -e native

# Run all suites on the physical board
pio test -e yd_esp32_23

# Run a specific suite by folder name
pio test -e yd_esp32_23 --filter "test_rgb_led"

# Compile and check without flashing (useful for catching build errors)
pio test -e yd_esp32_23 --without-uploading

# List all discovered test suites
pio test --list-tests

# Verbose output (shows individual test results and timing)
pio test -e yd_esp32_23 -v

# Extra verbose (shows serial output and full Unity output)
pio test -e yd_esp32_23 -vvv
```
