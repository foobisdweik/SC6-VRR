#!/bin/zsh
# Read SC6's files once, so macOS keeps them in its file cache (RAM). Launches
# and loads in the game then read from RAM instead of from the SSD.
#
# The LaunchAgent local.sc6vrr.warm-paks runs this at login and every 30
# minutes. A run reads the files again only when macOS has dropped part of them,
# because RAM is lost at each restart and macOS evicts cached pages when apps
# need memory. Cached pages never take RAM away from apps.
#
# On this Mac (32 GB) about 83% of the 19.6 GB of paks fits next to normal app
# use, so the files the game needs at launch are read last: macOS evicts the
# pages that were read first.
#
# Usage: warm-paks.sh [--now]   (--now skips the login delay and the residency check)
set -euo pipefail
zmodload zsh/datetime  # provides EPOCHREALTIME

here=${0:A:h}
game="$HOME/Library/Application Support/CrossOver/Bottles/SC6-Rosetta/drive_c/Games/SoulCalibur 6"
bottle="$HOME/Library/Application Support/CrossOver/Bottles/SC6-FEX"
cx="/Applications/CrossOver Preview.app/Contents/SharedSupport/CrossOver"
dxmt_cache="$(getconf DARWIN_USER_CACHE_DIR)dxmt/SoulcaliburVI.exe"
# Warm again only when less of the paks is resident than this (percent).
min_resident=${SC6_WARM_MIN_RESIDENT:-60}

stamp() { print "$(date '+%F %T') $*"; }

now=0
[[ ${1:-} == --now ]] && now=1
# At login, wait until the login items have started.
if (( ! now )); then
  boot=$(sysctl -n kern.boottime | sed -E 's/.*sec = ([0-9]+).*/\1/')
  (( EPOCHSECONDS - boot < 600 )) && sleep ${SC6_WARM_DELAY:-120}
fi
if pmset -g | grep -q 'lowpowermode *1'; then
  stamp "skipped: Low Power Mode is on"; exit 0
fi
# While the game runs, extra reads could cause stutter. The game keeps its own
# files cached by reading them.
if pgrep -f -q '[S]oulcaliburVI.exe'; then
  stamp "skipped: the game is running"; exit 0
fi
[[ -d $game ]] || { stamp "skipped: game folder missing"; exit 0; }

paks=( "$game"/SoulcaliburVI/Content/Paks/*.pak(N) "$game"/SoulcaliburVI/Content/Paks/~mods/*.pak(N) )
if (( ! now )); then
  resident=$(python3 -I "$here/resident.py" $paks | tail -1 | awk '{print int($1)}')
  if (( resident >= min_resident )); then
    stamp "skipped: ${resident}% of the paks is already in RAM"; exit 0
  fi
fi

# Order matters: pages read last stay cached longest.
files=(
  $paks
  "$game"/Engine/Binaries/ThirdParty/**/*.dll(N)
  "$game"/SoulcaliburVI/Binaries/Win64/*.(exe|dll|ini)(N)
  "$cx"/lib/wine/aarch64-windows/*(N.) "$cx"/lib/wine/aarch64-unix/*(N.)
  "$cx"/lib/dxmt/aarch64-windows/*(N.) "$cx"/lib/dxmt/aarch64-unix/*(N.)
  "$bottle"/*.reg(N)
  "$dxmt_cache"/**/*(N.)
)
start=$EPOCHREALTIME
# One process reads every file in order; a normal read goes through the file cache.
cat -- $files > /dev/null 2>&1 || true
bytes=$(stat -f %z -- $files | awk '{s+=$1} END{print s}')
resident=$(python3 -I "$here/resident.py" $paks | tail -1 | awk '{print $1}')
stamp "warmed ${#files} files, $(( bytes / 1000000 )) MB in $(printf %.1f $(( EPOCHREALTIME - start ))) s; paks in RAM: $resident"
