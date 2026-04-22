#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <driver/i2s.h>
#include <math.h>
#include <SPIFFS.h>
#include <Preferences.h>
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
static const uint16_t LED_MAX_STEP = 30;     // max brightness change per 10 ms tick (slew limiter)
static const uint16_t LED_MIN_DUTY = 2;      // minimum non-zero duty (avoids MOSFET linear region flicker)

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
    PIN_PSH = 6,  // Rotary encoder push button
    PIN_TRA = 5,  // Encoder A (CLK)
    PIN_TRB = 4,  // Encoder B (DT)
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
    HOLD_IN,
    BREATH_OUT,
    HOLD_OUT
};

volatile bool ledEnabled   = true;
volatile bool soundEnabled = true;
volatile BreathPhase breathPhase = BREATH_IN;
volatile uint32_t breathStartMs = 0;
volatile uint8_t soundVolume = 100;      // 0–100 %
volatile uint8_t ledMaxIntensity = 100;  // 0–100 %
volatile uint8_t delayAfterInSec  = 0;  // 0–30 s hold after BREATH_IN
volatile uint8_t delayAfterOutSec = 0;  // 0–30 s hold after BREATH_OUT

enum ScreenId  : uint8_t { SCREEN_MAIN = 0, SCREEN_LED, SCREEN_AUDIO, SCREEN_VOL, SCREEN_BRIGHTNESS, SCREEN_BEND, SCREEN_DELAY };
enum EditState : uint8_t { EDIT_NONE = 0, EDIT_FIRST, EDIT_SECOND };

// Separate bend profile per phase — the inhale and exhale WAVs have different
// loudness shapes, so the "right" amount of slope modulation differs too.
volatile uint8_t currentBendIdxIn  = 0;  // index into kLedEnvelopeInProfiles
volatile uint8_t currentBendIdxOut = 0;  // index into kLedEnvelopeOutProfiles

ScreenId  currentScreen = SCREEN_MAIN;
EditState editState     = EDIT_NONE;

// ---------------------------------------------------------------------------
// Persistent config (NVS via Preferences)
// ---------------------------------------------------------------------------
Preferences prefs;

void loadConfig()
{
    prefs.begin("config", /*readOnly=*/true);
    soundVolume      = prefs.getUChar("vol",    100);
    ledMaxIntensity  = prefs.getUChar("led",    100);
    delayAfterInSec  = prefs.getUChar("dlyIn",  0);
    delayAfterOutSec = prefs.getUChar("dlyOut", 0);
    ledEnabled       = prefs.getBool("ledEn",   true);
    soundEnabled     = prefs.getBool("sndEn",   true);
    // Legacy single-index key used as fallback for both if per-phase keys are unset.
    uint8_t legacy   = prefs.getUChar("bendIdx", 0);
    currentBendIdxIn  = prefs.getUChar("bendIdxIn",  legacy);
    currentBendIdxOut = prefs.getUChar("bendIdxOut", legacy);
    if (currentBendIdxIn  >= LED_ENV_NUM_PROFILES) currentBendIdxIn  = 0;
    if (currentBendIdxOut >= LED_ENV_NUM_PROFILES) currentBendIdxOut = 0;
    prefs.end();
}

void saveConfig()
{
    prefs.begin("config", /*readOnly=*/false);
    prefs.putUChar("vol",     soundVolume);
    prefs.putUChar("led",     ledMaxIntensity);
    prefs.putUChar("dlyIn",   delayAfterInSec);
    prefs.putUChar("dlyOut",  delayAfterOutSec);
    prefs.putBool("ledEn",    ledEnabled);
    prefs.putBool("sndEn",    soundEnabled);
    prefs.putUChar("bendIdxIn",  currentBendIdxIn);
    prefs.putUChar("bendIdxOut", currentBendIdxOut);
    prefs.end();
}

// ---------------------------------------------------------------------------
// Breathing logic
// ---------------------------------------------------------------------------
void updateBreath()
{
    const BreathPhase phase   = breathPhase;
    const uint32_t    elapsed = millis() - breathStartMs;
    BreathPhase next;

    switch (phase)
    {
    case BREATH_IN:
        if (elapsed < BREATH_IN_MS) return;
        next = (delayAfterInSec > 0) ? HOLD_IN : BREATH_OUT;
        break;
    case HOLD_IN:
        if (elapsed < (uint32_t)delayAfterInSec * 1000UL) return;
        next = BREATH_OUT;
        break;
    case BREATH_OUT:
        if (elapsed < BREATH_OUT_MS) return;
        next = (delayAfterOutSec > 0) ? HOLD_OUT : BREATH_IN;
        break;
    case HOLD_OUT:
        if (elapsed < (uint32_t)delayAfterOutSec * 1000UL) return;
        next = BREATH_IN;
        break;
    default:
        return;
    }

    breathStartMs = millis(); // timestamp FIRST — soundTask reads phase as trigger
    __sync_synchronize();     // full memory barrier: Core 0 sees startMs before phase
    breathPhase = next;       // phase SECOND — this is what readers poll
}

