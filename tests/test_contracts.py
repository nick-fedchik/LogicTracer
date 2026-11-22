#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Mykola Fedchyk
"""Contract tests of logictracer-cli: every --json document matches its JSON Schema (docs/schema), exit codes
follow the documented table, and identical runs give identical output (apart from fields marked x-volatile).

Usage: test_contracts.py <logictracer-cli> <docs/schema directory>
Uses replay devices built from generated recordings, so it needs no hardware and runs on every platform.
"""

import json
import os
import re
import subprocess
import sys
import tempfile
import unittest

CLI, SCHEMAS = (sys.argv.pop(1), sys.argv.pop(1)) if len(sys.argv) > 2 else ("logictracer-cli", "docs/schema")


# --------------------------------------------------------------------------------------------------
# Minimal JSON Schema validator (the subset the LogicTracer schemas use; standard library only)

class Validator:
    def __init__(self, directory):
        self.dir = directory
        self.files = {}

    def load(self, name):
        if name not in self.files:
            with open(os.path.join(self.dir, name), encoding="utf-8") as f:
                self.files[name] = json.load(f)
        return self.files[name]

    def resolve(self, ref, current):
        file, _, pointer = ref.partition("#")
        doc = self.load(file) if file else self.load(current)
        node = doc
        for part in [p for p in pointer.split("/") if p]:
            node = node[part]
        return node, (file or current)

    TYPES = {"object": dict, "array": list, "string": str, "boolean": bool, "null": type(None)}

    def type_ok(self, value, t):
        if t == "integer":
            return isinstance(value, int) and not isinstance(value, bool)
        if t == "number":
            return isinstance(value, (int, float)) and not isinstance(value, bool)
        return isinstance(value, self.TYPES[t])

    def errors(self, value, schema, file, path="$"):
        if "$ref" in schema:
            target, target_file = self.resolve(schema["$ref"], file)
            yield from self.errors(value, target, target_file, path)
            return
        if "const" in schema and value != schema["const"]:
            yield f"{path}: expected {schema['const']!r}, got {value!r}"
        if "enum" in schema and value not in schema["enum"]:
            yield f"{path}: {value!r} is not one of {schema['enum']}"
        if "type" in schema:
            types = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
            if not any(self.type_ok(value, t) for t in types):
                yield f"{path}: expected {types}, got {type(value).__name__}"
                return
        if "pattern" in schema and isinstance(value, str) and not re.search(schema["pattern"], value):
            yield f"{path}: {value!r} does not match {schema['pattern']!r}"
        if isinstance(value, dict):
            props = schema.get("properties", {})
            for key in schema.get("required", []):
                if key not in value:
                    yield f"{path}: missing required '{key}'"
            extra = schema.get("additionalProperties", True)
            for key, v in value.items():
                if key in props:
                    yield from self.errors(v, props[key], file, f"{path}.{key}")
                elif extra is False:
                    yield f"{path}: undocumented field '{key}'"
                elif isinstance(extra, dict):
                    yield from self.errors(v, extra, file, f"{path}.{key}")
        if isinstance(value, list) and isinstance(schema.get("items"), dict):
            for i, v in enumerate(value):
                yield from self.errors(v, schema["items"], file, f"{path}[{i}]")

    def check(self, value, schema_file):
        return list(self.errors(value, self.load(schema_file), schema_file))

    def volatile_keys(self):
        keys = set()

        def walk(node):
            if isinstance(node, dict):
                for k, v in node.get("properties", {}).items():
                    if isinstance(v, dict) and v.get("x-volatile"):
                        keys.add(k)
                for v in node.values():
                    walk(v)
            elif isinstance(node, list):
                for v in node:
                    walk(v)

        for name in os.listdir(self.dir):
            if name.endswith(".schema.json"):
                walk(self.load(name))
        return keys


# --------------------------------------------------------------------------------------------------

def recording(path, edges, samples, rate=1_000_000):
    channels = [{"channel": i + 1, "name": name, "idle_level": 0, "edge_samples": e}
                for i, (name, e) in enumerate(edges.items())]
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"schema_version": 1, "format": "logictracer-capture-v1", "sample_rate_hz": rate,
                   "samples": samples, "trigger_sample": 0, "channels": channels}, f)


