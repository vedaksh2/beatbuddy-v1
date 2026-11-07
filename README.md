# BeatBuddy v1

**Your little audio buddy between your phone and your buds.**

BeatBuddy v1 is a capacitive-touch desktop audio DSP and media controller powered by an ESP32 and a 2.8" ILI9341 color TFT display. It features an animated vinyl record player with a reactive cat mascot named **Beat**, a real-time 5-band biquad equalizer, and customizable audio enhancement effects.

---

---

## Features

* **PLAY Page:**
  * Spinning vinyl record with dynamic sheen effects and a lowering tonearm.
  * **Beat the Cat:** An animated mascot on the record label that bounces to the music, falls asleep when paused, and reacts with hearts when you tap ("boop") the record.
  * Scrolling song title marquee, live mini-equalizer bars, transport controls, and a color-graded volume staircase.
* **EQ Page:**
  * Real-time frequency-response curve rendered smoothly using biquad filter math.
  * 5 draggable glowing frequency nodes (`80Hz`, `250Hz`, `1kHz`, `4kHz`, `12kHz`) plus 5 built-in presets (*Flat, Bass, Vocal, Treble, Chill*).
* **FX Page:**
  * Interactive rotary dial knobs for **Bass Boost** and **Stereo Width**.
  * Quick toggles for **Loudness**, **Night Mode** (dynamic peak compression), and **Mono** downmixing.
* **SETUP Page:**
  * Live connection indicators for Phone and Headphones.
  * Screen brightness slider, sleep timer (`Off`, `15m`, `30m`, `60m`), and 4 selectable UI color themes matching Beat's colors.
  * Automatic saving to non-volatile flash memory (`Preferences`).

---

## Hardware & Wiring

| Component | ESP32 Pin | Protocol |
| :--- | :--- | :--- |
| **ILI9341 TFT CS** | `GPIO 15` | SPI |
| **ILI9341 TFT D/C** | `GPIO 2` | SPI |
| **ILI9341 TFT RST** | `GPIO 4` | SPI |
| **ILI9341 TFT MOSI** | `GPIO 23` | SPI |
| **ILI9341 TFT SCK** | `GPIO 18` | SPI |
| **FT6206 Touch SDA** | `GPIO 21` | I2C |
| **FT6206 Touch SCL** | `GPIO 22` | I2C |

---

## Dependencies

* `Adafruit GFX Library`
* `Adafruit ILI9341`
* `Adafruit FT6206 Library`
  
## Live Interactive Simulation

Test the full touch interface directly in your browser without needing physical hardware:
* **Run on Wokwi:** [https://wokwi.com/projects/479963205754145793](https://wokwi.com/projects/479963205754145793)
