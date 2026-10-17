#!/bin/zsh
# Launch SC6 through "SoulCalibur VI.app" so that macOS can turn on Game Mode.
# Build the bundle first with tools/gamemode/build-ls-app.sh.
set -euo pipefail
bottle=${SC6_BOTTLE:-SC6-Rosetta}
win64=${SC6_WIN64:-$HOME/Library/Application Support/CrossOver/Bottles/$bottle/drive_c/Games/SoulCalibur 6/SoulcaliburVI/Binaries/Win64}
app=${SC6_APP:-$HOME/Applications/SC6-VRR/SoulCalibur VI.app}
here=${0:A:h}
repo=${here:h:h}
pgrep -f -q '[S]oulcaliburVI' && { print -u2 "Quit SoulCalibur VI before launching it again."; exit 1; }
[[ -d $app ]] || { print -u2 "Build the bundle first: $here/build-ls-app.sh"; exit 1; }
# The loader copy must match the rest of CrossOver's Wine build.
loader=$(/usr/libexec/PlistBuddy -c 'Print :LSEnvironment:WINELOADER' "$app/Contents/Info.plist")
[[ "$(shasum -a 256 "$loader" | cut -d' ' -f1)" == "$(<"$app/Contents/Resources/wine.sha256")" ]] ||
  { print -u2 "CrossOver changed. Run $here/build-ls-app.sh again."; exit 1; }
# Install the current SC6-VRR hook DLL, as dll/launch.sh does.
"$repo/dll/build.sh" >/dev/null
SC6_BOTTLE=$bottle SC6_WIN64=$win64 "$repo/dll/install.sh" >/dev/null
open --stdout "$win64/wine-ls-app.log" --stderr "$win64/wine-ls-app.log" -a "$app" --args "$win64/SoulcaliburVI.exe"
print "Launched through $app. Log: $win64/wine-ls-app.log"
