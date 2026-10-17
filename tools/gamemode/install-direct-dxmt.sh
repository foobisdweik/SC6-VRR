#!/bin/zsh
# Install the custom build into Preview's x86-64 DXMT, with backups.
set -euo pipefail
source_root=${SC6_DXMT_SOURCE:-$HOME/Projects/dxmt-SC6}
runtime=${SC6_DXMT_RUNTIME:-$HOME/Library/Application Support/SC6-VRR/runtimes/dxmt-direct-x86_64}
cx_root=${CROSSOVER_APP:-/Applications/CrossOver Preview.app}/Contents/SharedSupport/CrossOver
payload=(d3d10/d3d10core.dll d3d11/d3d11.dll dxgi/dxgi.dll winemetal/winemetal.dll nvapi/nvapi64.dll nvngx/nvngx.dll)
for item in "${payload[@]}" winemetal/unix/winemetal.so; do
  [[ -f $source_root/build/src/$item ]] || {
    print -u2 "The custom DXMT file is missing: $source_root/build/src/$item"; exit 1; }
done
strings "$source_root/build/src/d3d11/d3d11.dll" | rg -F 'dxgi.nativeFullscreen' >/dev/null || {
  print -u2 "This DXMT build does not provide native fullscreen."; exit 1; }
mkdir -p "$runtime/x86_64-windows" "$runtime/x86_64-unix"
for item in "${payload[@]}"; do
  cp -p "$source_root/build/src/$item" "$runtime/x86_64-windows/${item:t}"
done
cp -p "$source_root/build/src/winemetal/unix/winemetal.so" "$runtime/x86_64-unix/winemetal.so"
shasum -a 256 "$runtime"/x86_64-windows/*.dll "$runtime/x86_64-unix/winemetal.so" > "$runtime/SHA256SUMS"
git -C "$source_root" rev-parse HEAD > "$runtime/source-checkout-commit.txt"
cp "$source_root/build/version.h" "$runtime/build-version.h"
backup="$HOME/Library/Application Support/SC6-VRR/backups/preview-direct-dxmt-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$backup/x86_64-windows" "$backup/x86_64-unix"
for name in d3d10core.dll d3d11.dll dxgi.dll winemetal.dll nvapi64.dll nvngx.dll; do
  cp -p "$cx_root/lib/dxmt/x86_64-windows/$name" "$backup/x86_64-windows/$name"
done
cp -p "$cx_root/lib/dxmt/x86_64-unix/winemetal.so" "$backup/x86_64-unix/winemetal.so"
for name in d3d10core.dll d3d11.dll dxgi.dll winemetal.dll nvapi64.dll nvngx.dll; do
  cp -p "$runtime/x86_64-windows/$name" "$cx_root/lib/dxmt/x86_64-windows/$name"
done
cp -p "$runtime/x86_64-unix/winemetal.so" "$cx_root/lib/dxmt/x86_64-unix/winemetal.so"
print -r -- "$backup" > "$runtime/last-backup.txt"
print "Installed DXMT: $cx_root/lib/dxmt"
print "Backup: $backup"
