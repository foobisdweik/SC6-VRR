#!/bin/zsh
# Put the current sc6vrr build (dll/build/dinput8.dll) into the game folder,
# so every way of starting the game loads it, including the CrossOver apps
# such as "SoulcaliburVI (Game Mode) (SC6-FEX).app". Both bottles share that
# folder (SC6-FEX links C:\Games\SoulCalibur 6 to the SC6-Rosetta copy).
#
# The LaunchAgent local.sc6vrr.install-dll runs this script when the build
# changes and at login, so nobody needs to run it by hand. If the game is
# running, the script waits until it exits, because the game has the DLL open.
# The replaced DLL is kept as dinput8.dll.previous.
set -euo pipefail
here=${0:A:h}
build=$here/build/dinput8.dll
win64=${SC6_WIN64:-$HOME/Library/Application Support/CrossOver/Bottles/SC6-Rosetta/drive_c/Games/SoulCalibur 6/SoulcaliburVI/Binaries/Win64}
target=$win64/dinput8.dll
say() { print -r -- "$(date '+%F %T') $*"; }

[[ -f $build ]] || { say "no build at $build; nothing to install"; exit 0; }
[[ -d $win64 ]] || { say "game folder missing: $win64"; exit 1; }

# The linker may still be writing: wait until the file stops changing.
for _ in {1..30}; do
  a=$(stat -f %m:%z "$build"); sleep 1; b=$(stat -f %m:%z "$build")
  [[ $a == $b ]] && break
done

# The game keeps dinput8.dll open, so wait for it to exit (up to 12 hours).
waited=0
while pgrep -f -q '[S]oulcaliburVI.exe'; do
  (( waited++ == 0 )) && say "game is running; installing after it exits"
  (( waited > 4320 )) && { say "game still running after 12 h; not installed"; exit 0; }
  sleep 10
done

cmp -s "$build" "$target" && { say "already installed ($(shasum -a 256 "$build" | cut -c1-12))"; exit 0; }
# Never replace a dinput8.dll that is not the sc6vrr proxy (for example another mod's).
if [[ -f $target ]] && ! grep -q -a SC6VRR_ "$target"; then
  say "a different dinput8.dll is installed in $win64; not replacing it"
  exit 1
fi
[[ -f $target ]] && cp -p "$target" "$target.previous"
cp "$build" "$target.new"
mv -f "$target.new" "$target" # a rename, so a reader never sees a half-written file
say "installed $(shasum -a 256 "$target" | cut -c1-12) from $build"
