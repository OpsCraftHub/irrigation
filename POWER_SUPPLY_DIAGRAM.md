# Power Supply & Valve Wiring

This document captures the recommended way to feed both the irrigation solenoid(s) and the ESP32 logic from a single 12 V DC plug-pack (same style used for IP cameras). It matches the guidance already sprinkled through `README.md` and `QUICKSTART.md`, but keeps the schematic and Communica-friendly parts list in one place.

## Block Diagram

```
         12 V / 3 A wall adapter (5.5 x 2.1 mm plug)
                            │
                        Panel jack
                            │
                         2 A fuse
                            │
      ┌─────────────────────┴─────────────────────┐
      │                                           │
Valve branch (12 V)                       Logic branch (buck to 5 V)
      │                                           │
  Relay NO ───> Valve +                     LM2596 buck module
  Relay COM ───> Valve –───┬───────┐              │
         │                 │       │              │
         │           Flyback diode │              │
         │            (1N5408)     │              │
         │                 │       │              │
         └────────────┬────┴───────┘              │
                      │                          5 V rail ──> ESP32 5V / Vin
                      └─────────────────────── Shared GND
```

**Implementation notes**

- Keep the high-current valve loop (relay contacts, valve wiring, flyback diode) physically separate from the ESP32 traces, then tie ground back at a single “star” point by the buck converter.
- Adjust the LM2596 (or similar) module to exactly 5.0 V before connecting it to the ESP32. Most dev boards regulate further down to 3.3 V internally.
- If you drive the valve with a MOSFET instead of a relay module, place the MOSFET, gate resistor, and flyback diode on the valve branch before it re-enters the shared ground.

## Communica Parts List

All part numbers below reference typical Communica catalogue entries; equivalents from other suppliers work too.

| Qty | Item | Notes |
|-----|------|-------|
| 1 | 12 V 3 A DC adapter (5.5 mm × 2.1 mm plug) | CCTV-style wall plug with enough headroom for solenoid inrush and future channels |
| 1 | Panel-mount DC jack (5.5 mm × 2.1 mm) | e.g. DCJ-005; brings the adapter into the enclosure |
| 1 | Inline mini-blade fuse holder + 1.5–2 A fuse | Protects the valve wiring and keeps shorts from cooking the adapter |
| 1 | LM2596/MP1584 buck converter module (≥3 A) | Drops 12 V to a clean 5 V rail for the ESP32, LCD, buttons, etc. |
| 1 | 2-pin 5.08 mm screw terminal | For the 12 V input |
| 2 | 3-pin 5.08 mm screw terminals | One for valve out (NO/COM/GND), one for the 5 V logic rail if desired |
| 1 | Relay module (5 V coil, opto-isolated) **or** logic-level MOSFET (IRLZ44N) + flyback diode (1N5408) + 10 k pull-down + 220 Ω gate resistor | Matches the firmware default on GPIO25 |
| 1 | Flyback diode (1N5408 or UF4007) | Mount directly across the solenoid terminals (stripe to +12 V) |
| misc | Heat-shrink, ferrules, DIN rail clips | For neat wiring and strain relief |

## Build Steps

1. Mount the panel jack, fuse holder, buck module, relay/MOSFET driver, and screw terminals on perfboard or a small PCB.
2. Pre-set the buck converter to 5.0 V using a bench supply before hooking into the ESP32.
3. Wire according to the diagram above, double-checking polarity on the solenoid and ESP32 rails.
4. Test with a multimeter and a dummy load (e.g., 12 V automotive bulb on the valve branch) before connecting the actual solenoid.
5. Once verified, add the flyback diode directly at the solenoid, close up the enclosure, and route the wiring as described in `README.md` (GPIO25 → driver → valve).

That’s all you need to plug a standard 12 V adapter in and reliably power both the irrigation hardware and the ESP32 controller.
