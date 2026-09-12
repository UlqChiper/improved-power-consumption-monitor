SPUMCS Project Notes — v1 to v2
This document records what changed between the original SPUMCS sketch and the upgraded version, why each change was made, and the debugging process that led to the final working circuit. It's meant to be read alongside the code and circuit diagram, as a record of the engineering decisions behind the finished project — not just the end result.
________________________________________
1. Overview: what v1 was, and why it needed upgrading
The original sketch measured and kept track of per-device power draw from fixed nominal wattages, using switch + breaker state to decide whether a device was "on," and reported interval and cumulative energy over Serial. It worked, but was static in its working and behaved as a monitoring device or count keeping, not a system: no persistence, no protection logic, no integrated display {Serial Monitor was only way to display/read data}, and no "smart" behaviour a real energy-monitoring hardware has.
v2 adds seven upgrades on top of that foundation:
1.	Struct/array device model (code quality — a prerequisite for everything else)
2.	Non-blocking reset debouncing
3.	Simulated EEPROM persistence {Due to the limitation of Simulation environment, the feature is not observable, it should, however, work in physical environment with real hardware.}
4.	Relay-based overload protection {automatic load shedding, in the simulation shedding AC solves the overload problem so dynamic nature of system is not easily noticeable}
5.	NILM-style appliance-guessing from aggregate power
6.	Potentiometers standing in for real current sensors
7.	An I2C LCD for on-board, PC-free readout {Do verify the CHIP type, had to learned it the hard way}
Below is what each of these required, followed by the hardware debugging story — which turned out to be the harder half of the project.
________________________________________
2. Struct/array refactor
Before: every device had its own copy-pasted set of variables (p_r1_fan, p_r1_lights, p_r1_tv, …), and every calculation — power, interval energy, cumulative energy — was written out seven separate times.
After: a single Device struct holds a device's name, switch pin, room, nominal wattage, LED bit position, shed-eligibility flag, and cumulative energy. All seven devices live in one array, and one loop handles power calculation, LED bits, and energy accumulation for all of them.
Why this mattered: beyond readability, this made implementing later feature easy. Overload shedding, EEPROM save/load, and the LED bit map all needed to iterate over "all devices" generically, that's not practical when each device is a separate hardcoded variable.
________________________________________
3. Non-blocking reset debounce
Before: the reset button handler called delay(400) directly inside loop().
Issue: this froze the entire sketch for 400ms every time reset was pressed — LED updates, serial output, and (once added) the LCD would all stall. In a simulation that's easy to miss, but it's bad practice regardless.
Fix: reset is now debounced using a millis() timestamp comparison (now - lastResetPress > DEBOUNCE_MS), so nothing else in loop() ever blocks waiting for it.
________________________________________
4. Simulated EEPROM persistence
Goal: cumulative energy should survive a reset/power-cycle instead of zeroing every time the board restarts.
Implementation: a "magic number" (0xBEEF) is written to EEPROM alongside the cumulative energy data. On boot, the code checks whether that magic number is already present — if yes, it trusts the stored data and loads it; if no, it assumes this is a fresh/uninitialized chip and starts from zero. This is the standard way to distinguish "real saved data" from EEPROM's default blank/garbage state.
Known limitation, found during testing: Tinkercad's simulated EEPROM does not appear to persist across a full simulation stop/restart — every fresh "Start Simulation" reads back "no valid data yet." This is almost certainly a simulator limitation rather than a code bug: real EEPROM retains charge with zero power thanks to how the physical memory cells are built; a browser-based simulator has no equivalent to actually hold that state between runs. The read/write logic was verified correct via the reset button. The code is written to the ATmega328P's real EEPROM behaviour and would persist correctly on physical hardware.
________________________________________
5. Relay-based overload protection
This was the most involved part of the whole project, hardware-wise, and went through several corrections.
5.1 The bare relay problem
The relay on hand (an LU-5-R, confirmed via datasheet to be a genuine SPDT / "1 Form C" relay) turned out to be a bare electromechanical relay, not a driver-equipped "relay module." This wasn't obvious at first — it has 6 unlabeled pins, which initially looked similar to the labeled VCC/GND/IN relay modules used in most beginner tutorials.
The distinction matters: a relay module has a small onboard driver circuit so it can be switched directly with 5V logic. A bare relay exposes its coil terminals directly, and a coil is not something a digital pin should drive on its own, for two reasons:
•	Current: a coil generally wants more current than an Arduino pin can safely source.
•	Back-EMF: when the coil de-energizes, its collapsing magnetic field generates a reverse voltage spike, which can damage whatever switched it if nothing absorbs that spike.
5.2 The fix: NPN transistor as a low-side switch, with a flyback diode
Rather than driving the coil directly, the coil is now switched by an NPN transistor acting as a low-side switch, with a flyback diode across the coil for protection:
•	Arduino pin (A3) → resistor → transistor base — the resistor limits the current the digital pin has to source into the base.
•	Transistor emitter → GND, transistor collector → relay coil (low side) — this is the correct low-side orientation: driving the base HIGH lets current flow coil → collector → emitter → GND, completing the circuit. (Collector/emitter reversal here is the most common mistake in this kind of circuit and was checked carefully.)
•	Coil high side → +5V.
•	Flyback diode across the coil, cathode (striped end) toward +5V, anode toward the coil's low side / transistor collector. Normally reverse-biased and doing nothing while the coil is energized — the instant the transistor switches off, the coil's collapsing field reverses the voltage across it, forward-biasing the diode for that moment and giving the current a safe path to dissipate instead of spiking back through the transistor.
5.3 Identifying the bare relay's actual pinout
With no printed labels, the relay's 6 pins (physically numbered 1, 5, 6, 7, 8, 12 — non-sequential, consistent with a footprint numbering scheme shared across a larger relay family) had to be identified against the relay's own datasheet wiring diagram rather than guessed. Confirmed mapping:
Pin	Function
5	Coil + (→ +5V)
8	Coil − (→ transistor collector)
6	NO (Normally Open)
1	COM
7	NC (Normally Open) — left unconnected
12	COM (duplicate of pin 1) — left unconnected
The 6th pin being a duplicate COM is a mechanical/manufacturing detail (splitting switched current across two solder joints on real hardware) with no distinct electrical function in this circuit.
5.4 What the relay actually switches
The relay's COM/NO contacts drive a filament bulb, standing in for the AC unit's real power line — chosen because it's a simple two-terminal resistive load (no polarity, no extra resistor needed in Tinkercad's model) that visibly lights when powered and goes dark when cut, giving a direct visual confirmation of shedding/restoring separate from the status LEDs.
5.5 Relay chattering, and why it happened
Once wired and coded, the very first version of the shedding logic oscillated rapidly: the AC alone draws 1400W, while the shed/restore thresholds were only 400W apart (2000W shed / 1600W restore). Since removing or adding the AC swings total power by more than that whole hysteresis gap, shedding immediately satisfied the restore condition, and restoring immediately re-triggered the shed condition — every loop cycle (~20ms).
This wasn't just a bad-looking bug: it was also very likely the cause of the shift-register chip reporting an overcurrent/overheating condition, since rapid relay/LED switching draws meaningfully more effective current than a stable state.
Fix: a minimum 5-second cooldown between shed/restore transitions was added, independent of the wattage thresholds. This is also standard real-world relay practice — rapid mechanical cycling causes physical contact wear, so real overload-protection systems enforce a similar minimum dwell time between switching decisions.
________________________________________
6. NILM-style appliance-guessing
Non-Intrusive Load Monitoring is the technique real smart meters use to guess which specific appliance turned on or off from a single aggregate power reading, by matching the size of a sudden power jump against known appliance wattages. Since the project already has a per-room "aggregate sensor" (the potentiometers, see below), this was implemented as a simple delta-matching check: any sudden jump above a noise threshold is compared against every device's nominal wattage in that room, within a tolerance percentage, and the best match is printed as a guess.
This is a simplified version of real NILM (which typically uses trained classifiers rather than fixed-tolerance matching), but it demonstrates the same underlying principle at a beginner-appropriate level.
________________________________________
7. Potentiometers as simulated current sensors
Two 10kΩ potentiometers (one per room) stand in for a real current sensor like an ACS712, which isn't meaningfully simulated in Tinkercad. Turning a pot varies its wiper voltage, read via analogRead() and scaled to a plausible wattage range — this lets the analog-sensing → power calculation → NILM-detection pipeline be exercised and demonstrated exactly as it would be with a real sensor, just with a hand on the dial instead of a real appliance.
________________________________________
8. The I2C LCD’s Wrong I/O expander chip.
The LCD initially showed nothing despite correct wiring, correct I2C address, and no compile errors. The root cause turned out to be the backpack chip itself: it was actually a MCP23008, not the PCF8574 the LiquidCrystal_I2C library is written for. Both are I2C I/O expanders and both can technically back an LCD backpack, but they don't share a register-level command set — pointed at the wrong chip, the library sends bytes the hardware doesn't interpret the same way, which produces a silently blank screen rather than an error. Switching the physical module to a PCF8574-based one resolved this immediately.
________________________________________
9. Switch type: pushbuttons → toggle/slide switches
Original design: 9 momentary pushbuttons (breaker + device switches), matching the original v1 circuit.
The problem, found during functional testing: Tinkercad's simulator only allows one button to be actively click-held with the mouse at a time. Releasing one switch to click another causes the first to spring back to its unpressed state immediately — meaning a breaker and a device switch can never both be "on" at once through manual testing, since testing them together requires holding two momentary buttons simultaneously with a single cursor. This made it impossible to verify any wattage calculation that depends on two switches being on together (which is every one of them), even though the wiring and code were correct.
Fix: the 9 breaker/device switches were changed from momentary pushbuttons to SPDT toggle/slide switches, which latch in position when clicked rather than springing back on release. This allows multiple switches to be set and left in the "on" state simultaneously, matching how real circuit breakers and light switches actually behave and made functional testing possible.
Wiring difference: the pushbuttons used 2 of their 4 terminals (one diagonal pair — legs on the same side of a 4-leg pushbutton are internally pre-bridged). The toggle switches are simpler: 3 terminals (SPDT), using just the middle COM terminal → Arduino pin and one outer terminal → GND, with the other outer terminal left unconnected.
________________________________________
10. Chip current overload
After fixing OE/SRCLR, Tinkercad flagged one shift register with: "Total current through power pins is 79.3 mA, while maximum is 50.0 mA." At 220Ω per LED (≈13.6mA each), 8 simultaneously-lit LEDs on one chip could draw over 100mA combined — well past the chip's 50mA total budget, regardless of each individual pin being within its own spec.
Fix: the 8 LED resistors on the chip carrying most of the load were increased from 220Ω to 680Ω, dropping each LED to roughly 4.4mA and keeping the worst case (all 8 lit) around 35mA — comfortably under the 50mA limit, with LEDs still clearly visible.
This was a separate root cause from the relay-chattering issue covered in §5.5 — both contributed to the chip's overheating warning at different points, and both needed independent fixes.
________________________________________
11. Known limitations, for the record
•	EEPROM does not persist across a Tinkercad simulation restart. The logic is correct per the ATmega328P's real EEPROM behavior and would work on physical hardware; this is specifically a simulator constraint.
•	Tinkercad's simulated clock runs slower than real time, so millisecond-based timing (LCD page duration, alert hold time) will feel longer while watching the simulation than the raw numbers suggest.
•	The potentiometers are a stand-in for real current sensing, not actual measurement — moving to physical hardware would replace these with something like an ACS712 current sensor for genuine RMS power measurement.

                                         ---------------------Thanks For Reading---------------------