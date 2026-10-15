#!/bin/zsh
# Build "SoulCalibur VI.app": a macOS game bundle that runs SC6 under CrossOver.
#
# Why: macOS grants Game Mode only to a fullscreen app whose bundle says it is a
# game (LSApplicationCategoryType, GCSupportsGameMode). CrossOver runs Windows
# programs with CrossOver-Hosted Application/wineloader, which sits outside any
# app bundle, so macOS sees no game. This bundle holds a copy of wineloader in
# Contents/MacOS, so the SC6 process gets this bundle's identity instead.
#
# How the copied loader still works: CrossOver's `wine` script adds
# $CX_ROOT/lib/wine to WINEDLLPATH, so a wineloader outside CrossOver.app can
# find ntdll.so (CrossOver's own "dock icon renaming hack" relies on this).
# The launcher passes WINELOADER through `wine --env`, which the script applies
# after it sets its own value, and Wine starts every child process (the game
# itself) with $WINELOADER.
#
# Usage: build-app.sh [output-dir]
# Environment overrides: CROSSOVER_APP, SC6_BOTTLE, SC6_EXE, SC6_BUNDLE_ID
set -euo pipefail

out_dir=${1:-$HOME/Applications/SC6-VRR}
crossover_app=${CROSSOVER_APP:-/Applications/CrossOver.app}
bottle=${SC6_BOTTLE:-Steam}
game_exe=${SC6_EXE:-$HOME/Games/SteamLibrary/steamapps/common/SoulCalibur 6/SoulcaliburVI/Binaries/Win64/SoulcaliburVI.exe}
bundle_id=${SC6_BUNDLE_ID:-com.foobisdweik.sc6vrr.soulcalibur6}

cx_root=$crossover_app/Contents/SharedSupport/CrossOver
src_loader="$cx_root/CrossOver-Hosted Application/wineloader"
app="$out_dir/SoulCalibur VI.app"

for f in "$src_loader" "$cx_root/bin/wine" "$game_exe"; do
  [[ -e $f ]] || { print -u2 "missing: $f"; exit 1; }
done
[[ -d "$HOME/Library/Application Support/CrossOver/Bottles/$bottle" ]] ||
  { print -u2 "missing bottle: $bottle"; exit 1; }

# Rebuild from scratch so a stale loader copy never survives a CrossOver update.
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"

cp "$src_loader" "$app/Contents/MacOS/wineloader"
cp "$crossover_app/Contents/Resources/CrossOver.icns" "$app/Contents/Resources/AppIcon.icns"
shasum -a 256 "$src_loader" | cut -d' ' -f1 > "$app/Contents/Resources/wineloader.sha256"

# Launcher. Paths are fixed at build time; rerun this script after moving
# CrossOver, the bottle, or the game.
cat > "$app/Contents/MacOS/sc6-launch" <<EOF
#!/bin/zsh
here=\${0:A:h}
loader="\$here/wineloader"
src_loader=${(q)src_loader}

# A CrossOver update replaces wineloader. The copy must match the rest of
# CrossOver's Wine build, so refuse to start with a stale copy.
if [[ "\$(shasum -a 256 "\$src_loader" | cut -d' ' -f1)" != "\$(<"\$here/../Resources/wineloader.sha256")" ]]; then
  osascript -e 'display alert "SoulCalibur VI.app is out of date" message "CrossOver was updated. Run tools/gamemode/build-app.sh again."'
  exit 1
fi

exec ${(q)cx_root}/bin/wine --bottle ${(q)bottle} \\
  --env "WINELOADER=\\"\$loader\\" CX_WINELOADER=\\"\$loader\\"" \\
  --workdir ${(q)game_exe:h} \\
  --start ${(q)game_exe} "\$@"
EOF
chmod +x "$app/Contents/MacOS/sc6-launch"

plist="$app/Contents/Info.plist"
plutil -create xml1 "$plist"
plutil -insert CFBundleIdentifier -string "$bundle_id" "$plist"
plutil -insert CFBundleName -string "SoulCalibur VI" "$plist"
plutil -insert CFBundleDisplayName -string "SoulCalibur VI" "$plist"
plutil -insert CFBundleExecutable -string "sc6-launch" "$plist"
plutil -insert CFBundleIconFile -string "AppIcon" "$plist"
plutil -insert CFBundlePackageType -string "APPL" "$plist"
plutil -insert CFBundleInfoDictionaryVersion -string "6.0" "$plist"
plutil -insert CFBundleShortVersionString -string "1.0" "$plist"
plutil -insert CFBundleVersion -string "1" "$plist"
plutil -insert LSMinimumSystemVersion -string "14.0" "$plist"
plutil -insert NSHighResolutionCapable -bool true "$plist"
# The Game Mode keys. LSApplicationCategoryType must be a game category.
plutil -insert LSApplicationCategoryType -string "public.app-category.games" "$plist"
plutil -insert GCSupportsGameMode -bool true "$plist"
plutil -insert LSSupportsGameMode -bool true "$plist"
plutil -lint "$plist" >/dev/null

# Ad hoc signature for the whole bundle. Without the hardened runtime, the
# copied loader may load CrossOver's CodeWeavers-signed libraries and map
# executable memory, as the original loader's entitlements allow.
codesign --force --sign - "$app/Contents/MacOS/wineloader"
codesign --force --sign - --identifier "$bundle_id" "$app"
codesign --verify --deep --strict "$app"

# Register with Launch Services so gamepolicyd can find the bundle record.
/System/Library/Frameworks/CoreServices.framework/Versions/A/Frameworks/LaunchServices.framework/Versions/A/Support/lsregister -f "$app"

print "built: $app"
