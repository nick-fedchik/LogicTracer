#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Mykola Fedchyk
# Two-stage HIL check for Seeed reServer Industrial J501 DO1 -> RP2040 GP28 (Channel 24).
#
# Stage 1: Probes CH24 (GP28, Pin 34) to confirm the external 4.7-10 kOhm pull-up to 3V3_OUT (Pin 36)
#          holds the open-drain optocoupler output HIGH (1) while idle.
# Stage 2: Arms a falling-edge capture on CH24 (optional ON_ARMED_CMD triggers J501 DO1 once armed)
#          and verifies at least one active-low pulse/edge with lossless JSON + VCD artifacts.
#
# Usage:
#   ./scripts/hil_j501_do1.sh [--probe-only] [--out-dir ./hil_out] [--on-armed "ssh jetson ..."]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLI="${REPO_ROOT}/build/src/cli/logictracer-cli"
PORT="${LA_PORT:-/dev/ttyACM0}"
OUT_DIR="${REPO_ROOT}/build/hil_j501_do1"
PROBE_ONLY=0
ON_ARMED=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --probe-only) PROBE_ONLY=1; shift ;;
        --out-dir) OUT_DIR="$2"; shift 2 ;;
        --on-armed) ON_ARMED="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        *) echo "Unknown option: $1" >&2; exit 64 ;;
    esac
done

echo "[1/2] Probing CH24 (GP28 / Pin 34) on ${PORT}..."
PROBE_JSON="$("${CLI}" probe --port "${PORT}" --channels 24:J501_DO1 --json)"
STATE="$(python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["channels"][0]["state"])' <<<"${PROBE_JSON}")"
DIAG="$(python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["channels"][0]["message"])' <<<"${PROBE_JSON}")"
ELAPSED="$(python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["elapsed_ms"])' <<<"${PROBE_JSON}")"

echo "      CH24 (GP28) state = ${STATE} (${ELAPSED} ms): ${DIAG}"
if [[ "${STATE}" != "HIGH" ]]; then
    echo "FAIL: Expected HIGH (pull-up OK). Check the 4.7k-10k resistor between Pin 36 (3V3_OUT) and Pin 34 (GP28), and ensure J501 DO1 is OFF." >&2
    exit 1
fi
echo "PASS: Pull-up to 3.3V verified on CH24 (GP28)!"

if [[ "${PROBE_ONLY}" -eq 1 ]]; then
    exit 0
fi

mkdir -p "${OUT_DIR}"
TS="$(date -u +%Y%m%dT%H%M%SZ)"
JSON_OUT="${OUT_DIR}/j501_do1_${TS}.json"
VCD_OUT="${OUT_DIR}/j501_do1_${TS}.vcd"
READY_FILE="${OUT_DIR}/armed.flag"
rm -f "${READY_FILE}"

echo "[2/2] Arming falling-edge capture on CH24 (1 MHz, 20 ms window, 10 s timeout)..."
EXTRA_ARGS=(--ready-file "${READY_FILE}")
if [[ -n "${ON_ARMED}" ]]; then
    EXTRA_ARGS+=(--on-armed "${ON_ARMED}")
else
    echo "      Toggle J501 DO1 ON now (waiting up to 10 s for falling edge)..."
fi

"${CLI}" capture \
    --port "${PORT}" \
    --freq 1M \
    --samples 20k \
    --channels 24:J501_DO1 \
    --trigger 24:falling \
    --timeout 10000 \
    --expect "J501_DO1:falling_edges>=1" \
    --out-json "${JSON_OUT}" \
    --out-vcd "${VCD_OUT}" \
    --summary \
    "${EXTRA_ARGS[@]}"

echo "Saved lossless JSON: ${JSON_OUT}"
echo "Saved VCD waveform:  ${VCD_OUT}"
