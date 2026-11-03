#!/usr/bin/env bash
# Regenerates every report the matrix, the release and the console read. About 30 minutes on 2 cores.
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null && ninja -C build
python3 -m iqk.cli bench --seconds 30 --report nominal --seed 21
python3 -m iqk.cli bench --seconds 24 --report campaign --seed 5 \
  --fault imu_bit_errors@4-8:ber=0.0002 --fault imu_garbage@4-8:rate=0.05 --fault imu_drop@8-10:rate=0.02 \
  --fault imu_silent@10-10.2 --fault imu_out_of_range@4-12:rate=0.005 --fault aam_silent@6-12:node=2 \
  --fault ptp_jitter@2-20:rate=0.1,ms=15 --fault ptp_outage@12-17
python3 -m iqk.cli qual evidence
python3 -m iqk.cli qual mutate
for c in hardening static sanitize repro sbom; do python3 -m iqk.cli assurance "$c" || true; done
python3 -m iqk.cli qual matrix || true
python3 -m iqk.cli qual release || true
python3 -m iqk.cli report
