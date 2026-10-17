#!/bin/zsh
# Summarize one test run directory written by tests/run.sh.
# Usage: tests/summarize.sh <run-dir>
run_dir=${1:?run dir}
here=${0:A:h}
cd $run_dir || exit 1

section() { print; print "== $1" }

cat meta.txt
section "Harness"
cat harness.log

section "Graphics backend (CrossOver compat DB, from wine.log)"
grep -h -i "graphics backend" wine.log | sort | uniq -c | head -5 || true

dxmt_logs=(dxmt/*.log(N))
section "DXMT logs: ${#dxmt_logs} file(s)"
for f in $dxmt_logs; do print "  ${f:t}  $(wc -l < $f | tr -d ' ') lines"; done
if (( ${#dxmt_logs} )); then
  print "PSO compiles started:   $(cat $dxmt_logs | grep -c 'Start compiling 1 PSO')"
  print "PSO compiles finished:  $(cat $dxmt_logs | grep -c 'Compiled 1 PSO')"
  print "PSO failures:           $(cat $dxmt_logs | grep -c 'Failed to create')"
  print "MetalFX unsupported:    $(cat $dxmt_logs | grep -c 'MetalFX spatial scaler is not supported')"
  print "Native fullscreen states (2 windowed, 3 not covering screen, 4 requested, 5 active, 6 gave up):"
  cat $dxmt_logs | grep 'native fullscreen state' | sed 's/^/  /' | head -10
  print "Errors (first 15 unique):"
  cat $dxmt_logs | grep -E '^err:|error' | sed 's/^[^:]*: //' | sort | uniq -c | sort -rn | head -15
fi

section "sc6vrr DLL (sc6vrr.log)"
if [[ -f sc6vrr.log ]]; then
  grep -v -E 'frames=|metrics_v=' sc6vrr.log | head -20
  python3 -I "$here/measurements.py" summary . --section dll
else
  print "no sc6vrr.log (DLL not loaded?)"
fi

section "DXMT frame statistics (DXMT_LOG_FRAME_STATS)"
if (( ${#dxmt_logs} )) && cat $dxmt_logs | grep -q "frame-stats:"; then
  cat $dxmt_logs | grep "frame-stats:" | python3 -I -c '
import sys,re
rows=[dict((k,float(v)) for k,v in re.findall(r"(\w+)=([\d.]+)",l)) for l in sys.stdin]
keys=["commit_avg","sync_avg","present_latency","encode_prepare","encode_flush","drawable_wait","render_passes"]
print("samples:",len(rows))
for k in keys:
    v=[r[k] for r in rows if k in r]
    if v: print(f"  {k:16s} avg {sum(v)/len(v):7.2f}  max {max(v):7.2f}")
'
else
  print "none"
fi
for f in sample-*.txt(N); do print "CPU sample: $f"; done

section "Frame breakdown (SC6VRR_FRAME_TIMERS)"
python3 -I "$here/measurements.py" summary . --section detail

section "Thread CPU time (threads.tsv)"
python3 -I "$here/measurements.py" summary . --section threads

if [[ -f metal-status.json ]]; then
  section "Native Metal export (metal-status.json)"
  cat metal-status.json
fi

section "Game process (proc.tsv)"
python3 -I "$here/measurements.py" summary . --section cpu

section "Game Mode (gamepolicyd.log)"
print "lines: $(wc -l < gamepolicyd.log | tr -d ' ')"
grep -i -E "game ?mode|GameSession|foreground|fullscreen|sc6vrr|wineloader" gamepolicyd.log | tail -15 || true
[[ -f gamemode-status.txt ]] && { print "status during run:"; cat gamemode-status.txt }

section "Frame rate (Metal HUD counters, separated by process)"
python3 -I "$here/measurements.py" summary . --section hud

section "Metal cache folders written in this run"
cat metal-cache-dirs.txt 2>/dev/null
