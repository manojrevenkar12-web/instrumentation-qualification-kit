#!/usr/bin/env bash
# Fast feedback for people and agents: generated files, build, unit tests, Python tests without the bench.
#   scripts/check.sh           everything fast (about a minute)
#   scripts/check.sh --system  plus the SITL system tests (about two minutes more)
set -euo pipefail
cd "$(dirname "$0")/.."
python3 -m iqk.cli icd check
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
ninja -C build
./build/iqk_tests --gtest_brief=1
python3 -m ruff check iqk tests
if [[ "${1:-}" == "--system" ]]; then
  python3 -m pytest -q tests
else
  python3 -m pytest -q -m "not system" tests
fi
