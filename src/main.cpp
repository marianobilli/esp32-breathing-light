#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <driver/i2s.h>
#include <math.h>
#include "RotaryEncoder.h"

// ---------------------------------------------------------------------------
// Pin definitions
// ---------------------------------------------------------------------------
static const uint8_t LED_PIN = 48;
static const uint8_t LED_MAX_BRIGHT = 80;

// ---------------------------------------------------------------------------
// I2S / MAX98357A pin definitions
// ---------------------------------------------------------------------------
static const uint8_t I2S_BCLK_PIN = 41;
static const uint8_t I2S_WS_PIN = 42;
static const uint8_t I2S_DOUT_PIN = 40;
// I2S_SD_PIN (GPIO 35) is intentionally left floating — see hardware notes.
// Datasheet requires ~2kΩ series resistor when VDD = VDDIO; without it,
// direct GPIO drive at 3.3V can put the SD_MODE comparator in an undefined
// state. Muting is handled by sending zero-amplitude I2S data instead.
static const uint32_t I2S_SAMPLE_RATE = 16000;
static const float SINE_FREQ_MIN = 150.0f; // pitch at exhale bottom
static const float SINE_FREQ_MAX = 300.0f; // pitch at inhale peak
static const int16_t MAX_AMPLITUDE = 30000;

// ---------------------------------------------------------------------------
// Pin definitions
// ---------------------------------------------------------------------------
enum Pins : uint8_t
{
    PIN_SDA = 5,
    PIN_SCL = 3,
    PIN_CON = 4,  // Confirm button
    PIN_PSH = 8,  // Rotary encoder push button
    PIN_TRA = 9,  // Encoder A (CLK)
    PIN_TRB = 18, // Encoder B (DT)
    PIN_BAK = 17, // Back button
};

// ---------------------------------------------------------------------------
// OLED driver
// Swap to U8G2_SSD1306_128X64_NONAME_F_HW_I2C if the display doesn't init.
// ---------------------------------------------------------------------------
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ---------------------------------------------------------------------------
// Button debounce
// ---------------------------------------------------------------------------
static const uint32_t DEBOUNCE_MS = 50;

struct Button
{
    uint8_t pin;
    bool lastRaw;
    bool stableState;
    uint32_t lastChangeMs;
    bool pressed; // single-shot flag; caller clears after handling
};

void initButton(Button &btn, uint8_t pin)
{
    btn.pin = pin;
    btn.lastRaw = HIGH;
    btn.stableState = HIGH;
    btn.lastChangeMs = 0;
    btn.pressed = false;
    pinMode(pin, INPUT_PULLUP);
}