def strip(value, keys):
    if isinstance(value, dict):
        return {k: strip(v, keys) for k, v in value.items() if k not in keys}
    if isinstance(value, list):
        return [strip(v, keys) for v in value]
    return value


class Contracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.v = Validator(SCHEMAS)
        cls.tmp = tempfile.TemporaryDirectory()
        t = cls.tmp.name
        pulses = [100 + 120 * k for k in range(30)]
        stim = [e for p in pulses for e in (p, p + 20)]
        react = [e for k, p in enumerate(pulses) for e in ((p + 40, p + 60) if k == 17 else (p + 2, p + 22))]
        cls.rec = os.path.join(t, "rec.json")
        recording(cls.rec, {"STIM": stim, "REACT": react, "IDLE": []}, 4000)
        cls.port = "replay:" + cls.rec
        cls.env = dict(os.environ, LOGICTRACER_SOCKET=os.path.join(t, "no-gui.sock"), LOGICTRACER_DENY_HARDWARE="1")

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_cli(self, *args):
        p = subprocess.run([CLI, *args], capture_output=True, text=True, encoding="utf-8", env=self.env, timeout=60)
        return p.returncode, p.stdout

    def json_cli(self, schema, expected_rc, *args):
        rc, out = self.run_cli(*args, "--json")
        self.assertEqual(rc, expected_rc, f"{args}: exit {rc}\n{out[:500]}")
        doc = json.loads(out)
        problems = self.v.check(doc, schema)
        self.assertEqual(problems, [], f"{args} vs {schema}:\n" + "\n".join(problems[:20]))
        return doc

    def out(self, name):
        return os.path.join(self.tmp.name, name)

    # -- documents ---------------------------------------------------------------------------------
    def test_invalid_channel_alias_is_a_usage_error(self):
        self.json_cli("error.schema.json", 64, "capture", "--port", self.port, "--no-session",
                      "--channels", "1:" + "x" * 17)

    def test_capture_documents(self):
        common = ["capture", "--port", self.port, "--no-session", "--freq", "1M"]
        self.json_cli("capture.schema.json", 0, *common, "--samples", "100", "--channels", "1,2")
        self.json_cli("capture.schema.json", 0, *common, "--samples", "100", "--channels", "1,2", "--summary", "--edges")
        doc = self.json_cli("capture.schema.json", 0, *common, "--samples", "1000", "--pre-samples", "50",
                            "--channels", "1:STIM,2:REACT", "--trigger", "1:rising", "--measure-latency", "1..2",
                            "--expect", "drops==0", "--invariant", "within(STIM, REACT, 50us)", "--summary",
                            "--meta", "dut.board=SIM", "--out-json", self.out("full.json"),
                            "--out-vcd", self.out("full.vcd"), "--out-csv", self.out("full.csv"),
                            "--out-events-csv", self.out("events.csv"))
        self.assertEqual(sorted(doc["outputs"]), ["csv", "events_csv", "json", "vcd"])
        self.json_cli("capture.schema.json", 3, *common, "--samples", "2000", "--channels", "1:STIM,2:REACT",
                      "--trigger", "1:rising", "--measure-latency", "1..2", "--expect", "drops==1",
                      "--invariant", "within(STIM, REACT, 1us); never(NOPE)")
        self.json_cli("capture.schema.json", 0, *common, "--samples", "60", "--pre-samples", "10", "--segments", "4",
                      "--channels", "1:STIM,2:REACT", "--trigger", "1:rising", "--measure-latency", "1..2",
                      "--invariant", "within(STIM, REACT, 5us)", "--summary", "--out-json", self.out("seg.json"))
        self.json_cli("capture.schema.json", 2, *common, "--samples", "50", "--channels", "1,3",
                      "--trigger", "3:rising", "--timeout", "50")
        self.json_cli("capture.schema.json", 64, *common, "--freq", "7M", "--samples", "50", "--channels", "1")
        for name in ("full.json", "seg.json"):
            with open(self.out(name), encoding="utf-8") as f:
                problems = self.v.check(json.load(f), "capturefile.schema.json")
            self.assertEqual(problems, [], name + ":\n" + "\n".join(problems[:20]))

    def test_analyze_diff_probe_detect_documents(self):
        self.test_capture_documents()  # produces full.json / seg.json
        self.json_cli("analyze.schema.json", 0, "analyze", self.out("full.json"), "--measure-latency", "1..2",
                      "--expect", "drops==0", "--invariant", "never(STIM & !REACT, settle=50us)", "--summary", "--edges")
        self.json_cli("analyze.schema.json", 0, "analyze", self.out("seg.json"), "--measure-latency", "1..2")
        self.json_cli("analyze.schema.json", 0, "analyze", self.out("full.vcd"))
        self.json_cli("analyze.schema.json", 0, "analyze", self.rec, "--measure-latency", "2..1")  # direction check
        self.json_cli("analyze.schema.json", 0, "analyze", self.rec, "--decode", "uart:tx=1,baud=500k",
                      "--expect", "uart:frames>=1")
        self.json_cli("capture.schema.json", 0, "capture", "--port", self.port, "--no-session", "--freq", "1M",
                      "--samples", "60", "--pre-samples", "5", "--channels", "1:STIM,2:REACT",
                      "--trigger", "pattern:1:10", "--decode", "uart:tx=1,baud=500k")
        self.json_cli("diff.schema.json", 0, "diff", self.out("full.json"), self.out("full.json"), "--measure-latency", "1..2")
        self.json_cli("diff.schema.json", 0, "diff", self.out("seg.json"), self.rec)
        self.json_cli("probe.schema.json", 0, "probe", "--port", self.port, "--no-session", "--channels", "1:STIM,2:REACT")
        self.json_cli("probe.schema.json", 64, "probe", "--port", self.port, "--no-session", "--channels", "9")
        gap = self.out("gap.json")  # CH1 and CH3 recorded: probing CH2 fails inside the backend (exit 1)
        with open(gap, "w", encoding="utf-8") as f:
            json.dump({"schema_version": 1, "format": "logictracer-capture-v1", "sample_rate_hz": 1_000_000,
                       "samples": 100, "trigger_sample": 0,
                       "channels": [{"channel": c, "idle_level": 0, "edge_samples": []} for c in (1, 3)]}, f)
        self.json_cli("probe.schema.json", 1, "probe", "--port", "replay:" + gap, "--no-session", "--channels", "2")
        rc, out = self.run_cli("detect", "--no-session", "--no-handshake", "--json")  # 0 with a device, else 1
        doc = json.loads(out)
        self.assertEqual(self.v.check(doc, "detect.schema.json"), [])
        self.assertEqual(rc, 0 if doc["status"] == "OK" else 1)

    def test_edge_cases(self):
        """Documents without the data that usually fills them: no edges, no latency pairs, missing channels."""
        common = ["capture", "--port", self.port, "--no-session", "--freq", "1M"]
        self.json_cli("capture.schema.json", 0, *common, "--samples", "100", "--channels", "3", "--summary", "--edges")
        self.json_cli("capture.schema.json", 3, *common, "--samples", "4", "--channels", "1,3", "--summary",
                      "--measure-latency", "3..1", "--expect", "drops==0,p99_ns<=5", "--invariant", "never(IDLE)")
        self.json_cli("capture.schema.json", 0, *common, "--samples", "30", "--pre-samples", "5", "--segments", "3",
                      "--channels", "1:STIM,3:IDLE", "--trigger", "1:rising", "--measure-latency", "1..3", "--summary",
                      "--out-json", self.out("segnolat.json"))
        self.json_cli("capture.schema.json", 0, "capture", "--port", self.port, "--no-session", "--freq", "500k",
                      "--samples", "100", "--channels", "1-3", "--trigger", "1:falling", "--summary",
                      "--measure-latency", "1:falling..2:falling")
        with open(self.out("segnolat.json"), encoding="utf-8") as f:
            self.assertEqual(self.v.check(json.load(f), "capturefile.schema.json"), [])
        # A requested latency pair that one side does not record is a usage error, not an "equivalent" verdict.
        self.json_cli("error.schema.json", 64, "diff", self.out("segnolat.json"), self.rec, "--measure-latency", "1..2")
        self.json_cli("diff.schema.json", 0, "diff", self.rec, self.rec, "--measure-latency", "1..2")
        empty = self.json_cli("diff.schema.json", 0, "diff", self.rec, self.rec, "--measure-latency", "1..3")
        self.assertEqual(empty["latency"]["count"], {"a": 0, "b": 0})
        self.assertEqual(empty["latency"]["p99_ns"], {"a": None, "b": None})
        self.assertEqual(empty["latency"]["histogram_a"], [])
        self.assertIsNone(empty["latency"]["same_fixed_delay_possible"])
        idle = self.out("idle.json")
        recording(idle, {"STIM": [], "REACT": []}, 100)
        mixed = self.json_cli("diff.schema.json", 0, "diff", idle, self.rec, "--measure-latency", "1..2")
        self.assertIsNone(mixed["latency"]["p99_ns"]["a"])
        self.assertGreater(mixed["latency"]["p99_ns"]["b"], 0)
        self.assertNotIn("delta", mixed["latency"]["p99_ns"])

    def test_error_documents_and_exit_codes(self):
        self.json_cli("error.schema.json", 64, "capture", "--samples", "lots")
        self.json_cli("error.schema.json", 64, "bogus")
        self.json_cli("error.schema.json", 1, "analyze", os.path.join(self.tmp.name, "missing.json"))
        self.json_cli("error.schema.json", 64, "diff", self.rec)
        self.json_cli("error.schema.json", 1, "bootloader", "--port", "/dev/ttyNONE", "--yes")

    # -- determinism -------------------------------------------------------------------------------
    def test_selftest_documents(self):
        # Without hardware: the generator is missing (error, exit 1) or the options are wrong (usage, exit 64).
        doc = self.json_cli("error.schema.json", 1, "selftest", "--port", self.port)
        self.assertIn("test-signal generator", doc["error"])
        self.json_cli("error.schema.json", 64, "selftest", "--a", "0")
        # Recorded on a Pico: every check passing, and a missing jumper (the rest skipped).
        data = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
        for name, status in (("selftest-pass.json", "OK"), ("selftest-wiring-fail.json", "FAILED")):
            with open(os.path.join(data, name), encoding="utf-8") as f:
                doc = json.load(f)
            self.assertEqual(self.v.check(doc, "selftest.schema.json"), [], name)
            self.assertEqual(doc["status"], status, name)
            counts = {s: sum(c["status"] == s for c in doc["checks"]) for s in ("PASS", "FAIL", "SKIP")}
            self.assertEqual(doc["summary"], {"passed": counts["PASS"], "failed": counts["FAIL"],
                                              "skipped": counts["SKIP"]}, name)

    def test_about_document(self):
        doc = self.json_cli("about.schema.json", 0, "about")
        self.assertIn("github.com", doc["repository"])

    def test_identical_runs_give_identical_output(self):
        volatile = self.v.volatile_keys()
        self.assertIn("elapsed_ms", volatile)
        cap = ["capture", "--port", self.port, "--no-session", "--freq", "1M", "--samples", "1000", "--channels",
               "1:STIM,2:REACT", "--trigger", "1:rising", "--measure-latency", "1..2", "--summary"]
        a = json.loads(self.run_cli(*cap, "--json", "--out-json", self.out("d1.json"))[1])
        b = json.loads(self.run_cli(*cap, "--json", "--out-json", self.out("d2.json"))[1])
        a["outputs"] = b["outputs"] = None
        self.assertEqual(strip(a, volatile), strip(b, volatile))
        for args in (["analyze", self.out("d1.json"), "--measure-latency", "1..2", "--ai"],
                     ["analyze", self.out("d1.json"), "--measure-latency", "1..2", "--json"],
                     cap + ["--ai"]):
            first, second = self.run_cli(*args)[1], self.run_cli(*args)[1]
            if "--json" in args:
                first, second = strip(json.loads(first), volatile), strip(json.loads(second), volatile)
            self.assertEqual(first, second, args)
        # The saved file and its analysis agree with each other across runs as well.
        x = strip(json.load(open(self.out("d1.json"), encoding="utf-8")), volatile | {"outputs"})
        y = strip(json.load(open(self.out("d2.json"), encoding="utf-8")), volatile | {"outputs"})
        self.assertEqual(x, y)


if __name__ == "__main__":
    unittest.main()
