# 3D-printed case (placeholder)

STL/step files for a printable enclosure are not designed yet — this is a
placeholder for when they are. Contributions welcome.

## Constraints for a case design

- **Board size:** ~100 × 50 mm, 2-layer PCB (see [`../ROTARY_PHONE_SMD4.md`](../ROTARY_PHONE_SMD4.md)).
- **Connector cutouts needed:**
  - RJ12 phone jack(s) — 1 or 2, depending on build (dual-line vs. single-line + microSD)
  - USB-C (power/programming)
  - DC barrel jack (alternate power input)
  - microSD card slot access (single-line build only)
- **Mounting:** standoff/screw pattern TBD once a specific enclosure design is started — check hole positions against the actual KiCad board outline in [`../kicad/Analog Redial v4/`](../kicad/Analog%20Redial%20v4/) before finalizing.
- Consider ventilation/access for the BOOT (GPIO9) and reset controls if they need to be reachable without opening the case.

## Status

No files yet.
