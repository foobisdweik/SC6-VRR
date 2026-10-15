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
  print "Native fullscreen states (2 windowed, 3 not covering screen, 4 requested, 5 active, 6 gave up):"
  cat $dxmt_logs | grep 'native fullscreen state' | sed 's/^/  /' | head -10
  print "Errors (first 15 unique):"
  cat $dxmt_logs | grep -E '^err:|error' | sed 's/^[^:]*: //' | sort | uniq -c | sort -rn | head -15
fi

section "sc6vrr DLL (sc6vrr.log)"
if [[ -f sc6vrr.log ]]; then
  grep -v "frames=" sc6vrr.log | head -20
  awk '/frames=/ { for (i=1;i<=NF;i++) { if ($i ~ /^frames=/) { split($i,a,"="); f+=a[2] } if ($i ~ /^steps=/) { split($i,b,"="); s+=b[2] } } n++ }
       END { if (n) printf "per-second windows: %d, avg engine frames/s: %.1f, avg battle steps/s: %.1f\n", n, f/n, s/n }' sc6vrr.log
else
  print "no sc6vrr.log (DLL not loaded?)"
fi

section "Game process (proc.tsv)"
awk -F'\t' 'NR>1 {n++; cpu+=$3; if ($4>rss) rss=$4; if (!first) first=$1; last=$1}
  END {if (n) printf "samples: %d (%s to %s)\navg cpu: %.0f%%\nmax rss: %.0f MB\n", n, first, last, cpu/n, rss/1024; else print "no samples"}' proc.tsv

section "Game Mode (gamepolicyd.log)"
print "lines: $(wc -l < gamepolicyd.log | tr -d ' ')"
grep -i -E "game ?mode|GameSession|foreground|fullscreen|sc6vrr|wineloader" gamepolicyd.log | tail -15 || true
[[ -f gamemode-status.txt ]] && { print "status during run:"; cat gamemode-status.txt }

section "Frame rate (Metal HUD frame counter, per log line)"
# fps = change in the frame counter / change in time between HUD lines.
awk '/metal-HUD:/ {
    split($2, hms, ":"); t = hms[1]*3600 + hms[2]*60 + hms[3]
    sub(/.*metal-HUD: /, ""); split($0, f, ","); n = f[1] + 0
    if (have && t > pt && n >= pn) {
      fps = (n - pn) / (t - pt); c++; sum += fps
      if (!c0 || fps < mn) mn = fps; if (fps > mx) mx = fps; c0 = 1
      if (fps < 55) low++; if (fps > 65) high++
    }
    pt = t; pn = n; have = 1
  }
  END { if (c) printf "intervals: %d\navg fps: %.1f  min: %.1f  max: %.1f\nintervals under 55 fps: %d  over 65 fps: %d\n", c, sum/c, mn, mx, low, high
        else print "no HUD frame data" }' metal.log

section "Metal cache folders written in this run"
cat metal-cache-dirs.txt 2>/dev/null
