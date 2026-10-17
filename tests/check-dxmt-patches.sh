#!/bin/zsh
# Exercise the installed Preview DXMT through public D3D11 and Direct2D APIs.
set -euo pipefail
repo=${0:A:h:h}
cx_root=${CROSSOVER_APP:-/Applications/CrossOver Preview.app}/Contents/SharedSupport/CrossOver
source "$repo/tools/gamemode/launch-env.sh"
out=${1:-$repo/tests/runs/dxmt-patches-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$out"
x86_64-w64-mingw32-g++ -O2 -Wall -Wextra -Werror -Wno-cast-function-type \
  -o "$out/test-dxmt-patches.exe" "$repo/tests/test_dxmt_patches.cpp" \
  -ld3d11 -ldxgi -ldxguid -ld2d1 -static
sc6_env_list+=("CX_GAMEMODE_DRY_RUN=1" "DXMT_SHADER_CACHE=1" "MTL_HUD_ENABLED=0" \
  "DXMT_CONFIG=dxgi.nativeFullscreen=False;d3d11.pipelineArchive=False;")
sc6_encode_env "${sc6_env_list[@]}"
"$cx_root/bin/wine" --bottle "${SC6_BOTTLE:-SC6-Rosetta}" --env "$sc6_env_arg" \
  "$out/test-dxmt-patches.exe" > "$out/regression.log" 2>&1
cat "$out/regression.log"