void updateLed()
{
    // Snapshot volatile shared state once — prevents mixed-phase reads if updateBreath()
    // preempts this task between two volatile reads within a single call.
    const bool enabled = ledEnabled;
    const BreathPhase phase = breathPhase;
    const uint32_t startMs = breathStartMs;
    const uint8_t maxInt = ledMaxIntensity;
    // Snapshot both so a mid-tick menu change can't pick mismatched pointers.
    const uint8_t bendIdxIn  = currentBendIdxIn;
    const uint8_t bendIdxOut = currentBendIdxOut;

    static uint16_t lastB = 0; // 0xFFFF would cause a spurious spike on first tick with the slew clamp
    if (!enabled)
    {
        if (lastB != 0)
        {
            ledcWrite(LED_PWM_CHANNEL, 0);
            lastB = 0;
        }
        return;
    }

    // Hold phases: maintain LED at phase-boundary brightness without envelope math.
    if (phase == HOLD_IN)
    {
        // Peak brightness — matches last kLedEnvelopeIn sample (255/255 * peak)
        uint16_t b = (uint16_t)(LED_MAX_BRIGHT * (maxInt / 100.0f));
        if (b > 0 && b < LED_MIN_DUTY) b = LED_MIN_DUTY;
        uint16_t maxStep = (lastB > 100) ? LED_MAX_STEP : (uint16_t)((lastB / 5) + 3);
        if (maxStep < 3) maxStep = 3;
        if (b > lastB + maxStep)       b = lastB + maxStep;
        else if (b + maxStep < lastB)  b = lastB - maxStep;
        if (b != lastB) { ledcWrite(LED_PWM_CHANNEL, b); lastB = b; }
        return;
    }
    if (phase == HOLD_OUT)
    {
        // LED at 0 — matches last kLedEnvelopeOut sample (0)
        if (lastB != 0) { ledcWrite(LED_PWM_CHANNEL, 0); lastB = 0; }
        return;
    }

    uint32_t duration = (phase == BREATH_IN) ? BREATH_IN_MS : BREATH_OUT_MS;
    uint32_t elapsed = millis() - startMs;
    if (elapsed > duration)
        elapsed = duration;

    // Use pre-computed RMS envelope derived from the actual audio files.
    // Linear interpolation between adjacent 50 ms envelope samples.
    const uint8_t *env = (phase == BREATH_IN) ? kLedEnvelopeInProfiles[bendIdxIn]
                                               : kLedEnvelopeOutProfiles[bendIdxOut];
    const uint16_t envN = (phase == BREATH_IN) ? LED_ENV_IN_N : LED_ENV_OUT_N;

    float pos = (float)elapsed / duration * (envN - 1);
    uint16_t lo = (uint16_t)pos;
    uint16_t hi = (lo + 1 < envN) ? lo + 1 : lo;
    float frac = pos - lo;
    float val = env[lo] + frac * (env[hi] - env[lo]);

    // Both envelope tables already encode the final perceptual shape
    // (threshold-triggered, average-anchored linear ramp, slope-modulated
    // by the audio's own dBFS). No runtime gamma or inversion needed.
    float norm = val / 255.0f;
    float peak = LED_MAX_BRIGHT * (maxInt / 100.0f);
    uint16_t b = (uint16_t)(norm * peak);

    // Floor: avoid sub-threshold duty where MOSFET operates in linear region
    if (b > 0 && b < LED_MIN_DUTY)
        b = LED_MIN_DUTY;

    // Adaptive slew rate: scale step size down at low brightness to avoid
    // visible jumps (30/50 = 60% is jarring; 3/50 = 6% is smooth).
    uint16_t maxStep = (lastB > 100) ? LED_MAX_STEP
                                     : (uint16_t)((lastB / 5) + 3);
    if (maxStep < 3)
        maxStep = 3;

    if (b > lastB + maxStep)
        b = lastB + maxStep;
    else if (b + maxStep < lastB)
        b = lastB - maxStep;

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
        updateBreath();
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
        .dma_buf_count = 32,
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
// WAV file player — PSRAM-backed for glitch-free playback
//
// Expected format: RIFF PCM, 16-bit signed, mono, 16000 Hz.
// WAV files are pre-loaded into PSRAM at boot to eliminate SPIFFS blocking
// during real-time audio streaming.
//
// Playback stops early if breathPhase changes or breatheEnabled goes false,
// with a short fade-out to prevent audible clicks.
// ---------------------------------------------------------------------------

// Pre-loaded WAV audio data (PCM samples in PSRAM)
static int16_t *wavDataIn = nullptr;
static uint32_t wavLenIn = 0; // sample count
static int16_t *wavDataOut = nullptr;
static uint32_t wavLenOut = 0; // sample count

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
        f.seek(f.position() + chunkSize + (chunkSize & 1));
    }
    return 0;
}

