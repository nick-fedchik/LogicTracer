# Contracts of `logictracer-cli`

What a human or an AI agent may rely on when it runs `logictracer-cli` with `--json`. Every statement below is
checked by `tests/test_contracts.py` (CTest `logictracer-contract-tests`, on Linux, macOS and Windows).

## Envelope

Every JSON document printed by a command has:

| Field | Meaning |
|---|---|
| `schema_version` | `1`. Raised when a field is removed or renamed or changes its type. |
| `command` | the sub-command (`detect`, `probe`, `capture`, `analyze`, `diff`, `bootloader`, ...) |
| `status` | `OK`, `TIMEOUT`, `ERROR`, `CANCELLED`; `detect` also `NOT_FOUND` |
| `warnings` | array of strings; caveats the result carries (an empty array when there are none) |
| `error` | present when the command did not succeed |

Exactly one JSON document is printed on stdout, also on errors; diagnostics go to stderr.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | OK |
| 1 | error: device or file problem; `detect` found no device |
| 2 | the trigger did not occur within `--timeout` |
| 3 | `--expect` or `--invariant` failed, or `diff --strict` found a regression |
| 64 | usage error: invalid arguments or a request the device cannot do |

## Documents and schemas

The schemas ([JSON Schema 2020-12](https://json-schema.org/)) are in [`docs/schema`](schema):

| Document | Schema |
|---|---|
| `capture --json` (and the session/MCP capture result) | [`capture.schema.json`](schema/capture.schema.json) |
| `analyze --json` | [`analyze.schema.json`](schema/analyze.schema.json) |
| `diff --json` | [`diff.schema.json`](schema/diff.schema.json) |
| `probe --json` | [`probe.schema.json`](schema/probe.schema.json) |
| `detect --json` | [`detect.schema.json`](schema/detect.schema.json) |
| `bootloader --json` | [`bootloader.schema.json`](schema/bootloader.schema.json) |
| an error before the command could run (usage, missing file, ...) | [`error.schema.json`](schema/error.schema.json) |
| the lossless capture file (`--out-json`) | [`capturefile.schema.json`](schema/capturefile.schema.json) |
| shared parts: latency, invariants, assertions, segments, device, per-channel summary, setup | [`common.schema.json`](schema/common.schema.json) |

The schemas list **every** field (`additionalProperties: false`): a field that is not in the schema is a bug,
and adding one changes the schema in the same commit. Fields that only appear in some cases (an `error`,
`latency` statistics when there are pairs, the per-window latency of a segmented capture) are optional;
values that can be unknown are typed `null` as well.

## Determinism

The same command with the same input (a file, or a replayed recording) prints the same JSON and the same
AI report every time, except the fields marked `"x-volatile": true` in the schemas:

| Field | Why it changes |
|---|---|
| `elapsed_ms` | wall-clock duration of the operation |
| `captured_at_utc` | wall-clock time of the capture |

Output paths in `outputs` are the paths that were requested.

## Not yet covered

- The responses of the GUI session commands `annotate` and `view` (they need a running GUI).
- MCP tool results are the capture/probe documents above wrapped in MCP `structuredContent`; the MCP
  envelope itself is checked by `tests/test_mcp.cpp`, not by schema.
