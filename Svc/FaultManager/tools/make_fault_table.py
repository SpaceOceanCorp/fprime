#!/usr/bin/env python3
"""Build a Svc::FaultManager fault table file from JSON.

Input JSON: a list of entries, for example

    [
      {"fault_id": 100, "state": "ACTIVE", "persistence": 3, "latch": true,
       "clear_on_success": true, "priority": 1, "event_limit": 0, "sequence": "seq_low.bin"}
    ]

Usage: make_fault_table.py table.json fault_table.bin

The format is described in Svc/FaultManager/docs/sdd.md (section 4). All integers are
big-endian, and the trailer is CRC-32 (IEEE 802.3, as computed by Utils::Hash and zlib.crc32).
"""

import json
import struct
import sys
import zlib

MAGIC = 0x464D5431
STATES = {"ACTIVE": 0, "PASSIVE": 1, "DISABLED": 2}
FLAG_LATCH = 0x01
FLAG_CLEAR_ON_SUCCESS = 0x02
MAX_NAME = 239  # FileNameStringSize (240 in default/config/AcConstants.fpp) - 1


def build(entries):
    if len({e["fault_id"] for e in entries}) != len(entries):
        raise ValueError("duplicate fault_id")
    out = bytearray(struct.pack(">IH", MAGIC, len(entries)))
    for e in entries:
        name = e["sequence"].encode("ascii")
        if not 1 <= len(name) <= MAX_NAME or b"\0" in name:
            raise ValueError(f"bad sequence name for fault {e['fault_id']}")
        persistence = int(e["persistence"])
        if not 1 <= persistence <= 255:
            raise ValueError(f"persistence must be 1..255 for fault {e['fault_id']}")
        flags = (FLAG_LATCH if e.get("latch") else 0) | (FLAG_CLEAR_ON_SUCCESS if e.get("clear_on_success") else 0)
        out += struct.pack(">IBBBBBB", int(e["fault_id"]), STATES[e["state"]], persistence, flags,
                           int(e.get("priority", 0)), int(e.get("event_limit", 0)), len(name))
        out += name
    out += struct.pack(">I", zlib.crc32(bytes(out)) & 0xFFFFFFFF)
    return bytes(out)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], encoding="utf-8") as f:
        entries = json.load(f)
    data = build(entries)
    with open(sys.argv[2], "wb") as f:
        f.write(data)
    print(f"wrote {sys.argv[2]}: {len(entries)} entries, {len(data)} bytes")


if __name__ == "__main__":
    main()
