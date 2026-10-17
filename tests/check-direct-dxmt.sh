#!/bin/zsh
# Load the private DXMT libraries and exercise their native fullscreen call.
set -euo pipefail
repo=${0:A:h:h}
cx_root=${CROSSOVER_APP:-/Applications/CrossOver Preview.app}/Contents/SharedSupport/CrossOver
source "$repo/tools/gamemode/launch-env.sh"
mkdir -p "$repo/dll/build"
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -Werror -o "$repo/dll/build/probe-direct-dxmt.exe" \
  "$repo/tests/probe_direct_dxmt.c" -ld3d11 -ldxgi
sc6_env_list+=("CX_GAMEMODE_DRY_RUN=1" "DXMT_CONFIG=dxgi.nativeFullscreen=False;dxgi.metalLayerPixelFormat=bgr10a2;d3d11.metalSpatialUpscale=False;")
sc6_encode_env "${sc6_env_list[@]}"
"$cx_root/bin/wine" --bottle "${SC6_BOTTLE:-SC6-Rosetta}" --env "$sc6_env_arg" "$repo/dll/build/probe-direct-dxmt.exe"
