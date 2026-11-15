#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Mykola Fedchyk
"""Program a Xilinx .bit file into the Zynq-7000 PL through the Linux FPGA Manager.

Runs ON the PYNQ board as root (no PYNQ Python package needed). The .bit header is parsed
properly (fields a..e), the bitstream payload is byte-swapped per 32-bit word as the Zynq
FPGA Manager expects, written to /lib/firmware and loaded. Prints one JSON line with the
SHA-256 of the .bit file so the host can record exactly what was loaded.
"""

from __future__ import annotations

import hashlib
import json
import struct
import sys
import time
from pathlib import Path

FPGA_MGR = Path("/sys/class/fpga_manager/fpga0")


def parse_bit(data: bytes) -> tuple[dict, bytes]:
    """Return (header fields, raw bitstream) of a Xilinx .bit file."""
    pos = 0

    def take(n: int) -> bytes:
        nonlocal pos
        if pos + n > len(data):
            raise ValueError("truncated .bit header")
        chunk = data[pos:pos + n]
        pos += n
        return chunk

    first_len = struct.unpack(">H", take(2))[0]
    take(first_len)
    take(2)  # 0x0001
    fields = {}
    names = {b"a": "design", b"b": "part", b"c": "date", b"d": "time"}
    while True:
        key = take(1)
        if key == b"e":
            length = struct.unpack(">I", take(4))[0]
            return fields, take(length)
        if key not in names:
            raise ValueError(f"unexpected .bit header field {key!r}")
        length = struct.unpack(">H", take(2))[0]
        fields[names[key]] = take(length).rstrip(b"\0").decode("ascii", "replace")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: pynq_program_pl.py <bitstream.bit>", file=sys.stderr)
        return 64
    bit_path = Path(sys.argv[1])
    data = bit_path.read_bytes()
    sha = hashlib.sha256(data).hexdigest()
    fields, payload = parse_bit(data)
    if len(payload) % 4:
        raise ValueError("bitstream length is not a multiple of 4")

    swapped = bytearray(len(payload))
    for off in range(0, len(payload), 4):
        struct.pack_into(">I", swapped, off, struct.unpack_from("<I", payload, off)[0])

    fw_name = "logictracer_" + bit_path.stem + ".bin"
    Path("/lib/firmware", fw_name).write_bytes(swapped)
    (FPGA_MGR / "flags").write_text("0\n")
    (FPGA_MGR / "firmware").write_text(fw_name + "\n")
    time.sleep(0.5)
    state = (FPGA_MGR / "state").read_text().strip()

    print(json.dumps({
        "bitstream": bit_path.name,
        "sha256": sha,
        "design": fields.get("design", ""),
        "part": fields.get("part", ""),
        "built": (fields.get("date", "") + " " + fields.get("time", "")).strip(),
        "payload_bytes": len(payload),
        "fpga_state": state,
    }))
    return 0 if state == "operating" else 1


if __name__ == "__main__":
    sys.exit(main())
