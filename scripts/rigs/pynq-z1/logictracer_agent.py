#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Mykola Fedchyk
"""LogicTracer board agent for the PYNQ-Z1 (and other Zynq boards running Linux).

Serves the in-design capture core ``lt_capture`` (hdl/lt_capture, register map in hdl/README.md) to the
LogicTracer host (``PynqConnection``, port ``pynq://host[:port]``) as newline-delimited JSON over TCP.

Register access:
  --uio /dev/uio0                 the core's UIO device (no root needed with a udev rule)
  --devmem 0x43C00000             physical base address through /dev/mem (root)
  --sim                           a register-level model of lt_capture driven by a synthetic burst,
                                  for host development and tests without a board

The agent binds to 127.0.0.1 by default: reach it through an SSH tunnel
(``ssh -N -L 7878:localhost:7878 xilinx@pynq``) or pass --bind 0.0.0.0 on a trusted lab network.

Only the Python standard library is used, so it runs on the stock PYNQ image.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import mmap
import os
import select
import socket
import socketserver
import struct
import sys
import threading
import time

PROTOCOL = 1

# Register map (hdl/README.md)
ID, VERSION, CTRL, STATUS = 0x000, 0x004, 0x008, 0x00C
TRIG_MASK, TRIG_VALUE, TRIG_RISE, TRIG_FALL = 0x010, 0x014, 0x018, 0x01C
POST_LO, POST_HI, WRITE_PTR, TRIGGER_PTR = 0x020, 0x024, 0x028, 0x02C
TRIG_TS_LO, TRIG_TS_HI, END_TS_LO, END_TS_HI = 0x030, 0x034, 0x038, 0x03C
CLOCK_HZ, CONFIG, READ_INDEX, READ_TS_LO, READ_TS_HI = 0x040, 0x044, 0x048, 0x04C, 0x050
READ_PROBE, RECORDS, OLDEST = 0x054, 0x058, 0x05C
CORE_ID = 0x4C545243  # "LTRC"

ST_ARMED, ST_TRIGGERED, ST_DONE, ST_OVERFLOW, ST_WRAPPED, ST_ABORTED = 1, 2, 4, 8, 16, 32
CTRL_ARM, CTRL_ABORT, CTRL_FORCE, CTRL_EXT = 1, 2, 4, 8


# --------------------------------------------------------------------------------------------------
# Register backends


class MmapRegisters:
    """32-bit registers of one core through /dev/mem or a UIO device."""

    def __init__(self, path: str, offset: int, size: int = 0x1000):
        self._fd = os.open(path, os.O_RDWR | os.O_SYNC)
        self._map = mmap.mmap(self._fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE,
                              offset=offset)

    def read(self, offset: int) -> int:
        return struct.unpack_from("<I", self._map, offset)[0]

    def write(self, offset: int, value: int) -> None:
        struct.pack_into("<I", self._map, offset, value & 0xFFFFFFFF)


class SimCore:
    """Register-level model of lt_capture: the equations of hdl/lt_capture/rtl/lt_capture.v.

    The probe vector is a step function of the cycle count since arm: ``stimulus(cycle)``, changing only
    at the cycles listed in ``changes``. Time advances by events (arm, probe changes, end of the post-
    trigger window); nothing can happen in between. A capture whose trigger never comes stays armed (the
    agent then times out and aborts). ``force`` fires on the cycle after the last stimulus change.
    """

    def __init__(self, stimulus, changes, probe_width=32, addr_width=13, ts_width=48, sync_stages=0,
                 clock_hz=100_000_000):
        self.stimulus, self.changes = stimulus, sorted(set(changes))
        self.pw, self.aw, self.tsw, self.sync = probe_width, addr_width, ts_width, sync_stages
        self.clock_hz = clock_hz
        self.depth = 1 << addr_width
        self.mem = [(0, 0)] * self.depth
        self.mask = self.value = self.rise = self.fall = 0
        self.post = 0
        self.ext_mode = False
        self.read_index = 0
        self.armed = self.triggered = self.done = self.overflow = self.wrapped = self.aborted = False
        self.wr_ptr = self.trig_ptr = 0
        self.ts = self.trig_ts = self.end_ts = 0
        self.post_count = 0

    # -- capture engine -------------------------------------------------------------------------
    def _arm(self):
        self.armed, self.triggered, self.done = True, False, False
        self.overflow = self.wrapped = self.aborted = False
        self.wr_ptr, self.post_count, self.ts = 0, 0, 0
        self._run(sorted({0, *[c for c in self.changes if c > 0]}), first_cycle=0)

    def _run(self, cycles, first_cycle=None, force_at=None):
        pmask = (1 << self.pw) - 1
        i = 0
        while self.armed and not self.done:
            # `elapsed` uses the registered `triggered`: the window ends max(POST_CYCLES, 1) after the trigger.
            stop_at = self.trig_ts + max(self.post, 1) if self.triggered else None
            nxt = cycles[i] if i < len(cycles) else None
            candidates = [c for c in (nxt, stop_at, force_at) if c is not None]
            if not candidates:
                return  # waiting for a trigger that does not come
            c = min(candidates)
            if nxt == c:
                i += 1
            first = c == first_cycle
            p = self.stimulus(c) & pmask
            prev = self.stimulus(c - 1) & pmask
            rise, fall = p & ~prev, ~p & prev
            edge_cfg = (self.rise | self.fall) != 0
            edge_hit = ((rise & self.rise) | (fall & self.fall)) != 0 and not first
            level_hit = ((p ^ self.value) & self.mask) == 0
            pattern = level_hit and (edge_hit if edge_cfg else True)
            fire = not self.triggered and (c == force_at or (not self.ext_mode and pattern))
            elapsed = self.triggered and (c - self.trig_ts) >= self.post
            changed = first or p != prev
            do_write = changed or fire or elapsed
            post_full = self.triggered and do_write and self.post_count == self.depth - 1
            if fire:
                self.trig_ptr, self.trig_ts = self.wr_ptr, c & ((1 << self.tsw) - 1)
            if do_write:
                self.mem[self.wr_ptr] = (c & ((1 << self.tsw) - 1), p)
                if self.wr_ptr == self.depth - 1:
                    self.wrapped = True
                self.wr_ptr = (self.wr_ptr + 1) % self.depth
                if self.triggered:
                    self.post_count += 1
            if fire:
                self.triggered, self.post_count = True, 1
            if elapsed or post_full:
                self.done, self.armed = True, False
                self.overflow = post_full and not elapsed
                self.end_ts = c & ((1 << self.tsw) - 1)
            self.ts = c
            if force_at is not None and c >= force_at:
                force_at = None

    def _force(self):
        if self.armed and not self.done and not self.triggered:
            self._run([], force_at=max([self.ts, *self.changes]) + 1)

    def _resume(self):
        """Re-evaluates from the next cycle after a register change while armed (as the RTL does every clock)."""
        if self.armed and not self.done:
            self._run(sorted({self.ts + 1, *[c for c in self.changes if c > self.ts]}))

    def _abort(self):
        if self.armed and not self.done:
            self.armed, self.aborted, self.end_ts = False, True, self.ts

    # -- registers ------------------------------------------------------------------------------
    def read(self, off: int) -> int:
        if off == READ_PROBE:
            value = self.mem[self.read_index][1]
            self.read_index = (self.read_index + 1) % self.depth
            return value
        ts, _ = self.mem[self.read_index]
        return {
            ID: CORE_ID, VERSION: 0x00010000,
            CTRL: CTRL_EXT if self.ext_mode else 0,
            STATUS: (self.armed * ST_ARMED | self.triggered * ST_TRIGGERED | self.done * ST_DONE
                     | self.overflow * ST_OVERFLOW | self.wrapped * ST_WRAPPED | self.aborted * ST_ABORTED),
            TRIG_MASK: self.mask, TRIG_VALUE: self.value, TRIG_RISE: self.rise, TRIG_FALL: self.fall,
            POST_LO: self.post & 0xFFFFFFFF, POST_HI: self.post >> 32,
            WRITE_PTR: (self.wrapped << self.aw) | self.wr_ptr, TRIGGER_PTR: self.trig_ptr,
            TRIG_TS_LO: self.trig_ts & 0xFFFFFFFF, TRIG_TS_HI: self.trig_ts >> 32,
            END_TS_LO: self.end_ts & 0xFFFFFFFF, END_TS_HI: self.end_ts >> 32,
            CLOCK_HZ: self.clock_hz,
            CONFIG: (self.tsw << 24) | (self.sync << 16) | (self.aw << 8) | self.pw,
            READ_INDEX: self.read_index, READ_TS_LO: ts & 0xFFFFFFFF, READ_TS_HI: ts >> 32,
            RECORDS: self.depth if self.wrapped else self.wr_ptr,
            OLDEST: self.wr_ptr if self.wrapped else 0,
        }.get(off, 0)

    def write(self, off: int, value: int) -> None:
        value &= 0xFFFFFFFF
        tsmask = (1 << self.tsw) - 1
        if off == CTRL:  # precedence per clock: arm > abort > force
            self.ext_mode = bool(value & CTRL_EXT)
            if value & CTRL_ARM:
                self._arm()
            elif value & CTRL_ABORT:
                self._abort()
            elif value & CTRL_FORCE:
                self._force()
            else:
                self._resume()  # e.g. leaving external-trigger mode lets the pattern fire
        elif off in (TRIG_MASK, TRIG_VALUE, TRIG_RISE, TRIG_FALL):  # registers are PROBE_WIDTH bits wide
            name = {TRIG_MASK: "mask", TRIG_VALUE: "value", TRIG_RISE: "rise", TRIG_FALL: "fall"}[off]
            setattr(self, name, value & ((1 << self.pw) - 1))
            self._resume()
        elif off == POST_LO:
            self.post = ((self.post & ~0xFFFFFFFF) | value) & tsmask
            self._resume()
        elif off == POST_HI:
            self.post = ((self.post & 0xFFFFFFFF) | (value << 32)) & tsmask
            self._resume()
        elif off == READ_INDEX:
            self.read_index = value % self.depth


def burst_stimulus(start=1000, pulses=100, period=500, high=200, delay=5):
    """STIM (bit 0) pulses and REACT (bit 1) following each one `delay` cycles later."""
    changes = []
    for k in range(pulses):
        t0 = start + k * period
        changes += [t0, t0 + delay, t0 + high, t0 + high + delay]

    def value(c):
        if c < start:
            return 0
        k, phase = divmod(c - start, period)
        if k >= pulses:
            return 0
        return (1 if phase < high else 0) | (2 if delay <= phase < high + delay else 0)

    return value, changes


# --------------------------------------------------------------------------------------------------
# Core access shared by hardware and simulation


class Core:
    def __init__(self, regs, probe_names=None, info=None):
        self.regs = regs
        if regs.read(ID) != CORE_ID:
            raise RuntimeError(f"no lt_capture core at this address (ID 0x{regs.read(ID):08X})")
        cfg = regs.read(CONFIG)
        self.probe_width, self.addr_width = cfg & 0xFF, (cfg >> 8) & 0xFF
        self.sync_stages, self.ts_width = (cfg >> 16) & 0xFF, (cfg >> 24) & 0xFF
        self.clock_hz = regs.read(CLOCK_HZ)
        version = regs.read(VERSION)
        self.version = f"{version >> 16}.{version & 0xFFFF}"
        self.probe_names = list(probe_names or [])[: self.probe_width]
        self.info = info or {}
        self.lock = threading.Lock()
        self.cancel = threading.Event()

    def identify(self):
        return {"ok": True, "protocol": PROTOCOL, "core": "lt_capture", "version": self.version,
                "clock_hz": self.clock_hz, "probe_width": self.probe_width,
                "records": 1 << self.addr_width, "ts_width": self.ts_width, "sync_stages": self.sync_stages,
                "probes": self.probe_names, **self.info}

    def _read64(self, lo, hi):
        return self.regs.read(lo) | (self.regs.read(hi) << 32)

    def capture(self, mask=0, value=0, rise=0, fall=0, post_cycles=1000, external=False, timeout_ms=5000,
                on_armed=None, client_gone=None):
        r = self.regs
        self.cancel.clear()
        ext = CTRL_EXT if external else 0
        for reg, v in ((TRIG_MASK, mask), (TRIG_VALUE, value), (TRIG_RISE, rise), (TRIG_FALL, fall),
                       (POST_LO, post_cycles & 0xFFFFFFFF), (POST_HI, post_cycles >> 32)):
            r.write(reg, v)
        r.write(CTRL, CTRL_ARM | ext)
        if on_armed:
            on_armed()
        deadline = time.monotonic() + timeout_ms / 1000.0
        while not r.read(STATUS) & ST_DONE:
            gone = client_gone is not None and client_gone()
            if self.cancel.is_set() or gone or time.monotonic() > deadline:
                r.write(CTRL, CTRL_ABORT | ext)
                return {"ok": True, "status": "CANCELLED" if self.cancel.is_set() or gone else "TIMEOUT"}
            time.sleep(0.002)
        return self.read_records()

    def read_records(self):
        """Software sequence of hdl/README.md: oldest record first, times relative to the trigger."""
        r = self.regs
        status = r.read(STATUS)
        count, oldest, trig_ptr = r.read(RECORDS), r.read(OLDEST), r.read(TRIGGER_PTR)
        trig_ts = self._read64(TRIG_TS_LO, TRIG_TS_HI)
        end_ts = self._read64(END_TS_LO, END_TS_HI)
        depth, wrap = 1 << self.addr_width, 1 << self.ts_width
        trig_idx = (trig_ptr - oldest) % depth  # position of the trigger record in reading order

        r.write(READ_INDEX, oldest)
        raw = []
        for _ in range(count):
            ts = self._read64(READ_TS_LO, READ_TS_HI)
            raw.append((ts, r.read(READ_PROBE)))  # reading READ_PROBE advances READ_INDEX
        # The record position, not the timestamp, says which side of the trigger a record is on, so a long wait
        # cannot make history look like the future. Ages are taken modulo 2^TS_WIDTH; walking back from the
        # trigger they must grow, and the first one that does not lies more than a full counter period back.
        pre, last_age = [], 0
        for ts, probe in reversed(raw[:trig_idx]):
            age = (trig_ts - ts) % wrap
            if age < last_age:
                break
            pre.append([-age, probe])
            last_age = age
        records = list(reversed(pre)) + [[(ts - trig_ts) % wrap, probe] for ts, probe in raw[trig_idx:]]
        return {"ok": True, "status": "OK", "trigger_ts": trig_ts, "end_rel": (end_ts - trig_ts) % wrap,
                "overflow": bool(status & ST_OVERFLOW), "wrapped": bool(status & ST_WRAPPED),
                "dropped_history": trig_idx - len(pre), "records": records}


# --------------------------------------------------------------------------------------------------
# JSON server


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        core: Core = self.server.core
        for line in self.rfile:
            try:
                req = json.loads(line)
                cmd = req.get("cmd")
                if cmd == "identify":
                    reply = core.identify()
                elif cmd == "capture":
                    if not core.lock.acquire(blocking=False):
                        reply = {"ok": False, "error": "busy: another capture is running"}
                    else:
                        try:
                            args = {k: int(req[k]) for k in ("mask", "value", "rise", "fall", "post_cycles",
                                                             "timeout_ms") if k in req}
                            if any(v < 0 for v in args.values()):
                                raise ValueError("negative capture parameter")
                            reply = core.capture(external=bool(req.get("external", False)),
                                                 on_armed=lambda: self._send({"ok": True, "event": "armed"}),
                                                 client_gone=self._client_gone, **args)
                        finally:
                            core.lock.release()
                elif cmd == "cancel":
                    core.cancel.set()
                    reply = {"ok": True}
                else:
                    reply = {"ok": False, "error": f"unknown command {cmd!r}"}
            except Exception as exc:  # report and keep serving
                reply = {"ok": False, "error": str(exc)}
            if not self._send(reply):
                return

    def _send(self, msg):
        try:
            self.wfile.write((json.dumps(msg, separators=(",", ":")) + "\n").encode())
            self.wfile.flush()
            return True
        except OSError:  # the client went away
            return False

    def _client_gone(self):
        """True when the client closed its connection (a capture it started is then aborted)."""
        try:
            readable, _, _ = select.select([self.connection], [], [], 0)
            return bool(readable) and self.connection.recv(1, socket.MSG_PEEK) == b""
        except OSError:
            return True


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--uio", help="UIO device of the core, e.g. /dev/uio0")
    src.add_argument("--devmem", type=lambda s: int(s, 0), help="physical base address of the core")
    src.add_argument("--sim", action="store_true", help="simulated core with a STIM/REACT burst")
    ap.add_argument("--sim-delay", type=int, default=5, help="simulated REACT delay in cycles (default 5)")
    ap.add_argument("--probes", default="", help="comma-separated probe names, bit 0 first (e.g. STIM,REACT)")
    ap.add_argument("--bitstream", default="", help="bitstream file: its name and SHA-256 are reported")
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=7878, help="TCP port (0 = any free port)")
    args = ap.parse_args(argv)

    names = args.probes.split(",") if args.probes else []  # an empty entry keeps its bit unnamed
    if args.sim:
        stim, changes = burst_stimulus(delay=args.sim_delay)
        regs = SimCore(stim, changes, probe_width=2)
        names = names or ["STIM", "REACT"]
    elif args.uio:
        regs = MmapRegisters(args.uio, 0)
    else:
        regs = MmapRegisters("/dev/mem", args.devmem)
    info = {"backend": "sim" if args.sim else "hardware"}
    if args.bitstream:
        with open(args.bitstream, "rb") as f:
            info["bitstream"] = {"name": os.path.basename(args.bitstream),
                                 "sha256": hashlib.sha256(f.read()).hexdigest()}
    server = Server((args.bind, args.port), Handler)
    server.core = Core(regs, names, info)
    print(f"LISTENING {server.server_address[1]}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
