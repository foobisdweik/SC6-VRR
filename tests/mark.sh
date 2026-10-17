#!/bin/zsh
# Mark the start or end of a battle interval in a harness run.
# Usage: tests/mark.sh <run-dir|latest> <battle-start|battle-end>
set -euo pipefail
here=${0:A:h}
run_dir=${1:?Choose a run directory or latest}
marker=${2:?Choose battle-start or battle-end}
if [[ $run_dir == latest ]]; then
  runs=($here/runs/*(/Nom))
  (( ${#runs} )) || { print -u2 "No run directory exists."; exit 1; }
  run_dir=$runs[1]
fi
python3 -I "$here/measurements.py" mark "$run_dir" --marker "$marker"
