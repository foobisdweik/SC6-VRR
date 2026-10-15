#!/bin/zsh
# Run one SC6 test case with background logging.
#
# Usage: tests/run.sh <case>        (a file name in tests/cases, with or without .env)
#        tests/run.sh --list
#        SC6_DRY_RUN=1 tests/run.sh <case>   runs cmd.exe instead of the game, to test the harness
#
# What it does:
#   1. Checks that no Wine process is running (cache clearing and registry
#      changes need the bottle to be idle).
#   2. Sets the per-app Retina mode for SoulcaliburVI.exe, as the case asks.
#   3. Clears SC6's shader caches: DXMT's cache and the Metal caches that
#      earlier runs recorded for the SC6 process.
#   4. Starts the loggers, launches the game, and waits for it to exit.
#   5. Stops the loggers and writes summary.txt.
#
# Each run writes to tests/runs/<time>-<case>/ (git-ignored).
set -euo pipefail

here=${0:A:h}
repo=${here:h}
cases_dir=$here/cases
state_dir=$here/runs
metal_state=$state_dir/.sc6-metal-cache-dirs

crossover_app=${CROSSOVER_APP:-/Applications/CrossOver.app}
cx_root=$crossover_app/Contents/SharedSupport/CrossOver
wine=$cx_root/bin/wine
bottle=${SC6_BOTTLE:-Steam}
game_exe=${SC6_EXE:-$HOME/Games/SteamLibrary/steamapps/common/SoulCalibur 6/SoulcaliburVI/Binaries/Win64/SoulcaliburVI.exe}
bundle_loader="$HOME/Applications/SC6-VRR/SoulCalibur VI.app/Contents/MacOS/wineloader"
user_cache=$(getconf DARWIN_USER_CACHE_DIR)
dxmt_cache=${user_cache}dxmt/SoulcaliburVI.exe
retina_key='HKCU\Software\Wine\AppDefaults\SoulcaliburVI.exe\Mac Driver'

if [[ ${1:-} == --list || -z ${1:-} ]]; then
  for f in $cases_dir/*.env; do
    DESCRIPTION=""; source $f; print "${f:t:r}  $DESCRIPTION"
  done
  exit 0
fi

case_file=$cases_dir/${1%.env}.env
[[ -f $case_file ]] || { print -u2 "no such case: $case_file"; exit 1; }

# Case defaults, then the case file.
DESCRIPTION=""; LAUNCH=bundle; RETINA_SC6=unset; GAMEMODE=auto; TEST_ENV=()
source $case_file

# A running wineserver means the bottle is busy. Wine processes themselves show
# Windows command lines, and `log stream` filters mention "wineloader", so the
# server's path is the reliable signal.
wineserver_pattern='CrossOver[^ ]*/wineserver'
wine_running() { pgrep -f -q "$wineserver_pattern" }
wait_for_idle_bottle() {
  local i
  for i in {1..60}; do wine_running || return 0; sleep 1; done
  print -u2 "Wine processes are still running after 60 s:"; pgrep -fl "$wineserver_pattern" >&2
  return 1
}

dry_run=${SC6_DRY_RUN:-0}
proc_pattern='SoulcaliburVI\.exe'
# Wine shows each Windows process with its Windows command line (for example
# "cmd /c ver ..."), so match that text, not the loader path.
(( dry_run )) && proc_pattern='cmd /c ver'

# Xcode's gamepolicyctl can force Game Mode on (no admin rights needed).
# NotProton PR #156 uses the same tool.
gamepolicyctl=$(xcode-select -p 2>/dev/null)/usr/bin/gamepolicyctl
if [[ $GAMEMODE == on && ! -x $gamepolicyctl ]]; then
  print -u2 "GAMEMODE=on needs Xcode's gamepolicyctl; not found at $gamepolicyctl"; exit 1
fi

if wine_running; then
  print -u2 "A Wine process is running. Quit every CrossOver program first:"
  pgrep -fl "$wineserver_pattern" >&2
  exit 1
fi
if [[ $LAUNCH == bundle && ! -x $bundle_loader ]]; then
  print -u2 "missing $bundle_loader; run tools/gamemode/build-app.sh"; exit 1
