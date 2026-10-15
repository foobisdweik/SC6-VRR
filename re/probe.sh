#!/bin/zsh
# Run re/scripts/Probe.java against the read-only SC6 Ghidra project.
# Usage: re/probe.sh <outFile> <strings|decomp|xrefs> <items...>
set -e
here=${0:A:h}
GHIDRA_HEADLESS_MAXMEM=${GHIDRA_HEADLESS_MAXMEM:-8G} \
  /opt/homebrew/opt/ghidra/libexec/support/analyzeHeadless "$here/ghidra" SC6 \
  -process SoulcaliburVI.exe -noanalysis -readOnly \
  -scriptPath "$here/scripts" -postScript Probe.java "$@" \
  > "$here/logs/probe-last.log" 2>&1 || { tail -20 "$here/logs/probe-last.log"; exit 1; }
grep -E "ERROR|Exception" "$here/logs/probe-last.log" | head -5 || true
