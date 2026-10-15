# Game Mode for SC6

Goal: macOS turns on Game Mode while SC6 runs fullscreen. Game Mode gives the game priority for CPU and GPU time, lowers background work, and raises the Bluetooth sample rate for controllers.

## Why SC6 does not get Game Mode today (2026-10-15)

1. macOS (`gamepolicyd`) looks up the bundle of the fullscreen app's process (`bundleRecordForAuditToken`). A bundle counts as a game through `LSApplicationCategoryType` (a game category) and the opt-in keys `GCSupportsGameMode` / `LSSupportsGameMode`.
2. CrossOver 26.3 runs every Windows program with `CrossOver.app/Contents/SharedSupport/CrossOver/CrossOver-Hosted Application/wineloader`. That folder is not an app bundle, and the loader has no embedded `__TEXT,__info_plist`.
3. `CrossOver.app`'s own `Info.plist` has no app category and no Game Mode key. CrossOver's generated launchers (`~/Applications/CrossOver/Steam/*.app`) have none either.
4. So macOS sees no game.

The Mac driver (`winemac.so`) has no Game Mode or native-fullscreen option. `gamepolicyd` does contain `NonAppBundleGames` and `ScanForUnregisteredGames`, but how those work is not known.

## What we do: a game bundle

`tools/gamemode/build-app.sh` builds `~/Applications/SC6-VRR/SoulCalibur VI.app`:

| Part | Purpose |
| --- | --- |
| `Contents/MacOS/wineloader` | Copy of CrossOver's loader. The SC6 process runs this file, so its bundle is this app. |
| `Contents/MacOS/sc6-launch` | Bundle executable. Runs CrossOver's `wine --bottle Steam --env "WINELOADER=… CX_WINELOADER=…" --start SoulcaliburVI.exe`. |
| `Contents/Info.plist` | `LSApplicationCategoryType = public.app-category.games`, `GCSupportsGameMode = true`, `LSSupportsGameMode = true` |
| Signature | Ad hoc, without the hardened runtime, so the copy can load CrossOver's libraries and map executable memory. |
| Launch Services | Registered with `lsregister -f`. |

The copied loader works for these reasons:

1. The `wine` script adds `$CX_ROOT/lib/wine` to `WINEDLLPATH`, so a loader outside `CrossOver.app` finds `ntdll.so`. CrossOver's own "dock icon renaming hack" (bug 22144) relies on this.
2. `--env` is applied after the script sets `WINELOADER` and `CX_WINELOADER` (`bin/wine` lines 713–722).
3. Wine starts new processes, the game among them, with `$WINELOADER`.

The launcher refuses to start if CrossOver's `wineloader` changed since the build, because the copy must match the rest of CrossOver's Wine build. Rerun the build script after each CrossOver update.

## Verified

- The bundle's `Info.plist` lints and holds the keys above. `codesign --verify --deep --strict` passes.
- Smoke test: `wine --bottle Steam --env "WINELOADER=<bundle loader> …" cmd /c ver` ran from the bundle's `wineloader` (seen with `ps`). It printed `Microsoft Windows 10.0.22000` and exited with code 0.

## Not verified yet (needs a game run)

1. Whether `gamepolicyd` treats the SC6 process as this bundle and starts Game Mode. Watch with:
   `log stream --predicate 'process == "gamepolicyd"' --info`
   and look for the Game Mode icon in the menu bar.
2. Whether SC6's borderless or exclusive fullscreen counts as "fullscreen" for Game Mode. Wine's Mac driver does not use a native macOS fullscreen Space.
3. Whether the bundle's `NSHighResolutionCapable = true` matches the Retina setting that we need for MetalFX (`RetinaMode = n` for SoulcaliburVI.exe).

Undo: delete `~/Applications/SC6-VRR/SoulCalibur VI.app`. Nothing in CrossOver or the bottle changes.

## Results (2026-10-15)

- **The bundle alone does not give Game Mode.** In test 01, `gamepolicyd` saw the SC6 window as a foreground app with no `CFBundleIdentifier`. So macOS never reads the bundle's game keys for the game process. The cause is unknown; Wine's loader may register the process with Launch Services without a bundle.
- **`gamepolicyctl` works.** Xcode's `gamepolicyctl game-mode set on` (also used by NotProton PR #156) turned Game Mode on in test 04, and the Metal HUD showed Game Mode on. It needs no admin rights. `game-mode set auto` restores the normal policy.
- **The launcher now does this.** `sc6-launch` forces Game Mode on, waits for `SoulcaliburVI.exe` to exit, then sets the policy back to automatic. Without Xcode it skips this step.
- **Presentation stays "Composited"** (Metal HUD) even with Game Mode on. Direct presentation probably needs native fullscreen. That is the deferred item above.
