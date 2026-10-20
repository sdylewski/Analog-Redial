import os
data = open(os.path.join(os.path.dirname(__file__),
    "USB Serial Device (COM9)$20260925-085143.dmslog8"), 'rb').read()

for off, label in [
    (0x14331, 'read#1 request  (before any write)'),
    (0x14375, 'read#1 completion'),
    (0x148F1, 'read#2 request  (after write=09)'),
    (0x14935, 'read#2 completion'),
    (0x14BD3, 'write(0A) request'),
    (0x14EB1, 'read#3 request  (after write=0A, same session)'),
    (0x14EF5, 'read#3 completion'),
    (0x15193, 'read#4 request  (after reconnect)'),
    (0x151D7, 'read#4 completion'),
]:
    print(f"{label}  @ 0x{off:05X}")
    print(' '.join(f'{b:02X}' for b in data[off:off+140]))
    print()
