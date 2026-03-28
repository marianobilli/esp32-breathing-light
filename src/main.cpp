#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <driver/i2s.h>
#include <math.h>
#include <SPIFFS.h>
#include "RotaryEncoder.h"
#include "led_envelope.h"

// ---------------------------------------------------------------------------
// Pin definitions
// ---------------------------------------------------------------------------
static const uint8_t LED_PWM_PIN = 1;
static const uint8_t LED_PWM_CHANNEL = 0;
static const uint32_t LED_PWM_FREQ = 25000;  // 25 kHz — above audio band to reduce audible noise
static const uint8_t LED_PWM_BITS = 10;      // 0–1023 duty range (finer steps at low brightness)
static const uint16_t LED_MAX_BRIGHT = 1023; // full 10-bit range
static const uint16_t LED_MAX_STEP   = 30;   // max brightness change per 10 ms tick (slew limiter)

// ---------------------------------------------------------------------------
// I2S / PCM5102 pin definitions
// ---------------------------------------------------------------------------
static const uint8_t I2S_BCLK_PIN = 2;  // BCK
static const uint8_t I2S_WS_PIN = 41;   // LCK
static const uint8_t I2S_DOUT_PIN = 42; // DIN
static const uint32_t I2S_SAMPLE_RATE = 16000;