// Load a WAV file from SPIFFS into a PSRAM buffer. Returns sample count.
static uint32_t loadWavToRam(const char *path, int16_t **outBuf)
{
    File f = SPIFFS.open(path);
    if (!f)
    {
        Serial.printf("WAV not found: %s\n", path);
        return 0;
    }

    uint32_t dataOffset = findWavDataOffset(f);
    if (dataOffset == 0)
    {
        Serial.printf("Bad WAV header: %s\n", path);
        f.close();
        return 0;
    }

    f.seek(dataOffset);
    uint32_t dataBytes = f.size() - dataOffset;
    uint32_t samples = dataBytes / 2;

    *outBuf = (int16_t *)ps_malloc(dataBytes);
    if (!*outBuf)
    {
        Serial.printf("PSRAM alloc failed for %s (%u bytes)\n", path, dataBytes);
        f.close();
        return 0;
    }

    f.read((uint8_t *)*outBuf, dataBytes);
    f.close();
    Serial.printf("Loaded %s: %u samples (%u bytes) into PSRAM\n", path, samples, dataBytes);
    return samples;
}

static void initWavBuffers()
{
    wavLenIn = loadWavToRam("/breath_in.wav", &wavDataIn);
    wavLenOut = loadWavToRam("/breath_out.wav", &wavDataOut);
}

static const int FADE_SAMPLES = 64; // 4 ms fade-out at 16 kHz