fi

run_dir=$state_dir/$(date +%Y%m%d-%H%M%S)-${case_file:t:r}
mkdir -p $run_dir/dxmt
cp $case_file $run_dir/case.env
note() { print -r -- "[$(date +%H:%M:%S)] $*" | tee -a $run_dir/harness.log }

# The loader decides which macOS identity the game process has, and so which
# Metal cache folder it uses.
if [[ $LAUNCH == bundle ]]; then loader=$bundle_loader
else loader="$cx_root/CrossOver-Hosted Application/wineloader"; fi

# --- 1. Per-app Retina mode ---------------------------------------------------
case $RETINA_SC6 in
  y|n) note "Setting RetinaMode=$RETINA_SC6 for SoulcaliburVI.exe"
       "$wine" --bottle $bottle reg add "$retina_key" /v RetinaMode /t REG_SZ /d $RETINA_SC6 /f >>$run_dir/harness.log 2>&1 ;;
  unset) note "Removing the per-app RetinaMode (the bottle default applies)"
       "$wine" --bottle $bottle reg delete "$retina_key" /v RetinaMode /f >>$run_dir/harness.log 2>&1 || true ;;
  *) print -u2 "RETINA_SC6 must be y, n, or unset"; exit 1 ;;
esac
wait_for_idle_bottle

# --- 2. Clear SC6's shader caches ---------------------------------------------
note "Clearing DXMT cache: $dxmt_cache"
rm -rf "$dxmt_cache"
if [[ -f $metal_state ]]; then
  while IFS= read -r d; do
    [[ -n $d && -d ${user_cache}com.apple.metal/$d ]] || continue
    # Never clear the folder that every CrossOver game shares.
    if [[ $d == com.codeweavers.CrossOver.wineloader ]]; then
      note "Not clearing shared Metal cache $d (used by all CrossOver games)"; continue
    fi
    note "Clearing Metal cache: $d"; rm -rf "${user_cache}com.apple.metal/$d"
  done < $metal_state
else
  note "No Metal cache folders recorded yet; this run will record them"
fi
[[ $LAUNCH == direct ]] && note "NOTE: direct launch uses the shared Metal cache, so this run is not a full cold start"
touch $run_dir/.start-marker