// ---------------------------------------------------------------------------
// Pin definitions
// ---------------------------------------------------------------------------
enum Pins : uint8_t
{
    PIN_SDA = 14,
    PIN_SCL = 13,
    PIN_CON = 21, // Confirm button
    PIN_PSH = 4,  // Rotary encoder push button
    PIN_TRA = 5,  // Encoder A (CLK)
    PIN_TRB = 6,  // Encoder B (DT)
    PIN_BAK = 38, // Back button
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
enum BreathPhase : uint8_t
{
    BREATH_IN,
    BREATH_OUT
};

static const uint32_t BREATH_IN_MS = 4000;
static const uint32_t BREATH_OUT_MS = 6000;

volatile bool breatheEnabled = true;
volatile bool soundEnabled = true;
volatile BreathPhase breathPhase = BREATH_IN;
volatile uint32_t breathStartMs = 0;
volatile uint8_t soundVolume = 100;     // 0–100 %
volatile uint8_t ledMaxIntensity = 100; // 0–100 %

enum MenuState : uint8_t
{
    BROWSE,
    EDIT
};
MenuState menuState = BROWSE;
uint8_t selectedItem = 0; // 0 = Vol, 1 = LED, 2 = ON/OFF

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

void updateLed()
{
    static uint16_t lastB = 0; // 0xFFFF would cause a spurious spike on first tick with the slew clamp
    if (!breatheEnabled)
    {
        if (lastB != 0) { ledcWrite(LED_PWM_CHANNEL, 0); lastB = 0; }
        return;
    }

    uint32_t duration = (breathPhase == BREATH_IN) ? BREATH_IN_MS : BREATH_OUT_MS;
    uint32_t elapsed = millis() - breathStartMs;
    if (elapsed > duration)
        elapsed = duration;

    // Use pre-computed RMS envelope derived from the actual audio files.
    // Linear interpolation between adjacent 50 ms envelope samples.
    const uint8_t *env = (breathPhase == BREATH_IN) ? kLedEnvelopeIn : kLedEnvelopeOut;
    const uint8_t envN = (breathPhase == BREATH_IN) ? 80 : 120;

    float pos = (float)elapsed / duration * (envN - 1);
    uint8_t lo = (uint8_t)pos;
    uint8_t hi = (lo + 1 < envN) ? lo + 1 : lo;
    float frac = pos - lo;
    float val = env[lo] + frac * (env[hi] - env[lo]);

    // kLedEnvelopeIn  is ascending  0→255: norm goes 0→1, gamma 2.2 for perceptual linearity.
    // kLedEnvelopeOut is descending 255→0: norm goes 1→0, used directly (no inversion, no
    // gamma) so the LED remains visibly lit throughout the exhalation and reaches exactly
    // zero at the end of the phase.
    float norm = val / 255.0f;
    float peak = LED_MAX_BRIGHT * (ledMaxIntensity / 100.0f);
    uint16_t b = (breathPhase == BREATH_IN)
                     ? (uint16_t)(powf(norm, 2.2f) * peak)
                     : (uint16_t)(norm * peak);

    // Slew rate clamp — prevents large jumps even if timing drifts briefly.
    if (b > lastB + LED_MAX_STEP)      b = lastB + LED_MAX_STEP;
    else if (b + LED_MAX_STEP < lastB) b = lastB - LED_MAX_STEP;

    if (b != lastB)
    {
        ledcWrite(LED_PWM_CHANNEL, b);
        lastB = b;
    }
}

// ---------------------------------------------------------------------------
// LED task — fixed 10 ms cadence on Core 1, decoupled from display I2C blocking
// ---------------------------------------------------------------------------
void ledTask(void * /*param*/)
{
    TickType_t lastWake = xTaskGetTickCount();
    while (true)
    {
        updateLed();
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(10));
    }
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
        .dma_buf_count = 16,
        .dma_buf_len = 128,
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

// ---------------------------------------------------------------------------
// WAV file player
//
// Expected format: RIFF PCM, 16-bit signed, mono, 16000 Hz.
// The player parses the WAV chunk headers to find the 'data' chunk, so
// files with non-standard header sizes (e.g. LIST/INFO chunks) are handled.
//
// Playback stops early if breathPhase changes or breatheEnabled goes false,
// so the next phase's audio can start immediately.
// ---------------------------------------------------------------------------
// Opens a WAV from SPIFFS. Returns an invalid (falsy) File on failure.
static File openWavFile(const char *path)
{
    File f = SPIFFS.open(path);
    if (!f)
        Serial.printf("WAV not found in SPIFFS: %s\n", path);
    return f;
}

// Scans RIFF chunks starting after the WAVE FourCC to find the 'data' chunk.
// Returns the file offset of the first audio sample, or 0 on failure.
static uint32_t findWavDataOffset(File &f)
{
    uint8_t buf[8];
    f.seek(12); // skip "RIFF", file-size, "WAVE"
    while (f.available() >= 8)
    {
        if (f.read(buf, 8) != 8)
            break;
        uint32_t chunkSize = buf[4] | ((uint32_t)buf[5] << 8) |
                             ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
        if (memcmp(buf, "data", 4) == 0)
            return f.position();
        // RIFF chunks are word-aligned; skip this chunk
        f.seek(f.position() + chunkSize + (chunkSize & 1));
    }
    return 0;
}

// Streams WAV samples to I2S with volume scaling. Exits early if the breath
// phase changes so the new phase's audio can start without delay.
static void playWavFile(const char *path, BreathPhase triggerPhase)
{
    File f = openWavFile(path);
    if (!f)
        return;

    uint32_t dataOffset = findWavDataOffset(f);
    if (dataOffset == 0)
    {
        Serial.printf("Bad WAV header: %s\n", path);
        f.close();
        return;
    }
    f.seek(dataOffset);

    // Static buffers: avoids putting ~3 KB on the FreeRTOS task stack.
    // Safe because playWavFile is only ever called from soundTask (no reentrancy).
    static int16_t mono[512];
    static int16_t stereo[512 * 2];

    while (f.available() && breathPhase == triggerPhase && breatheEnabled && soundEnabled)
    {
        int bytesRead = f.read((uint8_t *)mono, sizeof(mono));
        if (bytesRead <= 0)
            break;

        int samplesRead = bytesRead / 2;
        float volLinear = soundVolume / 100.0f;
        float volExp = volLinear * volLinear; // square for perceptual loudness

        for (int i = 0; i < samplesRead; i++)
        {
            int16_t s = (int16_t)(mono[i] * volExp);
            stereo[i * 2] = s;
            stereo[i * 2 + 1] = s;
        }
        size_t written;
        i2s_write(I2S_NUM_0, stereo, samplesRead * 4, &written, portMAX_DELAY);
    }
    f.close();
}

// Sends one buffer of silence to keep the I2S clock alive.
static void writeSilence()
{
    static int16_t silBuf[128 * 2]; // zeroed by BSS init; never written
    size_t written;
    i2s_write(I2S_NUM_0, silBuf, sizeof(silBuf), &written, portMAX_DELAY);
}

// ---------------------------------------------------------------------------
// Sound task
//
// Detects breath-phase transitions and plays the matching WAV file.
// Between clips (after the WAV ends but before the next phase starts) it
// streams silence so the I2S peripheral keeps its clock running.
// ---------------------------------------------------------------------------
void soundTask(void * /*param*/)
{
    int8_t lastPhase = -1; // -1 = uninitialized / not yet started

    while (true)
    {
        if (!breatheEnabled || !soundEnabled)
        {
            lastPhase = -1;
            writeSilence();
            continue;
        }

        int8_t currentPhase = (int8_t)breathPhase;

        if (currentPhase != lastPhase)
        {
            lastPhase = currentPhase;
            const char *filename = (breathPhase == BREATH_IN)
                                       ? "/breath_in.wav"
                                       : "/breath_out.wav";
            playWavFile(filename, breathPhase);
            // Fall through to silence loop after WAV ends
        }
        else
        {
            writeSilence();
        }
    }
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------
static const uint32_t DISPLAY_TICK_MS = 500;

void drawScreen()
{
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);

    // Menu rows — ">" cursor in BROWSE, brackets around value in EDIT
    char rowVol[24], rowLed[24], rowOnOff[24];

    if (menuState == BROWSE)
    {
        snprintf(rowVol, sizeof(rowVol), "%cVol: %u%%",
                 selectedItem == 0 ? '>' : ' ', (unsigned)soundVolume);
        snprintf(rowLed, sizeof(rowLed), "%cLED: %u%%",
                 selectedItem == 1 ? '>' : ' ', (unsigned)ledMaxIntensity);
        snprintf(rowOnOff, sizeof(rowOnOff), "%cON/OFF: %s",
                 selectedItem == 2 ? '>' : ' ', breatheEnabled ? "ON" : "OFF");
    }
    else
    {
        snprintf(rowVol, sizeof(rowVol), "%cVol: %s%u%%%s",
                 selectedItem == 0 ? '>' : ' ',
                 selectedItem == 0 ? "[" : "", (unsigned)soundVolume,
                 selectedItem == 0 ? "]" : "");
        snprintf(rowLed, sizeof(rowLed), "%cLED: %s%u%%%s",
                 selectedItem == 1 ? '>' : ' ',
                 selectedItem == 1 ? "[" : "", (unsigned)ledMaxIntensity,
                 selectedItem == 1 ? "]" : "");
        snprintf(rowOnOff, sizeof(rowOnOff), "%cON/OFF: %s",
                 selectedItem == 2 ? '>' : ' ', breatheEnabled ? "ON" : "OFF");
    }
    u8g2.drawStr(0, 14, rowVol);
    u8g2.drawStr(0, 28, rowLed);
    u8g2.drawStr(0, 42, rowOnOff);

    u8g2.drawStr(0, 64, menuState == BROWSE ? "[PUSH=ok BACK=exit]" : "[BAK=done]");

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

    // I2C scanner — remove after display confirmed working
    Serial.println("Scanning I2C bus...");
    for (uint8_t addr = 1; addr < 127; addr++)
    {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0)
            Serial.printf("  I2C device at 0x%02X\n", addr);
    }
    Serial.println("Scan done.");

