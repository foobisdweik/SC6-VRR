#!/bin/zsh
# Build "SoulCalibur VI.app", the double-clickable SC6 launcher for the
# CrossOver Preview SC6-Rosetta bottle. It keeps the presentation fixes
# (private DXMT build, native fullscreen, BGR10A2 layer, MetalFX off), lets
# macOS turn on Game Mode by itself, keeps stock frame timing, and writes no
# logs.
#
# Why two bundles: gamepolicyd labels a process as a game only when the
# process is the declared executable of a bundle with a game Info.plist
# (verified 2026-10-17, see notes/gamemode.md). So the game bundle's
# executable must be the Wine loader itself, and the loader needs the game's
# path as an argument. A double-click passes no arguments, so the outer
# launcher bundle opens the inner game bundle with that argument and exits.
#
# Inner game bundle (Contents/Helpers/SoulCalibur VI Game.app):
# - CFBundleExecutable is a copy of CrossOver's Wine loader. Run without
#   winewrapper.exe, the loader runs the .exe given as its argument in-process.
# - The loader loads ntdll.so from its own folder, so a symlink there points to
#   CrossOver's ntdll.so. Wine finds its other libraries through WINEDLLPATH.
# - LSEnvironment carries CrossOver's Wine environment, captured from
#   CrossOver's own `wine` script at build time, plus the settings below.
# - LaunchServices refuses a bundle whose executable is signed by another team
#   ("Launch requires secure launch with spawn constraints"), so the bundle is
#   signed ad hoc. The loader keeps its hardened runtime flag and entitlements.
#
# Settings: sc6vrr.ini next to the game exe, read at every double-click.
# - The launcher reads [launch] (Metal HUD, DXMT options, logs) with a copy of
#   ini-env.sh and passes the values to the game bundle with `open --env`.
#   These keys are therefore left out of LSEnvironment.
# - The sc6vrr dinput8.dll loads as in every other launch, and it reads
#   [sc6vrr] itself. SC6VRR_LOG=none turns its log off.
# - Wine output goes to /dev/null. The Game Mode helper is not used, because
#   macOS turns Game Mode on for this game bundle by itself.
#
# Rebuild after updating CrossOver Preview or the custom DXMT; the launcher
# refuses to start when either changed.
set -euo pipefail
here=${0:A:h}
repo=${here:h:h}
out_dir=${1:-$HOME/Applications/SC6-VRR}
bottle=${SC6_BOTTLE:-SC6-Rosetta}
win64=${SC6_WIN64:-$HOME/Library/Application Support/CrossOver/Bottles/$bottle/drive_c/Games/SoulCalibur 6/SoulcaliburVI/Binaries/Win64}
cx_root=${CROSSOVER_APP:-/Applications/CrossOver Preview.app}/Contents/SharedSupport/CrossOver
bundle_id=${SC6_BUNDLE_ID:-local.sc6vrr.soulcalibur6}
app="$out_dir/SoulCalibur VI.app"
game="$app/Contents/Helpers/SoulCalibur VI Game.app"
dxmt_files=(x86_64-windows/{d3d10core,d3d11,dxgi,winemetal,nvapi64,nvngx}.dll x86_64-unix/winemetal.so)
pgrep -f -q '[S]oulcaliburVI' && { print -u2 "Quit SoulCalibur VI before rebuilding its bundle."; exit 1; }
[[ -f $win64/SoulcaliburVI.exe ]] || { print -u2 "The game is missing: $win64/SoulcaliburVI.exe"; exit 1; }

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

