#!/bin/zsh
# Install the sc6vrr proxy dinput8.dll next to SoulcaliburVI.exe and make Wine
# load it for that exe only (per-app DLL override "native,builtin").
# Undo: delete Binaries/Win64/dinput8.dll and the AppDefaults key.
set -euo pipefail
here=${0:A:h}
bottle=${SC6_BOTTLE:-Steam}
win64=${SC6_WIN64:-$HOME/Games/SteamLibrary/steamapps/common/SoulCalibur 6/SoulcaliburVI/Binaries/Win64}
wine=${CROSSOVER_APP:-/Applications/CrossOver.app}/Contents/SharedSupport/CrossOver/bin/wine
[[ -f $here/build/dinput8.dll ]] || $here/build.sh
cp $here/build/dinput8.dll "$win64/dinput8.dll"
"$wine" --bottle $bottle reg add 'HKCU\Software\Wine\AppDefaults\SoulcaliburVI.exe\DllOverrides' \
  /v dinput8 /t REG_SZ /d native,builtin /f
print "installed: $win64/dinput8.dll"