    Serial.println("u8g2 begin...");
    u8g2.begin();
    Serial.println("u8g2 ok.");

    breathPhase = BREATH_IN;
    breathStartMs = millis();

    Serial.println("encoder begin...");
    encoder.begin(PIN_TRA, PIN_TRB);
    Serial.println("encoder ok.");

    Serial.println("buttons begin...");
    initButton(btnPsh, PIN_PSH);
    initButton(btnBak, PIN_BAK);
    initButton(btnCon, PIN_CON);
    Serial.println("buttons ok.");

    ledcSetup(LED_PWM_CHANNEL, LED_PWM_FREQ, LED_PWM_BITS);
    ledcAttachPin(LED_PWM_PIN, LED_PWM_CHANNEL);
    ledcWrite(LED_PWM_CHANNEL, 0); // start off

    // SPIFFS — format on first boot if needed
    if (SPIFFS.begin(true))
        Serial.println("SPIFFS mounted.");
    else
        Serial.println("SPIFFS mount failed.");

    initI2S();
    // Audio task on core 0 with high priority to reduce interruptions
    xTaskCreatePinnedToCore(soundTask, "sound", 8192, NULL, 10, NULL, 0);

    // LED task on core 1 at priority 5 — runs every 10 ms regardless of display blocking
    xTaskCreatePinnedToCore(ledTask, "led", 2048, NULL, 5, NULL, 1);

    drawScreen();

    Serial.println("System ready.");
}

void loop()
{
    // --- Encoder ---
    int8_t delta = encoder.read();

    if (delta != 0)
    {
        if (menuState == BROWSE)
        {
            // Navigate between items (wrap 0–2)
            int8_t next = (int8_t)selectedItem + (delta > 0 ? 1 : -1);
            if (next < 0)
                next = 2;
            else if (next > 2)
                next = 0;
            selectedItem = (uint8_t)next;
        }
        else
        {
            // Edit selected value in 5% steps (item 2 is ON/OFF, not editable by encoder)
            if (selectedItem == 0)
            {
                int v = (int)soundVolume + (delta * 5);
                soundVolume = (v < 0) ? 0 : (v > 100) ? 100
                                                      : (uint8_t)v;
            }
            else if (selectedItem == 1)
            {
                int v = (int)ledMaxIntensity + (delta * 5);
                ledMaxIntensity = (v < 0) ? 0 : (v > 100) ? 100
                                                          : (uint8_t)v;
            }
        }
    }

    // --- Buttons ---
    updateButton(btnPsh);
    updateButton(btnBak);
    updateButton(btnCon);

    if (btnPsh.pressed)
    {
        btnPsh.pressed = false;
        if (menuState == BROWSE)
        {
            if (selectedItem == 2)
            {
                // Toggle ON/OFF
                breatheEnabled = !breatheEnabled;
                if (breatheEnabled)
                {
                    breathPhase = BREATH_IN;
                    breathStartMs = millis();
                }
            }
            else
            {
                menuState = EDIT;
            }
        }
    }

    if (btnBak.pressed)
    {
        btnBak.pressed = false;
        menuState = BROWSE;
    }

    if (btnCon.pressed)
    {
        btnCon.pressed = false; /* unused */
    }

    // --- Breathing ---
    updateBreath();

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
