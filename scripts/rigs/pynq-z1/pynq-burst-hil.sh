#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Mykola Fedchyk
#
# One-command HIL run against a Digilent PYNQ-Z1 device under test:
#   1. (optional) program a bitstream into the PL over SSH and record its SHA-256,
#   2. probe the RP2040 inputs (lines must be idle),
#   3. arm a triggered capture and ask the operator to start the stimulus (e.g. press BTN1),
#   4. measure stimulus -> reaction latency, check assertions, save JSON/VCD and an AI report.
# Every result carries the setup (board, bitstream, SHA-256, switches) as capture metadata.
#
# Example:
#   scripts/rigs/pynq-z1/pynq-burst-hil.sh --bitstream /home/xilinx/kpol_pynq_burst.bit \
#       --switches "SW0=1,SW1=1" --start "press BTN1"
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: pynq-burst-hil.sh [options]
  --host USER@HOST       PYNQ board for SSH (default: $PYNQ_HOST or xilinx@192.168.68.69)
  --bitstream PATH       .bit file on the board to program first (default: keep the loaded PL)
  --port PATH            RP2040 serial port (default: /dev/ttyACM0)
  --channels LIST        capture channels (default: 1:STIM,2:REACT)
  --freq HZ              sample rate (default: 100M)
  --samples N            total samples (default: 131k)
  --trigger SPEC         trigger (default: 1:rising)
  --latency SPEC         stimulus..reaction pair (default: 1..2)
  --expect EXPR          assertions (default: events>=100,drops==0,spurious==0,p99_ub_ns<=60)
  --switches TEXT        board switch positions to record, e.g. "SW0=1,SW1=1"
  --start TEXT           what the operator must do once armed (default: "press BTN1")
  --timeout MS           trigger timeout (default: 600000)
  --out DIR              output directory (default: build/hil/pynq-<UTC timestamp>)
  --note TEXT            question/context for the AI report
EOF
}

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
CLI="${LOGICTRACER_CLI:-${REPO_ROOT}/build/src/cli/logictracer-cli}"
HOST="${PYNQ_HOST:-xilinx@192.168.68.69}"
BITSTREAM=""
PORT="/dev/ttyACM0"
CHANNELS="1:STIM,2:REACT"
FREQ="100M"
SAMPLES="131k"
TRIGGER="1:rising"
LATENCY="1..2"
EXPECT="events>=100,drops==0,spurious==0,p99_ub_ns<=60"
SWITCHES=""
START="press BTN1"
TIMEOUT=600000
OUT=""
NOTE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --host) HOST="$2"; shift 2 ;;
        --bitstream) BITSTREAM="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --channels) CHANNELS="$2"; shift 2 ;;
        --freq) FREQ="$2"; shift 2 ;;
        --samples) SAMPLES="$2"; shift 2 ;;
        --trigger) TRIGGER="$2"; shift 2 ;;
        --latency) LATENCY="$2"; shift 2 ;;
        --expect) EXPECT="$2"; shift 2 ;;
        --switches) SWITCHES="$2"; shift 2 ;;
        --start) START="$2"; shift 2 ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --note) NOTE="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 64 ;;
    esac
done

[[ -x "$CLI" ]] || { echo "logictracer-cli not found at $CLI (build first or set LOGICTRACER_CLI)" >&2; exit 1; }
OUT="${OUT:-${REPO_ROOT}/build/hil/pynq-$(date -u +%Y%m%dT%H%M%SZ)}"
mkdir -p "$OUT"
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=5 "$HOST")

META=(--meta "dut.board=PYNQ-Z1" --meta "dut.host=${HOST#*@}" --meta "rig.start=${START}")
[[ -n "$SWITCHES" ]] && META+=(--meta "dut.switches=${SWITCHES}")

if [[ -n "$BITSTREAM" ]]; then
    echo "== Programming ${BITSTREAM} on ${HOST}"
    "${SSH[@]}" "sudo -n python3 - '$BITSTREAM'" < "$(dirname "${BASH_SOURCE[0]}")/pynq_program_pl.py" \
        | tee "$OUT/pl_program.json"
    SHA=$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d["sha256"]); sys.exit(d["fpga_state"]!="operating")' "$OUT/pl_program.json")
    META+=(--meta "dut.bitstream=$(basename "$BITSTREAM")" --meta "dut.bitstream_sha256=${SHA}")
else
    echo "== Keeping the bitstream already loaded on ${HOST} (not recorded: pass --bitstream to pin it)"
    META+=(--meta "dut.bitstream=unknown (not programmed by this run)")
fi

echo "== Probing idle lines"
"$CLI" probe --port "$PORT" --channels "$CHANNELS" | tee "$OUT/probe.txt"

READY="$OUT/armed.flag"
rm -f "$READY"
# Tell the operator as soon as the analyzer is armed (and its pre-trigger buffer is full).
(
    while [[ ! -e "$READY" ]]; do sleep 0.05; done
    printf '\n>>> ARMED on %s: now %s\n\n' "$PORT" "$START" >&2
    command -v notify-send >/dev/null && notify-send "LogicTracer" "Armed: ${START}" || true
) &
WATCHER=$!
trap 'kill $WATCHER 2>/dev/null || true' EXIT

echo "== Arming capture (timeout ${TIMEOUT} ms)"
set +e
"$CLI" capture --port "$PORT" --no-session --freq "$FREQ" --samples "$SAMPLES" --pre-samples 1000 \
    --channels "$CHANNELS" --trigger "$TRIGGER" --timeout "$TIMEOUT" \
    --measure-latency "$LATENCY" --expect "$EXPECT" "${META[@]}" --ready-file "$READY" \
    --out-json "$OUT/capture.json" --out-vcd "$OUT/capture.vcd" --out-events-csv "$OUT/events.csv" \
    --summary --ai --note "$NOTE" > "$OUT/report.md"
RC=$?
set -e

cat "$OUT/report.md"
echo "== Results in $OUT (exit code $RC: 0 pass, 2 trigger timeout, 3 assertion failed)"
exit $RC
