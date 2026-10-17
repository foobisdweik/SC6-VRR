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
#   3. Applies the case's cache policy. It can preserve caches or clear DXMT's
#      cache and the Metal caches recorded for the SC6 process.
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
DESCRIPTION=""; LAUNCH=bundle; RETINA_SC6=unset; GAMEMODE=auto; PROFILE=0; PROFILE_WINDOW=10; PROFILE_GAP=0; PROFILE_INTERVAL_MS=1; METAL_TRACE=0; THREAD_METRICS=0; GRAPHICS_BACKEND=dxmt; CLEAR_SC6_CACHES=1; TEST_ENV=()
PLAY_INSTRUCTION="In game, perform an attack."
source $case_file
case $GRAPHICS_BACKEND in
  dxmt) backend_dll=$cx_root/lib/dxmt/x86_64-windows/d3d11.dll ;;
  d3dmetal) backend_dll=$cx_root/lib64/apple_gptk/wine/x86_64-windows/d3d11.dll ;;
  *) print -u2 "Unsupported GRAPHICS_BACKEND: $GRAPHICS_BACKEND"; exit 1 ;;
esac
[[ -f $backend_dll ]] || { print -u2 "Missing graphics DLL: $backend_dll"; exit 1; }
[[ $CLEAR_SC6_CACHES == 0 || $CLEAR_SC6_CACHES == 1 ]] || { print -u2 "CLEAR_SC6_CACHES must be 0 or 1"; exit 1; }
if (( METAL_TRACE )) && [[ ! -x /usr/bin/metalperftrace ]]; then
  print -u2 "This case needs /usr/bin/metalperftrace. No run was started."; exit 1
fi

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
process_name=${game_exe:t}
# Wine shows each Windows process with its Windows command line (for example
# "cmd /c ver ..."), so match that text, not the loader path.
if (( dry_run )); then proc_pattern='cmd /c ver'; process_name=cmd; fi

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
# The bottle was idle before the registry command. Close the services that this
# command started so a persistent server cannot block the cache preparation.
note "Closing the bottle services started by the registry command"
"$wine" --bottle $bottle --ux-app "$cx_root/bin/wineserver" -k >>$run_dir/harness.log 2>&1
wait_for_idle_bottle

# --- 2. Clear SC6's shader caches ---------------------------------------------
if (( CLEAR_SC6_CACHES )); then
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
else
  note "Preserving existing shader caches for steady gameplay measurements"
fi
touch $run_dir/.start-marker

# --- 3. Environment -----------------------------------------------------------
# Passed with `wine --env`, because the wine script applies --env after the
# bottle's [EnvironmentVariables], which would otherwise override inherited values.
dxmt_log_win="Z:${run_dir//\//\\}\\dxmt"
env_list=(
  "WINELOADER=$loader" "CX_WINELOADER=$loader"
  "CX_GRAPHICS_BACKEND=$GRAPHICS_BACKEND"
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
  print "utc_offset: $(date +%z)"
  print "launch: $LAUNCH ($loader)"
  print "retina_sc6: $RETINA_SC6"
  print "graphics_backend_requested: $GRAPHICS_BACKEND"
  print "clear_sc6_caches: $CLEAR_SC6_CACHES"
  print "selected d3d11.dll: $backend_dll"
  print "selected d3d11.dll sha256: $(shasum -a 256 $backend_dll | cut -d' ' -f1)"
  print "crossover: $(plutil -extract CFBundleShortVersionString raw $crossover_app/Contents/Info.plist)"
  print "dxmt d3d11.dll sha256: $(shasum -a 256 $cx_root/lib/dxmt/x86_64-windows/d3d11.dll | cut -d' ' -f1)"
  print "dxmt-SC6 HEAD: $(git -C $HOME/Projects/dxmt-SC6 log -1 --format='%h %s' 2>/dev/null)"
  print "exe sha256: $(shasum -a 256 $game_exe | cut -d' ' -f1)"
  if [[ -f ${game_exe:h}/dinput8.dll ]]; then
    print "sc6vrr DLL sha256: $(shasum -a 256 ${game_exe:h}/dinput8.dll | cut -d' ' -f1)"
  fi
} > $run_dir/meta.txt

