"""Match unchanged instruction bytes between deployed and rebuilt PE images.

This does not load mismatched PDBs as evidence. Output candidates must be
checked for uniqueness and surrounding-function agreement before attribution.
"""
import struct
import sys
from pathlib import Path

def image(path):
    data = Path(path).read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional = struct.unpack_from("<H", data, pe + 20)[0]
    sections = []
    for i in range(count):
        p = pe + 24 + optional + 40 * i
        _, rva, size, raw = struct.unpack_from("<IIII", data, p + 8)
        sections.append((rva, size, raw))
    return data, sections

old, old_sections = image(sys.argv[1])
new, new_sections = image(sys.argv[2])
for argument in sys.argv[3:]:
    rva = int(argument, 0)
    old_offset = next(raw + rva - start for start, size, raw in old_sections if start <= rva < start + size)
    for width in (64, 32, 16):
        pattern = old[old_offset:old_offset + width]
        matches = []
        offset = new.find(pattern)
        while offset >= 0 and len(matches) < 16:
            for start, size, raw in new_sections:
                if raw <= offset < raw + size:
                    matches.append(hex(start + offset - raw))
            offset = new.find(pattern, offset + 1)
        print(hex(rva), "bytes", width, "candidates", matches)
        if len(matches) == 1:
            break
