#!/bin/zsh
# Copy the [launch] section of sc6vrr.ini into cxbottle.conf of every bottle
# whose C:\Games\SoulCalibur 6 is this game folder (SC6-Rosetta, and SC6-FEX
# through its symlink), so CrossOver GUI launches use the same settings.
# It also makes each of those bottles load the sc6vrr dinput8.dll for
# SoulcaliburVI.exe, because the DLL applies [sc6vrr] and [engine].
# dll/launch.sh runs this itself. Run it
# by hand after editing [launch] when you start the game from CrossOver.
# CrossOver resets WINELOADER and WINEDLLOVERRIDES, so the Game Mode helper
# stays specific to dll/launch.sh.
# MetalHUD also goes to the user defaults of CrossOver's SC6 launcher apps
# (~/Applications/CrossOver/SoulcaliburVI (<bottle>).app). Those apps run Wine
# in their own process, and in SC6-FEX the HUD ignored MTL_HUD_ENABLED from
# cxbottle.conf. MetalHUDForceEnabled is the HUD's documented app setting
# (man MetalPerformanceHUD).
set -euo pipefail
here=${0:A:h}
repo=${here:h}
bottles_dir="$HOME/Library/Application Support/CrossOver/Bottles"
win64=${SC6_WIN64:-$bottles_dir/SC6-Rosetta/drive_c/Games/SoulCalibur 6/SoulcaliburVI/Binaries/Win64}
wine=${CROSSOVER_APP:-/Applications/CrossOver Preview.app}/Contents/SharedSupport/CrossOver/bin/wine
sc6_ini=${SC6_INI:-$win64/sc6vrr.ini}
[[ -f $sc6_ini ]] || { print -u2 "Missing: $sc6_ini"; exit 1; }
source "$repo/tools/gamemode/ini-env.sh"
game=${win64:A:h:h:h}   # .../SoulCalibur 6, with symlinks resolved
synced=()

for conf in "$bottles_dir"/*/cxbottle.conf; do
  bottle=${conf:h:t}
  link="${conf:h}/drive_c/Games/SoulCalibur 6"
  [[ -e $link && ${link:A} == $game ]] || continue
  [[ -f $conf.before-sc6vrr-ini ]] || cp -p "$conf" "$conf.before-sc6vrr-ini"
  /usr/bin/python3 -I - "$conf" "${(j:,:)sc6_launch_keys}" "${sc6_launch_env[@]}" <<'PY'
import re, sys
path, owned, wanted = sys.argv[1], sys.argv[2].split(","), sys.argv[3:]
lines = open(path).read().split("\n")
start = next((i for i, l in enumerate(lines) if l.strip() == "[EnvironmentVariables]"), None)
if start is None:
    lines += ["", "[EnvironmentVariables]"]
    start = len(lines) - 1
end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("[")), len(lines))
key = lambda l: (re.match(r'\s*"([^"]+)"\s*=', l) or [None, None])[1]
body = [l for l in lines[start + 1:end] if key(l) not in owned]
while body and not body[-1].strip():
    body.pop()
for kv in wanted:
    k, v = kv.split("=", 1)
    body.append('"%s" = "%s"' % (k, v.replace("\\", "\\\\").replace('"', '\\"')))
lines[start + 1:end] = body + [""]
open(path, "w").write("\n".join(lines))
PY
  # The per-app override lives in the bottle registry. user.reg stores the
  # key name with doubled backslashes; add it only when it is missing.
  if ! grep -qF 'AppDefaults\\SoulcaliburVI.exe\\DllOverrides' "${conf:h}/user.reg" 2>/dev/null; then
    "$wine" --bottle "$bottle" reg add 'HKCU\Software\Wine\AppDefaults\SoulcaliburVI.exe\DllOverrides' \
      /v dinput8 /t REG_SZ /d native,builtin /f >/dev/null 2>&1
  fi
  synced+=($bottle)
  print "Applied sc6vrr.ini [launch] to $bottle"
done

hud=$(sc6_ini_get launch MetalHUD 1)
for plist in "$HOME/Applications/CrossOver"/*.app/Contents/Info.plist(N); do
  [[ $(plutil -extract CXOriginalMenuName raw "$plist" 2>/dev/null) == SoulcaliburVI ]] || continue
  (( ${synced[(Ie)$(plutil -extract CXHelperAppBottleName raw "$plist" 2>/dev/null)]} )) || continue
  id=$(plutil -extract CFBundleIdentifier raw "$plist")
  if [[ $hud == 1 ]]; then
    defaults write "$id" MetalHUDForceEnabled -int 1
  else
    defaults delete "$id" MetalHUDForceEnabled 2>/dev/null || true
  fi
  print "MetalHUDForceEnabled=$hud for ${plist:h:h:t}"
done
print -rl -- "  "${^sc6_launch_env}
