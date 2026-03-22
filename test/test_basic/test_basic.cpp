#include <unity.h>

// Test setup function - runs before each test
void setUp(void) {}

// Test teardown function - runs after each test
void tearDown(void) {}

void test_addition_should_work(void) {
    TEST_ASSERT_EQUAL(4, 2 + 2);
}

void test_subtraction_should_work(void) {
    TEST_ASSERT_EQUAL(0, 2 - 2);
}

// --- Board entry point (Arduino framework) ---
#ifndef NATIVE

#include <Arduino.h>

void setup() {
    delay(2000); // wait for serial port to be ready
    UNITY_BEGIN();
    RUN_TEST(test_addition_should_work);
    RUN_TEST(test_subtraction_should_work);
    UNITY_END();
}

void loop() {}

// --- Native entry point (host machine) ---
#else

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_addition_should_work);
    RUN_TEST(test_subtraction_should_work);
    return UNITY_END();
}

#endif