# The presentation settings, shared with dll/launch.sh. This also checks that
# Preview's DXMT matches the staged custom build.
source "$repo/tools/gamemode/launch-env.sh"
# Keep only the fixed values; the launcher adds the [launch] values each time.
sc6_env_list=("WINEDLLOVERRIDES=d3d11,dxgi,winemetal=b" "CX_GRAPHICS_BACKEND=dxmt")

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources" "$game/Contents/MacOS" "$game/Contents/Resources"
cp -p "$loader" "$game/Contents/MacOS/wine"
ln -s "${loader:h}/ntdll.so" "$game/Contents/MacOS/ntdll.so"
{
  grep -E '^(CX_|WINE|GST_|DOTNET_|ROSETTA_|PATH=|LANG=|LC_)' "$work/env.txt" |
    grep -v -E '^(WINELOADER|CX_INITIALIZED|WINEDLLOVERRIDES|WINEDEBUG|CX_GRAPHICS_BACKEND)='
  print -r -- "WINELOADER=$loader"
  print -rl -- "${sc6_env_list[@]}"
} > "$work/wine.env"

# Hashes the launcher checks: CrossOver's loader and Preview's DXMT files.
{
  shasum -a 256 "$loader"
  for f in $dxmt_files; do shasum -a 256 "$cx_root/lib/dxmt/$f"; done
} > "$app/Contents/Resources/expected.sha256"

write_plist() {  # path, then key=value pairs read by the Python below
  ENV_FILE="$work/wine.env" /usr/bin/python3 - "$@" <<'PY'
import os, plistlib, sys
path, kind = sys.argv[1], sys.argv[2]
d = dict(a.split("=", 1) for a in sys.argv[3:])
plist = {
    "CFBundleIdentifier": d["id"], "CFBundleName": "SoulCalibur VI",
    "CFBundleExecutable": d["exe"], "CFBundlePackageType": "APPL",
    "CFBundleShortVersionString": "1.0", "CFBundleVersion": "1",
    "LSApplicationCategoryType": "public.app-category.games",
    "LSMinimumSystemVersion": "14.0",
}
if kind == "game":
    plist.update(GCSupportsGameMode=True, LSSupportsGameMode=True, NSHighResolutionCapable=True,
                 LSEnvironment=dict(l.rstrip("\n").split("=", 1) for l in open(os.environ["ENV_FILE"]) if "=" in l))
else:
    plist.update(LSUIElement=True)  # the launcher exits at once; no Dock icon
plistlib.dump(plist, open(path, "wb"))
PY
}
write_plist "$game/Contents/Info.plist" game "id=$bundle_id" exe=wine
write_plist "$app/Contents/Info.plist" launcher "id=$bundle_id.launcher" exe=launch

cat > "$app/Contents/MacOS/launch" <<EOF
#!/bin/zsh
# Check that CrossOver's loader and Preview's DXMT still match this build,
# then open the game bundle with the game's path and exit.
here=\${0:A:h}
alert() { osascript -e "display alert \"SoulCalibur VI\" message \"\$1\"" >/dev/null; exit 1; }
# Match only the game itself: the game bundle, or a CrossOver launch whose
# command line is the Windows path. A plain name match also finds any process
# that mentions the game folder.
[[ -n "\$(lsappinfo find bundleid=$bundle_id)" ]] && alert "SoulCalibur VI is already running."
pgrep -f -q '^[A-Za-z]:.*SoulcaliburVI[.]exe' && alert "SoulCalibur VI is already running."
shasum -a 256 -c --status "\$here/../Resources/expected.sha256" 2>/dev/null ||
  alert "CrossOver Preview or its DXMT changed. Run tools/gamemode/build-ls-app.sh in SC6-VRR again."
# Read sc6vrr.ini now, so every double-click uses the file's current values.
sc6_ini=${(q)win64}/sc6vrr.ini
source "\$here/../Resources/ini-env.sh"
env_args=()
for kv in "\${sc6_launch_env[@]}"; do env_args+=(--env "\$kv"); done
exec open "\${env_args[@]}" -a "\$here/../Helpers/SoulCalibur VI Game.app" --args ${(q)win64}/SoulcaliburVI.exe
EOF
chmod +x "$app/Contents/MacOS/launch"
cp "$here/ini-env.sh" "$app/Contents/Resources/ini-env.sh"

codesign --force -s - --preserve-metadata=entitlements,flags "$game"
codesign --force -s - "$app"
lsregister=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
$lsregister -f "$game"
$lsregister -f "$app"
print "Built: $app"
