#!/bin/zsh
# Launch SC6 with unlocked rendering and world logic at 60 Hz.
# Usage: dll/launch.sh [maximum-fps]. The default, 0, removes the engine limit.
set -euo pipefail
here=${0:A:h}
repo=${here:h}
bottle=${SC6_BOTTLE:-SC6-Rosetta}
win64=${SC6_WIN64:-$HOME/Library/Application Support/CrossOver/Bottles/$bottle/drive_c/Games/SoulCalibur 6/SoulcaliburVI/Binaries/Win64}
wine=${CROSSOVER_APP:-/Applications/CrossOver Preview.app}/Contents/SharedSupport/CrossOver/bin/wine
cx_root=${wine:h:h}
maximum=${1:-0}
[[ $maximum == <-> ]] || { print -u2 "The maximum must be a nonnegative integer."; exit 1; }
pgrep -f -q '[S]oulcaliburVI.exe' && { print -u2 "Quit SoulCalibur VI before launching it again."; exit 1; }
[[ -f $win64/SoulcaliburVI.exe ]] || { print -u2 "The game is missing: $win64/SoulcaliburVI.exe"; exit 1; }
source "$repo/tools/gamemode/launch-env.sh"
"$here/build.sh" >/dev/null
if [[ -f $win64/dinput8.dll ]] && ! grep -q -a SC6VRR_SHADER_DIR "$win64/dinput8.dll"; then
  print -u2 "A different dinput8.dll is installed. Preserve it before using this launcher."
  exit 1
fi
if [[ -f $win64/dinput8.dll ]] && ! cmp -s "$here/build/dinput8.dll" "$win64/dinput8.dll"; then
  cp -p "$win64/dinput8.dll" "$win64/dinput8.dll.before-unlock-$(date +%Y%m%d-%H%M%S)"
fi
SC6_BOTTLE=$bottle SC6_WIN64=$win64 "$here/install.sh"
sc6_env_list+=(
  "SC6VRR_FRAME_MODS=1" "SC6VRR_UNCAP=1" "SC6VRR_GATE=1" "SC6VRR_MAX_FPS=$maximum"
  "SC6VRR_MEASURE=1" "SC6VRR_FRAME_TIMERS=0" "SC6VRR_SHADER_DIR=" "SC6VRR_SKIP_GS_DRAWS=0"
)
sc6_encode_env "${sc6_env_list[@]}"
print -rl -- "${sc6_env_list[@]}" > "$win64/unlocked-environment.txt"
"$wine" --bottle "$bottle" --env "$sc6_env_arg" --workdir "$win64" --start "$win64/SoulcaliburVI.exe" \
  > "$win64/wine-unlocked.log" 2>&1 &
print "Launched in $bottle. Engine limit: $maximum (0 means unlimited). Logic: 60 Hz."
print "Log: $win64/sc6vrr.log"
print "Presentation: native fullscreen with BGR10A2. The Game Mode policy follows the game process."
