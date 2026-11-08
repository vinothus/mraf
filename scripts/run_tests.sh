#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

printf '== schedule tests ==\n'
g++ -std=c++11 -O2 -Wall -Wextra tests/test_schedule.cpp -o /tmp/mraf-test-schedule
/tmp/mraf-test-schedule

printf '\n== simulator tests ==\n'
cd simulator
python3 -m unittest -v test_sim

printf '\n== python syntax ==\n'
python3 -m py_compile mraf_sim.py test_sim.py ../tools/gen_webui.py ../tools/gen_diagrams.py

printf '\nall tests passed\n'
