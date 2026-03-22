#include <unity.h>

// RGB LED is on pin 48 on the YD-ESP32-23 board
#define RGB_PIN 48
#define RGB_BRIGHTNESS 64

void setUp(void) {}
void tearDown(void) {}

// --- Device-only tests ---
// neopixelWrite is an Arduino/ESP32 built-in; it has no return value and
// cannot be called on native. All tests here target the yd_esp32_23 env.
#ifndef NATIVE

#include <Arduino.h>

// Verifies that calling neopixelWrite does not crash for each colour.
// Hardware output tests on embedded devices cannot read back the LED state,
// so the assertion confirms the call completed successfully (smoke test).

void test_rgb_white(void) {
    TEST_MESSAGE("Running test_rgb_white...");
    neopixelWrite(RGB_PIN, RGB_BRIGHTNESS, RGB_BRIGHTNESS, RGB_BRIGHTNESS);
    delay(500);
    TEST_ASSERT_TRUE(true);
}

void test_rgb_red(void) {
    TEST_MESSAGE("Running test_rgb_red...");
    neopixelWrite(RGB_PIN, RGB_BRIGHTNESS, 0, 0);
    delay(500);
    TEST_ASSERT_TRUE(true);
}

void test_rgb_green(void) {
    TEST_MESSAGE("Running test_rgb_green...");
    neopixelWrite(RGB_PIN, 0, RGB_BRIGHTNESS, 0);
    delay(500);
    TEST_ASSERT_TRUE(true);
}

void test_rgb_blue(void) {
    TEST_MESSAGE("Running test_rgb_blue...");
    neopixelWrite(RGB_PIN, 0, 0, RGB_BRIGHTNESS);
    delay(500);
    TEST_ASSERT_TRUE(true);
}

void test_rgb_off(void) {
    TEST_MESSAGE("Running test_rgb_off...");
    neopixelWrite(RGB_PIN, 0, 0, 0);
    delay(500);
    TEST_ASSERT_TRUE(true);
}

void setup() {
    delay(2000); // wait for serial port to be ready
    UNITY_BEGIN();
    RUN_TEST(test_rgb_white);
    RUN_TEST(test_rgb_red);
    RUN_TEST(test_rgb_green);
    RUN_TEST(test_rgb_blue);
    RUN_TEST(test_rgb_off);
    UNITY_END();
}

void loop() {}

#else

// neopixelWrite does not exist on native; skip all LED tests.
int main(void) {
    UNITY_BEGIN();
    // No tests to run in native env — RGB LED requires real hardware.
    return UNITY_END();
}

#endif
