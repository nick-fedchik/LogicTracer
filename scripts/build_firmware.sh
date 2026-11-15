#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Mykola Fedchyk
# Reproducibly builds fast-poll V6_6 UF2 images (100 MHz standard + 200 MHz Turbo)
# for the Raspberry Pi Pico (RP2040). Pass --flash to flash the 100 MHz image onto /dev/ttyACM0.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK_DIR="${PICO_SDK_PATH:-$HOME/.pico-sdk/sdk/2.1.1}"
PICOTOOL_DIR="${PICOTOOL_FETCH_FROM_GIT_PATH:-$HOME/.pico-sdk/picotool}"
OUT_DIR="${REPO_ROOT}/build/firmware"
CLI="${REPO_ROOT}/build/src/cli/logictracer-cli"

if [[ ! -f "${SDK_DIR}/pico_sdk_init.cmake" ]]; then
    echo "ERROR: Pico SDK not found at ${SDK_DIR} (set PICO_SDK_PATH)" >&2
    exit 1
fi

mkdir -p "${OUT_DIR}"

build_variant() {
    local name="$1"
    local turbo="$2"
    local bdir="${REPO_ROOT}/build/firmware/build_${name}"
    rm -rf "${bdir}"
    cmake -S "${REPO_ROOT}/firmware/rp2040" -B "${bdir}" -G Ninja \
        -DPICO_SDK_PATH="${SDK_DIR}" \
        -DPICOTOOL_FETCH_FROM_GIT_PATH="${PICOTOOL_DIR}" \
        -DBOARD_TYPE=BOARD_PICO -DTURBO_MODE="${turbo}" >/dev/null
    cmake --build "${bdir}"
    cp "${bdir}/LogicAnalyzer.uf2" "${OUT_DIR}/LogicAnalyzer_PICO_V6_6_${name}.uf2"
    rm -rf "${bdir}"
}

build_variant "100MHz" 0
build_variant "200MHz_Turbo" 1
sha256sum "${OUT_DIR}"/LogicAnalyzer_PICO_V6_6_*.uf2

if [[ "${1:-}" == "--flash" ]]; then
    UF2="${OUT_DIR}/LogicAnalyzer_PICO_V6_6_100MHz.uf2"
    if [[ "${2:-}" == "--turbo" ]]; then
        UF2="${OUT_DIR}/LogicAnalyzer_PICO_V6_6_200MHz_Turbo.uf2"
    fi
    "${CLI}" bootloader --yes
    for _ in $(seq 1 20); do
        [[ -e /dev/disk/by-label/RPI-RP2 ]] && break
        sleep 0.5
    done
    sleep 1
    MNT="$(findmnt -rn -S /dev/disk/by-label/RPI-RP2 -o TARGET | head -n 1 || true)"
    CLEAN_MNT=""
    if [[ -z "${MNT}" ]]; then
        CLEAN_MNT="$(mktemp -d /tmp/rpi-rp2.XXXXXX)"
        sudo mount /dev/disk/by-label/RPI-RP2 "${CLEAN_MNT}"
        MNT="${CLEAN_MNT}"
    fi
    sudo cp "${UF2}" "${MNT}/"
    sync
    if [[ -n "${CLEAN_MNT}" ]]; then
        sudo umount "${CLEAN_MNT}" 2>/dev/null || true
        rmdir "${CLEAN_MNT}" 2>/dev/null || true
    fi
    for _ in $(seq 1 20); do
        [[ -e /dev/ttyACM0 ]] && break
        sleep 0.5
    done
    sleep 1
    "${CLI}" detect
fi
