# ⚡ SPUMCS
### Smart Per-device Usage & Monitoring Control System

An Arduino-based energy monitoring system that tracks per-device power
consumption across two simulated rooms, automatically sheds load during
an overload condition, and displays live status on an I2C LCD — built
and simulated entirely in Tinkercad.

<p>
  <img alt="Platform" src="https://img.shields.io/badge/platform-Arduino%20Uno-00979D?logo=arduino&logoColor=white">
  <img alt="Simulated in" src="https://img.shields.io/badge/simulated%20in-Tinkercad-1E88E5">
  <img alt="Language" src="https://img.shields.io/badge/language-C%2B%2B-00599C?logo=cplusplus&logoColor=white">
  <img alt="Status" src="https://img.shields.io/badge/status-working-brightgreen">
  <img alt="License" src="https://img.shields.io/badge/license-MIT-lightgrey">
</p>

---

## 📸 Screenshot

<!-- Replace this with an actual screenshot or GIF of the running circuit,
     e.g. media/screenshot.png or media/demo.gif -->
<p align="center">
  <img src="https://i.ibb.co/Q7J5T8Dv/Improved-Power-Consumption-Monitor.png" alt="SPUMCS circuit running in Tinkercad" width="800">
</p>

---

## 🧠 What it does

SPUMCS simulates a two-room household electrical panel: two breakers,
seven individually-switched devices, and a full monitoring/control layer
on top:

- **Per-device power & energy tracking** — instantaneous wattage and
  cumulative kWh for every device, room, and the whole house
- **Automatic overload protection** — sheds the highest-load device (the
  simulated AC unit) via a relay when total house power crosses a
  threshold, and restores it once load drops back down, with hysteresis
  and a cooldown to prevent rapid relay chattering
- **NILM-style appliance guessing** — detects sudden power jumps from a
  simulated aggregate sensor and guesses which device likely caused it,
  the same principle real non-intrusive load monitoring smart meters use
- **EEPROM-backed persistence** — cumulative energy is designed to
  survive a reset/power cycle (see [Known Limitations](#-known-limitations)
  for a Tinkercad-specific caveat)
- **On-board I2C LCD display** — live total kWh, house/room wattage, and
  forced full-screen alerts the instant an overload event happens, no PC
  required
- **9 status LEDs** driven by two chained 74HC595 shift registers, using
  only 3 Arduino pins

---

## 🛠 Components

| Component | Qty | Role |
|---|---|---|
| Arduino Uno | 1 | Main controller |
| 74HC595 shift register | 2 (chained) | Drives 9 status LEDs from 3 pins |
| LED | 9 | Breaker + device status indicators |
| Resistor 680Ω | 8 | Current-limiting for chip 1's LEDs |
| Resistor 220Ω | 1 | Current-limiting for chip 2's LED |
| SPDT toggle/slide switch | 9 | 2 breakers + 7 device switches |
| Momentary pushbutton | 1 | Reset (intentionally not latching) |
| Potentiometer (10kΩ) | 2 | Simulated per-room current sensor |
| Relay (SPDT, bare coil — e.g. LU-5-R) | 1 | Load-shedding actuator |
| NPN transistor (e.g. 2N2222) | 1 | Low-side relay coil driver |
| Diode (1N4148/1N4007) | 1 | Flyback protection across the relay coil |
| Resistor ~1kΩ | 1 | Transistor base resistor |
| Filament bulb | 1 | Simulated AC unit load (visual shed/restore indicator) |
| 16x2 I2C LCD (**PCF8574** backpack) | 1 | On-board status display |
| Breadboard(s) + jumper wires | — | Everything else |

> ⚠️ **LCD chip matters:** this project's `LiquidCrystal_I2C` library
> expects a **PCF8574** I2C backpack. A visually identical module using a
> different expander chip (MCP23008/MCP23017) will compile and wire fine
> but display nothing — see `docs/project-notes.md` for the full story.

---

## 🔌 Pin map

| Arduino Pin | Connects to |
|---|---|
| D2 | Breaker 1 |
| D3 | Breaker 2 |
| D4 | Room 1 – Fan |
| D5 | Room 1 – Lights |
| D6 | Room 1 – TV |
| D7 | Room 2 – AC |
| D8 | Room 2 – Lights |
| D9 | Room 2 – Exhaust |
| D10 | Room 2 – Microwave |
| D11 | Shift register DATA (SER) |
| D12 | Shift register LATCH (RCLK) |
| D13 | Shift register CLOCK (SRCLK) |
| A0 | Potentiometer 1 — Room 1 simulated current |
| A1 | Potentiometer 2 — Room 2 simulated current |
| A2 | Reset button |
| A3 | NPN transistor base (relay control) |
| A4 | LCD SDA (I2C data) |
| A5 | LCD SCL (I2C clock) |

---

## 🚀 Running it

**In Tinkercad (simulation):**
1. Open the project: see [`sim/tinkercad-link.md`](sim/tinkercad-link.md)
2. Press Start Simulation
3. Toggle switches to bring devices online and watch the LEDs, Serial
   Monitor, and LCD respond
4. Turn a potentiometer to see the NILM appliance-guessing feature in
   Serial output

**On real hardware / Arduino IDE:**
1. Wire the circuit per the [pin map](#-pin-map) above
2. Open `src/SPUMCS.ino` in the Arduino IDE
3. Install the **LiquidCrystal_I2C** library (Library Manager)
4. Select **Arduino Uno** as the board, upload, and open the Serial
   Monitor at 115200 baud

---

## 📁 Repository structure

```
SPUMCS/
├── README.md
├── LICENSE
├── src/
│   └── SPUMCS.ino
├── docs/
│   ├── project-notes.md      ← detailed build/debugging log
│   └── circuit-diagram.png
├── media/
│   └── screenshot.png
└── sim/
    └── tinkercad-link.md
```

For the full story of *why* the circuit and code look the way they do —
including every bug found and fixed along the way — see
[`docs/project-notes.md`](docs/project-notes.md).

---

## ⚠️ Known limitations

- **EEPROM persistence does not survive a Tinkercad simulation restart.**
  The read/write logic is correct for real ATmega328P EEPROM behavior
  (verified via the reset button) and would persist correctly on
  physical hardware — this is a simulator-only constraint.
- **The potentiometers simulate current sensing**, standing in for
  hardware like an ACS712 that Tinkercad doesn't model meaningfully.
- **Tinkercad's simulated clock runs slower than real time**, so
  millisecond-based timing will feel longer while watching the
  simulation than the code's constants suggest.

---

## 📄 License

This project is licensed under the MIT License — see [`LICENSE`](LICENSE)
for details.