// Streams pre-loaded WAV samples to I2S with volume scaling.
// Fades out over FADE_SAMPLES when exiting early due to phase change.
static void playWavFromRam(const int16_t *data, uint32_t numSamples, BreathPhase triggerPhase)
{
    if (!data || numSamples == 0)
        return;

    static int16_t stereo[512 * 2];
    uint32_t pos = 0;

    while (pos < numSamples && breathPhase == triggerPhase && soundEnabled)
    {
        uint32_t chunk = numSamples - pos;
        if (chunk > 512)
            chunk = 512;

        float volLinear = soundVolume / 100.0f;
        float volExp = volLinear * volLinear;

        for (uint32_t i = 0; i < chunk; i++)
        {
            int16_t s = (int16_t)(data[pos + i] * volExp);
            stereo[i * 2] = s;
            stereo[i * 2 + 1] = s;
        }
        size_t written;
        i2s_write(I2S_NUM_0, stereo, chunk * 4, &written, portMAX_DELAY);
        pos += chunk;
    }

    // Fade-out if we exited early (phase changed or disabled)
    if (pos < numSamples)
    {
        uint32_t fadeLen = numSamples - pos;
        if (fadeLen > FADE_SAMPLES)
            fadeLen = FADE_SAMPLES;

        float volLinear = soundVolume / 100.0f;
        float volExp = volLinear * volLinear;

        for (uint32_t i = 0; i < fadeLen; i++)
        {
            float fade = 1.0f - (float)i / FADE_SAMPLES;
            int16_t s = (int16_t)(data[pos + i] * volExp * fade);
            stereo[i * 2] = s;
            stereo[i * 2 + 1] = s;
        }
        size_t written;
        i2s_write(I2S_NUM_0, stereo, fadeLen * 4, &written, portMAX_DELAY);
    }
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
        if (!soundEnabled)
        {
            lastPhase = -1;
            writeSilence();
            vTaskDelay(1);
            continue;
        }

        int8_t currentPhase = (int8_t)breathPhase;

        if (currentPhase != lastPhase)
        {
            lastPhase = currentPhase;
            switch ((BreathPhase)currentPhase)
            {
            case BREATH_IN:  playWavFromRam(wavDataIn,  wavLenIn,  BREATH_IN);  break;
            case BREATH_OUT: playWavFromRam(wavDataOut, wavLenOut, BREATH_OUT); break;
            case HOLD_IN:
            case HOLD_OUT:   break; // silence during hold phases
            }
        }
        else
        {
            writeSilence();
            vTaskDelay(1);
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

    char line[22]; // 21 chars + null; 128px / 6px = 21.3 chars max

    switch (currentScreen)
    {
    case SCREEN_MAIN:
        snprintf(line, sizeof(line), "Audio:%-3s  LED:%-3s",
                 soundEnabled ? "ON" : "OFF", ledEnabled ? "ON" : "OFF");
        u8g2.drawStr(0, 12, line);
        snprintf(line, sizeof(line), "Vol:%u%%  Br:%u%%",
                 (unsigned)soundVolume, (unsigned)ledMaxIntensity);
        u8g2.drawStr(0, 26, line);
        snprintf(line, sizeof(line), "DlyIn:%us DlyOut:%us",
                 (unsigned)delayAfterInSec, (unsigned)delayAfterOutSec);
        u8g2.drawStr(0, 40, line);
        snprintf(line, sizeof(line), "Bend I:%s O:%s",
                 kLedEnvelopeProfileLabels[currentBendIdxIn],
                 kLedEnvelopeProfileLabels[currentBendIdxOut]);
        u8g2.drawStr(0, 54, line);
        break;

    case SCREEN_LED:
        u8g2.drawStr(0, 12, "LED");
        snprintf(line, sizeof(line), " >[%s]", ledEnabled ? "ON" : "OFF");
        u8g2.drawStr(0, 26, line);
        u8g2.drawStr(0, 54, "PUSH=toggle");
        break;

    case SCREEN_AUDIO:
        u8g2.drawStr(0, 12, "Audio");
        snprintf(line, sizeof(line), " >[%s]", soundEnabled ? "ON" : "OFF");
        u8g2.drawStr(0, 26, line);
        snprintf(line, sizeof(line), "In:%s Out:%s",
                 wavLenIn  > 0 ? "OK" : "MISSING",
                 wavLenOut > 0 ? "OK" : "MISSING");
        u8g2.drawStr(0, 40, line);
        u8g2.drawStr(0, 54, "PUSH=toggle");
        break;

    case SCREEN_VOL:
        u8g2.drawStr(0, 12, "Volume");
        if (editState == EDIT_NONE)
        {
            snprintf(line, sizeof(line), "  %u%%", (unsigned)soundVolume);
            u8g2.drawStr(0, 26, line);
            u8g2.drawStr(0, 54, "PUSH=edit");
        }
        else
        {
            snprintf(line, sizeof(line), " [%u%%]", (unsigned)soundVolume);
            u8g2.drawStr(0, 26, line);
            u8g2.drawStr(0, 54, "PUSH=done");
        }
        break;

    case SCREEN_BRIGHTNESS:
        u8g2.drawStr(0, 12, "Brightness");
        if (editState == EDIT_NONE)
        {
            snprintf(line, sizeof(line), "  %u%%", (unsigned)ledMaxIntensity);
            u8g2.drawStr(0, 26, line);
            u8g2.drawStr(0, 54, "PUSH=edit");
        }
        else
        {
            snprintf(line, sizeof(line), " [%u%%]", (unsigned)ledMaxIntensity);
            u8g2.drawStr(0, 26, line);
            u8g2.drawStr(0, 54, "PUSH=done");
        }
        break;

    case SCREEN_BEND:
    {
        u8g2.drawStr(0, 12, "Bend");
        const char *inLbl  = kLedEnvelopeProfileLabels[currentBendIdxIn];
        const char *outLbl = kLedEnvelopeProfileLabels[currentBendIdxOut];
        snprintf(line, sizeof(line), (editState == EDIT_FIRST)  ? "In:  [%s]" : "In:   %s",  inLbl);
        u8g2.drawStr(0, 26, line);
        snprintf(line, sizeof(line), (editState == EDIT_SECOND) ? "Out: [%s]" : "Out:  %s",  outLbl);
        u8g2.drawStr(0, 40, line);
        const char *footer =
            (editState == EDIT_NONE)  ? "PUSH=edit in"  :
            (editState == EDIT_FIRST) ? "PUSH=edit out" :
                                        "PUSH=done";
        u8g2.drawStr(0, 54, footer);
        break;
    }

    case SCREEN_DELAY:
        u8g2.drawStr(0, 12, "Breath Delay");
        if (editState == EDIT_NONE)
        {
            snprintf(line, sizeof(line), "After in:  %us", (unsigned)delayAfterInSec);
            u8g2.drawStr(0, 26, line);
            snprintf(line, sizeof(line), "After out: %us", (unsigned)delayAfterOutSec);
            u8g2.drawStr(0, 40, line);
            u8g2.drawStr(0, 54, "PUSH=edit in");
        }
        else if (editState == EDIT_FIRST)
        {
            snprintf(line, sizeof(line), "After in: [%us]", (unsigned)delayAfterInSec);
            u8g2.drawStr(0, 26, line);
            snprintf(line, sizeof(line), "After out: %us", (unsigned)delayAfterOutSec);
            u8g2.drawStr(0, 40, line);
            u8g2.drawStr(0, 54, "PUSH=edit out");
        }
        else // EDIT_SECOND
        {
            snprintf(line, sizeof(line), "After in:  %us", (unsigned)delayAfterInSec);
            u8g2.drawStr(0, 26, line);
            snprintf(line, sizeof(line), "After out:[%us]", (unsigned)delayAfterOutSec);
            u8g2.drawStr(0, 40, line);
            u8g2.drawStr(0, 54, "PUSH=done");
        }
        break;
    }

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

    loadConfig();

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

    initWavBuffers(); // pre-load WAV files into PSRAM for glitch-free playback
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
        if (editState != EDIT_NONE)
        {
            // Adjust the active edit field
            switch (currentScreen)
            {
            case SCREEN_VOL:
            {
                int v = (int)soundVolume + (delta * 5);
                soundVolume = (v < 0) ? 0 : (v > 100) ? 100 : (uint8_t)v;
                break;
            }
            case SCREEN_BRIGHTNESS:
            {
                int v = (int)ledMaxIntensity + (delta * 5);
                ledMaxIntensity = (v < 0) ? 0 : (v > 100) ? 100 : (uint8_t)v;
                break;
            }
            case SCREEN_BEND:
            {
                const int n = LED_ENV_NUM_PROFILES;
                volatile uint8_t *target = (editState == EDIT_FIRST) ? &currentBendIdxIn
                                                                     : &currentBendIdxOut;
                int v = ((int)*target + delta) % n;
                if (v < 0) v += n;
                *target = (uint8_t)v;
                break;
            }
            case SCREEN_DELAY:
                if (editState == EDIT_FIRST)
                {
                    int v = (int)delayAfterInSec + delta;
                    delayAfterInSec = (v < 0) ? 0 : (v > 30) ? 30 : (uint8_t)v;
                }
                else // EDIT_SECOND
                {
                    int v = (int)delayAfterOutSec + delta;
                    delayAfterOutSec = (v < 0) ? 0 : (v > 30) ? 30 : (uint8_t)v;
                }
                break;
            default:
                break;
            }
        }
        else
        {
            // Navigate: MAIN ↔ LED ↔ AUDIO ↔ VOL ↔ BRIGHTNESS ↔ BEND ↔ DELAY (wraps)
            int8_t next = (int8_t)currentScreen + (delta > 0 ? 1 : -1);
            if (next < (int8_t)SCREEN_MAIN)   next = (int8_t)SCREEN_DELAY;
            if (next > (int8_t)SCREEN_DELAY)  next = (int8_t)SCREEN_MAIN;
            currentScreen = (ScreenId)next;
        }
    }

    // --- Buttons ---
    updateButton(btnPsh);
    updateButton(btnBak);
    updateButton(btnCon);

    if (btnPsh.pressed)
    {
        btnPsh.pressed = false;

        if (currentScreen == SCREEN_LED)
        {
            ledEnabled = !ledEnabled;
            saveConfig();
        }
        else if (currentScreen == SCREEN_AUDIO)
        {
            soundEnabled = !soundEnabled;
            saveConfig();
        }
        else if (currentScreen == SCREEN_VOL || currentScreen == SCREEN_BRIGHTNESS)
        {
            // Toggle EDIT_NONE <-> EDIT_FIRST; save when exiting edit
            if (editState == EDIT_NONE)
                editState = EDIT_FIRST;
            else
            {
                editState = EDIT_NONE;
                saveConfig();
            }
        }
        else if (currentScreen == SCREEN_BEND || currentScreen == SCREEN_DELAY)
        {
            // Cycle EDIT_NONE -> EDIT_FIRST -> EDIT_SECOND -> EDIT_NONE; save when done
            if      (editState == EDIT_NONE)   editState = EDIT_FIRST;
            else if (editState == EDIT_FIRST)  editState = EDIT_SECOND;
            else
            {
                editState = EDIT_NONE;
                saveConfig();
            }
        }
    }

    if (btnBak.pressed)
        btnBak.pressed = false;

    if (btnCon.pressed)
    {
        btnCon.pressed = false; /* unused */
    }

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