void updateButton(Button &btn)
{
    bool raw = digitalRead(btn.pin);
    if (raw != btn.lastRaw)
    {
        btn.lastChangeMs = millis();
        btn.lastRaw = raw;
    }
    if ((millis() - btn.lastChangeMs) >= DEBOUNCE_MS)
    {
        if (btn.stableState == HIGH && raw == LOW)
        {
            btn.pressed = true;
        }
        btn.stableState = raw;
    }
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------
enum Screen : uint8_t
{
    SCREEN_MAIN = 0
};
enum BreathPhase : uint8_t
{
    BREATH_IN,
    BREATH_OUT
};

static const uint32_t BREATH_IN_MS = 4000;
static const uint32_t BREATH_OUT_MS = 6000;

Screen currentScreen = SCREEN_MAIN;
bool breatheEnabled = true;
bool soundEnabled = true;
BreathPhase breathPhase = BREATH_IN;
uint32_t breathStartMs = 0;
uint8_t soundVolume = 50; // 0–100 %

// ---------------------------------------------------------------------------
// Breathing logic
// ---------------------------------------------------------------------------
void updateBreath()
{
    if (!breatheEnabled)
        return;

    uint32_t duration = (breathPhase == BREATH_IN) ? BREATH_IN_MS : BREATH_OUT_MS;
    uint32_t elapsed = millis() - breathStartMs;

    if (elapsed >= duration)
    {
        breathPhase = (breathPhase == BREATH_IN) ? BREATH_OUT : BREATH_IN;
        breathStartMs = millis();
    }
}

uint32_t breathSecondsRemaining()
{
    uint32_t duration = (breathPhase == BREATH_IN) ? BREATH_IN_MS : BREATH_OUT_MS;
    uint32_t elapsed = millis() - breathStartMs;
    if (elapsed >= duration)
        return 0;
    uint32_t msLeft = duration - elapsed;
    return (msLeft + 999) / 1000; // ceiling
}

// Piecewise linear easing: first 1/3 of time covers 2/3 of the range (faster),
// remaining 2/3 of time covers the last 1/3 (slower).
static float easeBreath(float t)
{
    if (t < 1.0f / 3.0f)
    {
        return t * 2.0f;
    }
    else
    {
        return 2.0f / 3.0f + (t - 1.0f / 3.0f) * 0.5f;
    }
}

void updateLed()
{
    if (!breatheEnabled)
    {
        neopixelWrite(LED_PIN, 0, 0, 0);
        return;
    }

    uint32_t duration = (breathPhase == BREATH_IN) ? BREATH_IN_MS : BREATH_OUT_MS;
    uint32_t elapsed = millis() - breathStartMs;
    if (elapsed > duration)
        elapsed = duration;

    float t = easeBreath((float)elapsed / duration);
    uint8_t b = (breathPhase == BREATH_IN)
                    ? (uint8_t)(t * LED_MAX_BRIGHT)
                    : (uint8_t)((1.0f - t) * LED_MAX_BRIGHT);

    neopixelWrite(LED_PIN, b, b, b);
}

// ---------------------------------------------------------------------------
// I2S / sound
// ---------------------------------------------------------------------------
void initI2S()
{
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = I2S_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false,
        .tx_desc_auto_clear = true,
    };
    i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);

    i2s_pin_config_t pins = {
        .bck_io_num = I2S_BCLK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_DOUT_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };
    i2s_set_pin(I2S_NUM_0, &pins);
}

void soundTask(void * /*param*/)
{
    const int BUF_SAMPLES = 64;
    int16_t buf[BUF_SAMPLES * 2]; // interleaved L/R
    float phaseAccum = 0.0f;

    while (true)
    {
        float amplitude = 0.0f;
        float freq = SINE_FREQ_MIN;

        if (soundEnabled && breatheEnabled)
        {
            uint32_t duration = (breathPhase == BREATH_IN) ? BREATH_IN_MS : BREATH_OUT_MS;
            uint32_t elapsed = millis() - breathStartMs;
            if (elapsed > duration)
                elapsed = duration;
            float t = easeBreath((float)elapsed / duration);
            float env = (breathPhase == BREATH_IN) ? t : (1.0f - t);
            float volLinear = soundVolume / 100.0f;
            float volExp = volLinear * volLinear; // square for perceptual loudness
            amplitude = env * MAX_AMPLITUDE * volExp;
            freq = SINE_FREQ_MIN + env * (SINE_FREQ_MAX - SINE_FREQ_MIN);
            static uint32_t lastLogMs = 0;
            if (millis() - lastLogMs > 1000)
            {
                lastLogMs = millis();
                Serial.printf("vol=%u%% amp=%.1f freq=%.0f\n", soundVolume, amplitude, freq);
            }
        }

        float phaseIncrement = 2.0f * M_PI * freq / I2S_SAMPLE_RATE;
        for (int i = 0; i < BUF_SAMPLES; i++)
        {
            int16_t sample = (int16_t)(sinf(phaseAccum) * amplitude);
            buf[i * 2] = sample;     // L
            buf[i * 2 + 1] = sample; // R
            phaseAccum += phaseIncrement;
        }
        // keep phaseAccum in [0, 2π) to avoid float precision drift
        phaseAccum = fmodf(phaseAccum, 2.0f * M_PI);

        size_t written;
        i2s_write(I2S_NUM_0, buf, sizeof(buf), &written, portMAX_DELAY);
    }
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------
static const uint32_t DISPLAY_TICK_MS = 100;

void drawSplash()
{
    u8g2.clearBuffer();

    // "Moni" — large serif font, centered
    u8g2.setFont(u8g2_font_osb26_tf);
    const char *line1 = "Gary";
    u8g2.drawStr((128 - u8g2.getStrWidth(line1)) / 2, 30, line1);

    // "Art" — same font, centered below
    const char *line2 = "Moni";
    u8g2.drawStr((128 - u8g2.getStrWidth(line2)) / 2, 58, line2);

    u8g2.sendBuffer();
}

void drawScreen()
{
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);

    u8g2.drawStr(0, 12, "=== Moni ===");

    if (breatheEnabled)
    {
        char buf[32];
        uint32_t secs = breathSecondsRemaining();
        if (breathPhase == BREATH_IN)
        {
            snprintf(buf, sizeof(buf), "Breathing in... %us", (unsigned)secs);
        }
        else
        {
            snprintf(buf, sizeof(buf), "Breathing out... %us", (unsigned)secs);
        }
        u8g2.drawStr(0, 30, buf);
    }
    else
    {
        u8g2.drawStr(0, 30, "Breathing disabled");
    }

    char volBuf[24];
    snprintf(volBuf, sizeof(volBuf), "Vol: %u%%", (unsigned)soundVolume);
    u8g2.drawStr(0, 48, volBuf);
    u8g2.drawStr(0, 62, "[CON to toggle breathing]");

    u8g2.sendBuffer();
}

