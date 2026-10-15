#!/bin/zsh
# Summarize one test run directory written by tests/run.sh.
# Usage: tests/summarize.sh <run-dir>
run_dir=${1:?run dir}
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
  print "Errors (first 15 unique):"
  cat $dxmt_logs | grep -E '^err:|error' | sed 's/^[^:]*: //' | sort | uniq -c | sort -rn | head -15
fi

section "Game process (proc.tsv)"
awk -F'\t' 'NR>1 {n++; cpu+=$3; if ($4>rss) rss=$4; if (!first) first=$1; last=$1}
  END {if (n) printf "samples: %d (%s to %s)\navg cpu: %.0f%%\nmax rss: %.0f MB\n", n, first, last, cpu/n, rss/1024; else print "no samples"}' proc.tsv

section "Game Mode (gamepolicyd.log)"
print "lines: $(wc -l < gamepolicyd.log | tr -d ' ')"
grep -i -E "game ?mode|GameSession|foreground|fullscreen|sc6vrr|wineloader" gamepolicyd.log | tail -15 || true
[[ -f gamemode-status.txt ]] && { print "status during run:"; cat gamemode-status.txt }

section "Metal HUD log (metal.log)"
print "lines: $(wc -l < metal.log | tr -d ' ')"
tail -5 metal.log

section "Metal cache folders written in this run"
cat metal-cache-dirs.txt 2>/dev/null