# --- 4. Background loggers ----------------------------------------------------
pids=()
/usr/bin/log stream --style compact --info --debug --predicate 'process == "gamepolicyd"' \
  > $run_dir/gamepolicyd.log 2>&1 &
pids+=$!
# Metal Performance HUD statistics (MTL_HUD_LOG_ENABLED) are collected after the
# run with `log show`; `log stream` did not deliver them (seen 2026-10-15).
hud_start=$(date '+%Y-%m-%d %H:%M:%S')
# Verify the native Wine path, retain each PID, and record cumulative CPU time.
# Pass the pattern through the environment so pgrep cannot match this collector.
SC6_PROCESS_CX_ROOT="$cx_root" SC6_PROCESS_PATTERN="$proc_pattern" SC6_PROCESS_NAME="$process_name" SC6_THREAD_METRICS="$THREAD_METRICS" \
  python3 -I "$here/measurements.py" collect "$run_dir" > $run_dir/process-collector.log 2>&1 &
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
print "  3. $PLAY_INSTRUCTION"
print "  4. Quit the game. The summary is written when the game exits."
print "To measure battle separately, use another terminal:"
print "  $here/mark.sh latest battle-start   (when Training is ready)"
print "  $here/mark.sh latest battle-end     (before leaving Training)"
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
# Preserve mapped files to check which graphics implementation actually loaded.
# libd3dshared alone cannot identify it because CrossOver uses it for both.
(
  for i in {1..120}; do
    mapped_pid=$(cat $run_dir/game-pid.txt 2>/dev/null || true)
    if [[ -n $mapped_pid ]]; then
      sleep 10
      /usr/sbin/lsof -p "$mapped_pid" > $run_dir/loaded-files.txt 2> $run_dir/loaded-files.log || true
      break
    fi
    sleep 1
  done
) &
pids+=$!
# Record the Game Mode status once the game has had time to go fullscreen.
( sleep 30; [[ -x $gamepolicyctl ]] && "$gamepolicyctl" game-mode status 2>&1 | sed 's/\x1b\[[0-9;]*m//g' > $run_dir/gamemode-status.txt ) &
# PROFILE=1: CPU samples of the game process with macOS `sample`, in
# back-to-back windows of PROFILE_WINDOW seconds (PROFILE_GAP seconds apart,
# 0 = continuous), one stack snapshot every PROFILE_INTERVAL_MS milliseconds.
# Sampling pauses the process and can severely reduce its frame rate.
# Use these runs to investigate functions, not to compare absolute performance.
if (( PROFILE )); then
  (
    n=0
    sleep 10
    while pgrep -f -q "$proc_pattern"; do
      gpid=$(cat $run_dir/game-pid.txt 2>/dev/null || true)
      if [[ -z $gpid ]]; then sleep 1; continue; fi
      /usr/bin/sample $gpid $PROFILE_WINDOW $PROFILE_INTERVAL_MS -mayDie \
        -file $run_dir/sample-$(printf %03d $((++n)))-$(date +%H%M%S).txt >/dev/null 2>&1
      (( PROFILE_GAP > 0 )) && sleep $PROFILE_GAP
    done
  ) &
fi
while pgrep -f -q "$proc_pattern"; do sleep 2; done
note "Game process exited"
wait $launcher_pid 2>/dev/null || true
wait_for_idle_bottle || true
stop_loggers
[[ -s $run_dir/process-collector.log ]] && note "Process collector output: $run_dir/process-collector.log"
restore_gamemode
trap - EXIT
/usr/bin/log show --start "$hud_start" --style compact --info \
  --predicate 'process == "wineloader" AND subsystem == "com.apple.metal.hud"' > $run_dir/metal.log 2>/dev/null || true
if (( METAL_TRACE )); then
  note "Exporting Metal metrics for the marked battle interval"
  python3 -I "$here/record_window.py" collect-metal "$run_dir" > $run_dir/metal-export.txt 2>&1 || \
    note "Metal export failed; see metal-export.txt. CPU measurements remain separate."
fi

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
