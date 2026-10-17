#!/bin/zsh
# Build "SoulCalibur VI.app", a game bundle that lets macOS turn on Game Mode
# for SC6 by itself (verified 2026-10-17, see notes/gamemode.md).
#
# Why: gamepolicyd labels a process as a game only when the process is the
# declared executable of an app bundle with a game Info.plist. CrossOver's
# `wine` script runs winewrapper.exe, which starts the game as a child process,
# so macOS tracks the game as an anonymous process and never detects it.
#
# How:
# - The bundle's CFBundleExecutable is a copy of CrossOver's Wine loader, so the
#   process that LaunchServices starts is the game itself. Run without
#   winewrapper.exe, the loader runs the .exe given as its argument in-process.
# - The loader loads ntdll.so from its own folder, so a symlink there points to
#   CrossOver's ntdll.so. Wine finds its other libraries through WINEDLLPATH.
# - LSEnvironment in Info.plist carries CrossOver's Wine environment, captured
#   from CrossOver's own `wine` script at build time, plus this project's
#   settings from launch-env.sh.
# - LaunchServices refuses to launch a bundle whose executable is signed by
#   another team ("Launch requires secure launch with spawn constraints"), so
#   the whole bundle is signed ad hoc. The loader keeps its hardened runtime
#   flag and entitlements.
#
# Rebuild after updating CrossOver Preview or the custom DXMT.
# Launch with tools/gamemode/launch-ls-app.sh.
set -euo pipefail
here=${0:A:h}
repo=${here:h:h}
out_dir=${1:-$HOME/Applications/SC6-VRR}
bottle=${SC6_BOTTLE:-SC6-Rosetta}
cx_root=${CROSSOVER_APP:-/Applications/CrossOver Preview.app}/Contents/SharedSupport/CrossOver
bundle_id=${SC6_BUNDLE_ID:-local.sc6vrr.soulcalibur6}
app="$out_dir/SoulCalibur VI.app"
pgrep -f -q '[S]oulcaliburVI' && { print -u2 "Quit SoulCalibur VI before rebuilding its bundle."; exit 1; }

# Capture the environment that CrossOver gives the first Wine process.
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cat > "$work/dump.sh" <<EOF
#!/bin/sh
env > "$work/env.txt"
exec "\$CX_WINELOADER" "\$@"
EOF
chmod +x "$work/dump.sh"
"$cx_root/bin/wine" --bottle "$bottle" --env "WINELOADER=\"$work/dump.sh\"" cmd /c ver >/dev/null 2>&1
[[ -s $work/env.txt ]] || { print -u2 "Could not capture CrossOver's Wine environment."; exit 1; }
loader=$(grep '^CX_WINELOADER=' "$work/env.txt" | cut -d= -f2-)

# The launcher's settings (DXMT, Metal HUD, stock frame timing), without its Game Mode helper.
source "$repo/tools/gamemode/launch-env.sh"
sc6_env_list=(${sc6_env_list:#WINELOADER=*})
sc6_env_list=(${sc6_env_list:#CX_GAMEMODE_*})
sc6_env_list+=(
  "SC6VRR_FRAME_MODS=0" "SC6VRR_SHADER_DIR=" "SC6VRR_SKIP_GS_DRAWS=0"
)

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp -p "$loader" "$app/Contents/MacOS/wine"
ln -s "${loader:h}/ntdll.so" "$app/Contents/MacOS/ntdll.so"
shasum -a 256 "$loader" | cut -d' ' -f1 > "$app/Contents/Resources/wine.sha256"
{
  grep -E '^(CX_|WINE|GST_|DOTNET_|ROSETTA_|DXMT_ENABLE_NVEXT=|PATH=|LANG=|LC_)' "$work/env.txt" |
    grep -v -E '^(WINELOADER|CX_INITIALIZED|WINEDLLOVERRIDES)='
  print -r -- "WINELOADER=$loader"
  print -rl -- "${sc6_env_list[@]}"
} > "$work/wine.env"

ENV_FILE="$work/wine.env" PLIST="$app/Contents/Info.plist" BUNDLE_ID="$bundle_id" /usr/bin/python3 - <<'PY'
import os, plistlib
env = dict(line.rstrip("\n").split("=", 1) for line in open(os.environ["ENV_FILE"]) if "=" in line)
plistlib.dump({
    "CFBundleIdentifier": os.environ["BUNDLE_ID"],
    "CFBundleName": "SoulCalibur VI",
    "CFBundleExecutable": "wine",
    "CFBundlePackageType": "APPL",
    "CFBundleShortVersionString": "1.0",
    "CFBundleVersion": "1",
    "LSApplicationCategoryType": "public.app-category.games",
    "GCSupportsGameMode": True,
    "LSSupportsGameMode": True,
    "NSHighResolutionCapable": True,
    "LSMinimumSystemVersion": "14.0",
    "LSEnvironment": env,
}, open(os.environ["PLIST"], "wb"))
PY

codesign --force -s - --preserve-metadata=entitlements,flags "$app"
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$app"
print "Built: $app"
