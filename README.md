# ESP32 Hello World — PlatformIO

A basic example for the **YD-ESP32-23** board that flashes the onboard RGB LED in red and blue like a police car light bar.

The firmware alternates three quick red flashes and three quick blue flashes in a continuous loop, using the built-in NeoPixel on GPIO 48 and the `neopixelWrite()` helper provided by the Arduino-ESP32 framework.

- **Board:** YD-ESP32-23 (ESP32-S3 DevKitC-1 clone)
- **Flash:** 16 MB
- **PSRAM:** 8 MB (OPI)
- **Framework:** Arduino via PlatformIO

> New to PlatformIO? Start here. This guide walks you through setup, the physical board, and how to build and run your first test.

---

## Table of Contents

1. [Prerequisites](#1-prerequisites)
2. [Install PlatformIO](#2-install-platformio)
3. [Physical setup — which USB port to use](#3-physical-setup--which-usb-port-to-use)
4. [Find your serial port and configure platformio.ini](#4-find-your-serial-port-and-configure-platformioini)
5. [Build and upload](#5-build-and-upload)
6. [Running tests](#6-running-tests)
7. [Hardware wiring — LED strip (IRLZ44N MOSFET)](#7-hardware-wiring--led-strip-irlz44n-mosfet)
8. [Updating the LED envelope after changing audio files](#8-updating-the-led-envelope-after-changing-audio-files)
9. [Project structure](#9-project-structure)
10. [AI coding agent setup](#10-ai-coding-agent-setup)

---

## 1. Prerequisites

- A computer running macOS, Linux, or Windows
- Python 3.8 or newer (required by PlatformIO)
- A USB-C cable (data-capable, not charge-only)
- The YD-ESP32-23 board

---

## 2. Install PlatformIO

PlatformIO can be used as a VS Code extension or as a standalone CLI tool. Both are supported.

**VS Code extension (recommended for beginners):**

1. Install [Visual Studio Code](https://code.visualstudio.com/)
2. Open the Extensions panel and search for **PlatformIO IDE**
3. Install it — the `pio` CLI becomes available automatically in the integrated terminal

**CLI only:**

```bash
pip install platformio
```

Verify the installation:

```bash
pio --version
```

---

## 3. Physical setup — which USB port to use

The YD-ESP32-23 has **two USB-C connectors** on the board. Using the wrong one is the most common source of confusion.

```
┌─────────────────────────────────────┐
│          YD-ESP32-23                │
│                                     │
│  [UART] ← use this one              │
│  [USB]  ← do NOT use for flashing   │
│                                     │
└─────────────────────────────────────┘
```

| Connector label | Chip behind it | When to use |
|-----------------|----------------|-------------|
| `UART`          | CH343P (dedicated USB-to-UART bridge) | **Always** — for upload, tests, and serial monitor |
| `USB`           | ESP32-S3 built-in USB peripheral | Not needed for this workflow |

**Always plug into the `UART` connector.**

The `UART` connector uses a CH343P chip — a separate, dedicated bridge that stays alive regardless of what the ESP32-S3 is doing (including resets and flashing). This is critical for test runs: after the board is flashed it resets, and if the serial port disappears during that reset `pio test` will hang waiting for output that never arrives. The CH343P port does not disappear.

The `USB` connector exposes the ESP32-S3's built-in USB peripheral directly. It drops off the bus during resets, which breaks the test runner.

---

## 4. Find your serial port and configure platformio.ini

PlatformIO needs to know which serial port your board is connected to. You must set this once for your machine.

### On macOS / Linux

List ports before and after plugging in the board to identify the new one:

```bash
# Before plugging in
ls /dev/cu.*

# Plug in the UART connector, then run again
ls /dev/cu.*
```

The new entry is your board's port. It will look something like:

```
/dev/cu.usbmodem5ABA0887541   # macOS (CH343P)
/dev/ttyUSB0                  # Linux (CH343P)
```

### On Windows

Open **Device Manager**, expand **Ports (COM & LPT)**, and look for the new COM port that appears when you plug in the board (e.g. `COM3`).

### Update platformio.ini

Open `platformio.ini` and replace the port values in the `[env:yd_esp32_23]` section:

```ini
upload_port = /dev/cu.usbmodem5ABA0887541   ; <- replace with your port
test_port   = /dev/cu.usbmodem5ABA0887541   ; <- same port, replace here too
```

On Windows it would be:

```ini
upload_port = COM3
test_port   = COM3
```

> Both `upload_port` and `test_port` should point to the same port — the CH343P `UART` connector.

---

## 5. Build and upload

Build the project (compiles without uploading):

```bash
pio run
```

Build and flash to the board:

```bash
pio run --target upload
```

Open the serial monitor to see output:

```bash
pio device monitor
```

Press `Ctrl+C` to exit the monitor.

---

## 6. Running tests

Tests live in `test/`. PlatformIO supports two environments:

| Environment | Runs on | Board required |
|-------------|---------|----------------|
| `native`    | Your computer | No |
| `yd_esp32_23` | The physical board | Yes, connected via `UART` |

```bash
# Run logic tests on your computer (no board needed)
pio test -e native

# Run hardware tests on the board (board must be connected)
pio test -e yd_esp32_23
```

See [how_to_unit_test.md](how_to_unit_test.md) for a full guide on writing and running tests.

---

## 7. Hardware wiring — LED strip (IRLZ44N MOSFET)

The LED strip is driven by an **IRLZ44N** logic-level N-channel MOSFET controlled via PWM on **GPIO 1** (5 kHz, 8-bit). The breathing animation varies the duty cycle from 0–100 % over each breath cycle.

### Schematic

```
12V PSU (+) ──────────────────────────────────── LED Strip (+)
                                                  LED Strip (–) ─── IRLZ44N Drain (pin 2)
12V PSU (–) ─── GND rail ──────────────────────── IRLZ44N Source (pin 3)
                    │
                    ├──── ESP32 GND
                    │
                    └──── 10 kΩ ──── IRLZ44N Gate (pin 1)   ← pull-down

ESP32 GPIO 1 ──── 330 Ω ──── IRLZ44N Gate (pin 1)
```

### Component notes

| Component | Value | Purpose |
|-----------|-------|---------|
| Gate series resistor | 330 Ω | Limits GPIO inrush current on each switching edge |
| Gate pull-down resistor | 10 kΩ (Gate → GND) | Keeps MOSFET off if GPIO 1 floats during boot/reset |
| Common ground | Required | 12V PSU GND and ESP32 GND must share the same rail |

The IRLZ44N is a logic-level MOSFET (Vgs(th) ≈ 1–2 V), so 3.3 V from the ESP32 drives it fully on (Rds(on) ≈ 22 mΩ). No level shifter is needed. LED strips are resistive loads, so no flyback diode is required.

---

## 8. Updating the LED envelope after changing audio files

The LED brightness follows the actual amplitude envelope of the breathing audio. The envelope is pre-computed from the WAV files at development time and stored in `include/led_envelope.h`.

**If you replace `data/breath_in.wav` or `data/breath_out.wav`, regenerate the header before building:**

```bash
# Copy your new WAV files into audio/ as well (audio/ is the source, data/ goes to SPIFFS)
cp data/breath_in.wav  audio/breath_in.wav
cp data/breath_out.wav audio/breath_out.wav

# Regenerate the LED envelope header
python3 tools/gen_led_envelope.py
```

The script reads the files in `audio/`, computes the RMS amplitude in 50 ms windows, normalises the peak to 255, and overwrites `include/led_envelope.h`. Then rebuild and flash as normal.

**Requirements:** Python 3 standard library only (`wave`, `struct`, `math`). No extra packages needed.

**WAV format the script expects:**
- 16 kHz sample rate, 16-bit signed PCM, mono
- `breath_in.wav`: up to 4 s
- `breath_out.wav`: up to 6 s

---

## 9. Project structure

```
.
├── platformio.ini                      # Board, framework, and environment config
├── AGENTS.md                           # AI coding agent guidelines (see below)
├── src/
│   └── main.cpp                        # Application entry point
├── include/
│   └── led_envelope.h                  # Auto-generated LED brightness tables (run tools/gen_led_envelope.py)
├── audio/                              # Source WAV files (input to gen_led_envelope.py)
├── data/                               # SPIFFS root — uploaded to flash with `pio run --target uploadfs`
├── tools/
│   └── gen_led_envelope.py             # Analyses audio files and writes include/led_envelope.h
├── test/
│   ├── test_basic/                     # Logic tests — run native and on device
│   └── test_rgb_led/                   # RGB LED smoke tests — device only
└── partitions/
    └── partitions_16MB_psram.csv       # Custom partition table for 16 MB flash
```

---

## 10. AI coding agent setup

This project is developed with **[OpenCode](https://opencode.ai)**, an AI coding assistant that automatically loads `AGENTS.md` as a system-level instruction file for the agent working in this repo.

`AGENTS.md` contains board-specific rules, build constraints, partition table requirements, USB serial flags, and testing conventions that the agent must follow. It is the single source of truth for how code should be written and validated in this project.

**Using Claude Code instead?** Rename `AGENTS.md` to `CLAUDE.md`. Claude Code reads `CLAUDE.md` from the project root and loads it as agent context automatically. The content is identical — only the filename needs to change.

| Tool | Instruction file |
|------|-----------------|
| OpenCode | `AGENTS.md` |
| Claude Code | `CLAUDE.md` |

### Key configuration notes

**Custom partition table** (`partitions/partitions_16MB_psram.csv`): required because the default partition table does not include an `otadata` partition at `0xe000`. Without it, `esptool` corrupts the boot process and the board panics before `setup()` ever runs. It also includes a `coredump` partition required by the ESP32-S3 crash handler.

**USB serial build flags** (in `platformio.ini`):

```ini
board_build.extra_flags =
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1
```

These flags redirect Arduino's `Serial` object to the USB CDC interface so that serial output is visible from boot. Without them, `Serial.print()` goes to UART0 — a hardware UART pin that nothing is connected to — and `pio test` hangs forever waiting for output.

> Side effect: `Serial.flush()` becomes blocking when no serial monitor is open. Do not leave firmware running `Serial.flush()` without a connected monitor.