# --- 3. Environment -----------------------------------------------------------
# Passed with `wine --env`, because the wine script applies --env after the
# bottle's [EnvironmentVariables], which would otherwise override inherited values.
dxmt_log_win="Z:${run_dir//\//\\}\\dxmt"
env_list=(
  "WINELOADER=$loader" "CX_WINELOADER=$loader"
  CX_GRAPHICS_BACKEND=dxmt
  DXMT_LOG_LEVEL=trace "DXMT_LOG_PATH=$dxmt_log_win"
  "SC6VRR_LOG=Z:${run_dir//\//\\}\\sc6vrr.log"
  MTL_HUD_ENABLED=1 MTL_HUD_LOG_ENABLED=1
  $TEST_ENV
)
env_arg=""
for kv in $env_list; do
  k=${kv%%=*}; v=${kv#*=}
  v=${v//\\/\\\\}; v=${v//\"/\\\"}
  env_arg+="$k=\"$v\" "
done
print -rl -- $env_list > $run_dir/env.txt

{
  print "case: ${case_file:t:r} - $DESCRIPTION"
  print "date: $(date)"
  print "launch: $LAUNCH ($loader)"
  print "retina_sc6: $RETINA_SC6"
  print "crossover: $(plutil -extract CFBundleShortVersionString raw $crossover_app/Contents/Info.plist)"
  print "dxmt d3d11.dll sha256: $(shasum -a 256 $cx_root/lib/dxmt/x86_64-windows/d3d11.dll | cut -d' ' -f1)"
  print "dxmt-SC6 HEAD: $(git -C $HOME/Projects/dxmt-SC6 log -1 --format='%h %s' 2>/dev/null)"
  print "exe sha256: $(shasum -a 256 $game_exe | cut -d' ' -f1)"
} > $run_dir/meta.txt

# --- 4. Background loggers ----------------------------------------------------
pids=()
/usr/bin/log stream --style compact --info --debug --predicate 'process == "gamepolicyd"' \
  > $run_dir/gamepolicyd.log 2>&1 &
pids+=$!
# Metal Performance HUD statistics (MTL_HUD_LOG_ENABLED) are collected after the
# run with `log show`; `log stream` did not deliver them (seen 2026-10-15).
hud_start=$(date '+%Y-%m-%d %H:%M:%S')
# Per-second CPU and memory of the game process.
{
  print "time\tpid\tcpu_pct\trss_kb"
  while true; do
    ps -axo pid=,pcpu=,rss=,args= | awk -v t="$(date +%H:%M:%S)" -v pat="$proc_pattern" '$0 ~ pat && !/awk/ {print t "\t" $1 "\t" $2 "\t" $3}'
    sleep 1
  done
} > $run_dir/proc.tsv 2>/dev/null &
pids+=$!
stop_loggers() { kill $pids 2>/dev/null || true; wait $pids 2>/dev/null || true }
restore_gamemode() { [[ $GAMEMODE == on ]] && "$gamepolicyctl" game-mode set auto >/dev/null 2>&1 || true }
trap 'stop_loggers; restore_gamemode' EXIT

if [[ $GAMEMODE == on ]]; then
  note "Forcing Game Mode on with gamepolicyctl (restored to automatic at the end)"
  "$gamepolicyctl" game-mode set on >>$run_dir/harness.log 2>&1
fi

# --- 5. Launch and wait -------------------------------------------------------
note "Launching SoulCalibur VI ($LAUNCH)"
print ""
print "Now in the game:"
print "  1. Get through the intro cards."
print "  2. Go through the menu into Training mode."
print "  3. In game, perform an attack."
print "  4. Quit the game. The summary is written when the game exits."
print ""
if (( dry_run )); then
  note "DRY RUN: starting cmd.exe for 20 s instead of the game"
  "$wine" --bottle $bottle --env "$env_arg" cmd /c "ver & ping -n 20 127.0.0.1 >nul" \
    > $run_dir/wine.log 2>&1 &
else
  "$wine" --bottle $bottle --env "$env_arg" --workdir "${game_exe:h}" --start "$game_exe" \
    > $run_dir/wine.log 2>&1 &
fi
launcher_pid=$!

started=0
for i in {1..120}; do
  if pgrep -f -q "$proc_pattern"; then started=1; break; fi
  sleep 1
done
(( started )) || { note "The game process did not appear within 120 s; see wine.log"; }
(( started )) && note "Game process started"
# Record the Game Mode status once the game has had time to go fullscreen.
( sleep 30; [[ -x $gamepolicyctl ]] && "$gamepolicyctl" game-mode status 2>&1 | sed 's/\x1b\[[0-9;]*m//g' > $run_dir/gamemode-status.txt ) &
while pgrep -f -q "$proc_pattern"; do sleep 2; done
note "Game process exited"
wait $launcher_pid 2>/dev/null || true
wait_for_idle_bottle || true
stop_loggers
restore_gamemode
trap - EXIT
/usr/bin/log show --start "$hud_start" --style compact --info \
  --predicate 'process == "wineloader" AND subsystem == "com.apple.metal.hud"' > $run_dir/metal.log 2>/dev/null || true

# --- 6. Record Metal cache folders written during the run -----------------------
new_dirs=()
for d in ${user_cache}com.apple.metal/*(N/); do
  [[ -n $(find "$d" -newer $run_dir/.start-marker -print -quit 2>/dev/null) ]] && new_dirs+=${d:t}
done
print -l $new_dirs > $run_dir/metal-cache-dirs.txt
if (( ${#new_dirs} )); then
  { [[ -f $metal_state ]] && cat $metal_state; print -l $new_dirs } | sort -u > $metal_state.tmp
  mv $metal_state.tmp $metal_state
fi

# --- 7. Summary -----------------------------------------------------------------
$here/summarize.sh $run_dir > $run_dir/summary.txt
cat $run_dir/summary.txt