// ---------------------------------------------------------------------------
// Rotary encoder
// ---------------------------------------------------------------------------
RotaryEncoder encoder;

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------
Button btnPsh;
Button btnBak;
Button btnCon;

void setup()
{
    Serial.begin(115200);

    Wire.begin(PIN_SDA, PIN_SCL);
    u8g2.begin();

    drawSplash();
    delay(3000);

    // Initialize breathing timer since it's enabled by default
    breathPhase = BREATH_IN;
    breathStartMs = millis();

    encoder.begin(PIN_TRA, PIN_TRB);

    initButton(btnPsh, PIN_PSH);
    initButton(btnBak, PIN_BAK);
    initButton(btnCon, PIN_CON);

    initI2S();
    // Audio task on core 0 with high priority to reduce interruptions
    xTaskCreatePinnedToCore(soundTask, "sound", 4096, NULL, 10, NULL, 0);

    drawScreen();

    Serial.println("System ready.");
}

void loop()
{
    // --- Encoder ---
    int8_t delta = encoder.read();

    if (delta != 0)
    {
        // delta is ±1 per detent (physical click); apply fixed 5% step.
        int newVol = (int)soundVolume + (delta * 5);
        soundVolume = (newVol < 0) ? 0 : (newVol > 100) ? 100 : newVol;
    }

    // --- Buttons ---
    updateButton(btnPsh);
    updateButton(btnBak);
    updateButton(btnCon);

    if (btnPsh.pressed)
    {
        btnPsh.pressed = false;
        // PSH button not used
    }

    if (btnBak.pressed)
    {
        btnBak.pressed = false;
        // BAK button not used (only one screen)
    }

    if (btnCon.pressed)
    {
        btnCon.pressed = false;
        // Toggle breathing
        breatheEnabled = !breatheEnabled;
        if (breatheEnabled)
        {
            breathPhase = BREATH_IN;
            breathStartMs = millis();
        }
    }

    // --- Breathing ---
    updateBreath();
    updateLed();

    // --- Display tick ---
    static uint32_t lastDrawMs = 0;
    static uint32_t lastDebugMs = 0;
    if (millis() - lastDrawMs >= DISPLAY_TICK_MS)
    {
        lastDrawMs = millis();
        drawScreen();
    }

    // Debug encoder
    if (millis() - lastDebugMs >= 1000)
    {
        lastDebugMs = millis();
        Serial.printf("Encoder: interrupts=%lu TRA=%d TRB=%d\n",
                      encoder.interruptCount(), digitalRead(PIN_TRA), digitalRead(PIN_TRB));
    }
}
