#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Mykola Fedchyk
"""Tests of the PYNQ board agent: the lt_capture register model (scenarios of the RTL testbench) and the
readout sequence the agent also uses on hardware. Usage: test_pynq_agent.py <path to logictracer_agent.py>."""

import importlib.util
import json
import socket
import sys
import threading
import time
import unittest

spec = importlib.util.spec_from_file_location("agent", sys.argv.pop(1) if len(sys.argv) > 1 else "logictracer_agent.py")
agent = importlib.util.module_from_spec(spec)
spec.loader.exec_module(agent)


def core(pulses=3, delay=5, **kw):
    stim, changes = agent.burst_stimulus(pulses=pulses, delay=delay)
    return agent.Core(agent.SimCore(stim, changes, probe_width=2, **kw))


class ModelTests(unittest.TestCase):
    def test_immediate_trigger_records_arm_and_end(self):
        r = core().capture(post_cycles=50)
        self.assertEqual(r["status"], "OK")
        self.assertEqual(r["records"], [[0, 0], [50, 0]])

    def test_rising_edge_trigger_and_exact_delay(self):
        r = core().capture(rise=1, post_cycles=600)
        self.assertEqual(r["records"], [[-1000, 0], [0, 1], [5, 3], [200, 2], [205, 0], [500, 1], [505, 3], [600, 3]])
        self.assertFalse(r["overflow"])

    def test_falling_edge_and_pattern(self):
        r = core().capture(fall=2, post_cycles=10)  # first REACT fall at 205
        self.assertEqual(r["records"][-2:], [[0, 0], [10, 0]])
        r = core().capture(mask=3, value=3, post_cycles=1)  # both HIGH from cycle 1005
        self.assertEqual(r["records"][-2:], [[0, 3], [1, 3]])
        self.assertEqual(r["trigger_ts"], 1005)

    def test_post_zero_stops_one_cycle_after_the_trigger(self):
        r = core().capture(rise=1, post_cycles=0)
        self.assertEqual(r["records"][-1], [1, 1])

    def test_ring_keeps_latest_history_and_overflow_stops_early(self):
        small = core(addr_width=2)  # 4 records
        r = small.capture(rise=1, post_cycles=10)
        self.assertTrue(r["wrapped"])
        self.assertEqual(r["records"], [[-1000, 0], [0, 1], [5, 3], [10, 3]])
        r = small.capture(fall=2, post_cycles=2000)  # trigger + 3 later records fill the ring
        self.assertTrue(r["overflow"])
        self.assertEqual([t for t, _ in r["records"]], [0, 295, 300, 495])

    def test_external_mode_never_fires_and_times_out(self):
        c = core()
        self.assertEqual(c.capture(external=True, post_cycles=10, timeout_ms=30)["status"], "TIMEOUT")
        self.assertTrue(c.regs.read(agent.STATUS) & agent.ST_ABORTED)

    def test_force_fires_after_the_stimulus(self):
        c = core(pulses=1)
        c.regs.write(agent.TRIG_RISE, 0)
        c.regs.write(agent.POST_LO, 5)
        c.regs.write(agent.CTRL, agent.CTRL_ARM | agent.CTRL_EXT)
        self.assertFalse(c.regs.read(agent.STATUS) & agent.ST_DONE)
        c.regs.write(agent.CTRL, agent.CTRL_FORCE | agent.CTRL_EXT)
        self.assertTrue(c.regs.read(agent.STATUS) & agent.ST_DONE)
        self.assertEqual(c.read_records()["records"][-1][0], 5)

    def test_timestamps_wrap_modulo_ts_width(self):
        c = core()
        c.regs.mem[0] = ((1 << 48) - 3, 1)  # a record just before the counter wrapped
        c.regs.mem[1] = (2, 0)              # the trigger record after the wrap
        c.regs.wr_ptr, c.regs.trig_ptr, c.regs.trig_ts = 2, 1, 2
        c.regs.wrapped = False
        self.assertEqual(c.read_records()["records"], [[-5, 1], [0, 0]])

    def test_long_wait_keeps_history_before_the_trigger(self):
        # TS_WIDTH 33 at 100 MHz wraps after 86 s; a trigger 50 s after arm is more than half of that.
        late = 5_000_000_000
        c = agent.Core(agent.SimCore(lambda t: 1 if t >= late else 0, [late], probe_width=1, ts_width=33))
        r = c.capture(rise=1, post_cycles=10)
        self.assertEqual(r["records"], [[-late, 0], [0, 1], [10, 1]])

    def test_registers_are_probe_width_wide(self):
        c = core()
        r = c.capture(mask=0xFFFFFFFF, value=0xFFFFFFFF, post_cycles=1)  # bits above the 2 probes are ignored
        self.assertEqual(r["trigger_ts"], 1005)
        self.assertEqual(c.regs.read(agent.TRIG_MASK), 3)
        self.assertEqual(core().capture(rise=4, post_cycles=1)["records"][0], [0, 0])  # no edge bit: immediate

    def test_leaving_external_mode_lets_the_pattern_fire(self):
        c = core()
        c.regs.write(agent.POST_LO, 3)
        c.regs.write(agent.CTRL, agent.CTRL_ARM | agent.CTRL_EXT)
        self.assertFalse(c.regs.read(agent.STATUS) & agent.ST_DONE)
        c.regs.write(agent.CTRL, 0)
        self.assertTrue(c.regs.read(agent.STATUS) & agent.ST_DONE)


class ServerTests(unittest.TestCase):
    def test_json_protocol(self):
        server = agent.Server(("127.0.0.1", 0), agent.Handler)
        server.core = core()
        threading.Thread(target=server.serve_forever, daemon=True).start()
        with socket.create_connection(server.server_address) as s:
            f = s.makefile("rw")

            def ask(msg):
                f.write(json.dumps(msg) + "\n")
                f.flush()
                return json.loads(f.readline())

            ident = ask({"cmd": "identify"})
            self.assertEqual((ident["core"], ident["probe_width"], ident["protocol"]), ("lt_capture", 2, 1))
            self.assertEqual(ask({"cmd": "capture", "rise": 1, "post_cycles": 10}), {"ok": True, "event": "armed"})
            self.assertEqual(json.loads(f.readline())["records"][1], [0, 1])
            self.assertFalse(ask({"cmd": "capture", "post_cycles": -1})["ok"] or False)
            self.assertFalse(ask({"cmd": "bogus"})["ok"])

        # A client that disconnects mid-capture releases the core (no "busy" for the next one).
        slow = agent.Server(("127.0.0.1", 0), agent.Handler)
        slow.core = core()
        threading.Thread(target=slow.serve_forever, daemon=True).start()
        with socket.create_connection(slow.server_address) as s1:
            s1.sendall(b'{"cmd":"capture","external":true,"timeout_ms":5000}\n')
            s1.makefile().readline()  # armed
        with socket.create_connection(slow.server_address) as s2:
            f2 = s2.makefile("rw")
            for _ in range(50):
                f2.write('{"cmd":"capture","post_cycles":1,"timeout_ms":100}\n')
                f2.flush()
                reply = json.loads(f2.readline())
                if reply.get("event") == "armed":
                    reply = json.loads(f2.readline())
                if reply["ok"]:
                    break
                time.sleep(0.05)
            self.assertEqual(reply["status"], "OK")
        slow.shutdown()
        slow.server_close()
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    unittest.main()
