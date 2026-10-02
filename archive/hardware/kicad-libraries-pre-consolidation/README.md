# Superseded: project-local KiCad library

Archived 2026-10-02. These were this repo's own copies of custom symbols and
footprints (`Custom_Symbol_Library.kicad_sym`, `WM8731_lib.kicad_sym`,
`Custom.pretty`), used by SMD2, SMD4, and the SMD5/"Analog Redial" board
files before this date.

They've been consolidated into one personal library used across all KiCad
projects, not just this one: `~/Documents/KiCad/Custom/` (`Custom.kicad_sym`
+ `Custom.pretty`), registered globally in KiCad's symbol/footprint library
tables under the nickname `Custom`. Every board project in this repo now
resolves that nickname from the global table instead of a local copy.

**What this means for someone cloning this repo without that global
library:** the schematic and PCB files still open, view, plot, and export
Gerbers/BOM/CPL fine — KiCad embeds a copy of every symbol and footprint
actually used directly in the `.kicad_sch`/`.kicad_pcb` files. What's lost
is "Update from library" and placing a *new* instance of one of these parts;
KiCad will also report the `Custom` library as missing if you try to edit.

These two files are kept here only as a historical record of exactly what
was in the project-local library at the time of consolidation — not meant
to be referenced by any live project.
