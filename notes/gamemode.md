# Game Mode for SC6

## Recommended launcher (2026-10-17, later)

Run `tools/gamemode/build-ls-app.sh` once. Then double-click `~/Applications/SC6-VRR/SoulCalibur VI.app`.
macOS then turns on real Game Mode for SC6 by itself, with the menu bar indicator.
This final app keeps the presentation fixes, keeps stock frame timing, and writes no logs. The Metal HUD is off.
See [Final app bundle](#final-app-bundle-2026-10-17).
Quit FineTune and BetterDisplay before playing, or presentation is `Composited`.
See [Game Mode works through the bundle](#game-mode-works-through-the-bundle-2026-10-17).

## Earlier launcher (2026-10-17)

`dll/launch.sh` still works for the CrossOver Preview `SC6-Rosetta` bottle, but it only forces the global Game Mode switch.
It keeps stock frame timing. The frame-rate changes were reverted on 2026-10-17 (backup branch `backup/pre-framerate-revert-20261017`).
It uses the installed `crossover-gamemode` helper to manage Game Mode.
The helper restores the previous policy after every tracked game exits.

Wine can report a renamed temporary loader instead of a path inside CrossOver.
The live SC6 process reported a temporary `explorer.exe` whose bytes matched Preview's `wine` loader exactly.
The helper now verifies that match with SHA-256 before accepting the temporary path.
It retains the verified identity using the process ID and its start time.
Therefore, a changed native path does not cause an early release of the forced-on policy.
A reused process ID does not inherit another process's verification.

The source and installed helper are:

- `/Users/iphoobis/Projects/NotProton-gamemode/standalone/crossover-gamemode`
- `/Users/iphoobis/.local/bin/crossover-gamemode`

All 16 helper tests passed. The command reported a forced-on policy while SC6 ran.
After the test game closed, the policy returned to automatic.
These checks did not verify that Game Mode was active for SC6.
The user subsequently confirmed that Game Mode was NOT active.
The earlier claim that Game Mode was restored was incorrect. The activation fix remains unresolved.

The daemon logs reported global Game Mode as on, but identified SC6 as `anon<SoulcaliburVI.exe>` and `NotVisible`.
Those entries do not establish that SC6 received Game Mode.
The helper's `--status` output now states that its global result cannot verify activation for a particular game.
The evidence is in `tests/runs/gamemode-correction-20261017/gamepolicyd.log`.

Preview's native `wine` loader contains an embedded `__TEXT,__info_plist` section.
It names `com.codeweavers.CrossOver.wineloader` and sets `LSUIElement` to `1`.
Therefore, the older statement below about a loader without an embedded plist does not describe Preview's current loader.
Whether this identity prevents Game Mode activation remains unverified.
The earlier helper is saved in `~/Library/Application Support/CrossOverGameMode/backups/crossover-gamemode-before-sc6-path-fix-20261017`.

## Direct presentation (2026-10-17)

Native fullscreen puts the game window into a macOS fullscreen Space.
The display layer uses `BGR10A2`, which is the format that enabled direct presentation in the earlier tests.
The current layer dump confirms format `94`, which is `BGR10A2Unorm`, and native fullscreen state `5`.

CrossOver Preview's stock DXMT did not provide the required settings.
`tools/gamemode/install-direct-dxmt.sh` installs the existing custom x86-64 build into Preview's DXMT folders.
The Windows DLLs and the Unix library must stay together because they share a private interface.
The installer saves the originals before replacing them.
This changes DXMT for other x86-64 games in Preview. It does not change the ARM64EC libraries or regular CrossOver.

The original libraries are saved here:

```text
/Users/iphoobis/Library/Application Support/SC6-VRR/backups/preview-direct-dxmt-20261017-122618
```

The user confirmed `Direct`, with ultra-brief `Composited` flashes every 3-4 seconds.
The cause of those flashes remains unverified. This test does not establish uninterrupted direct presentation.
See [Composited flashes](#composited-flashes-2026-10-17) for the later investigation.
The launcher keeps MetalFX disabled and sets no DXMT frame limit.
It disables the pipeline archive because the live custom build could not serialize some geometry pipelines.
Shader capture remains disabled for this launcher.

`tests/check-direct-dxmt.sh` creates a real device and verifies the custom fullscreen call through both library halves.
It passed in the Rosetta bottle. Mapped-file checks confirmed that the game loaded the installed custom libraries.
The local evidence is in `tests/runs/direct-presentation-20261017/`.

The installed graphics build now reports `v0.80-283-g000e282`.
It includes current upstream `main`, the geometry-conversion change, and the requested changes from 204, 230, 228 and 222.
See [the integration report](dxmt-integration-2026-10-17.md) for validation, limits and the complete backup.
The direct presentation report above came from the preceding build.

The sections below preserve the earlier implementation and investigation.

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

## Composited flashes (2026-10-17)

The Metal HUD showed `Direct`, with a split-second `Composited` flash every 3–4 seconds.
The flashes appeared with both DXMT builds, so the build that adds 204, 230, 228 and 222 did not cause them.

Ruled out, one test each, with the user watching the HUD:

1. Window layers under the Metal layer. The window keeps AppKit's frame layer and a visual effect view under the game's opaque Metal layer. A temporary switch, `DXMT_HIDE_WINDOW_BACKDROP=1`, hid them once native fullscreen was active. The second layer dump confirmed that they were hidden. The flashes continued.
2. meDock. Its window sat at layer −1 on the game's fullscreen Space. The flashes continued after the user quit meDock.
3. Game stalls. `sc6vrr.log` showed 70–80 engine frames per second, and the few stalls did not repeat every 3–4 seconds.

Cause: BetterDisplay.
A window-list sampler (`CGWindowListCopyWindowInfo`, every 20 ms for 20 seconds) found that BetterDisplay creates a 1×1 window every 3.0 seconds.
The window sits at the highest window level, in the bottom-left corner, with alpha 0.05. It exists for about 80 ms.
This matches the user's report in period and in length.
A visible window above the game means WindowServer must blend it with the game, so it cannot show the Metal layer directly for those frames.

BetterDisplay's settings include `appleBuiltInDirectForceEDR@Display:2 = 1` and `appleBuiltInDirectMaxBrightness@Display:2 = 1600`.
These settings force EDR (extended dynamic range) on the built-in display for extra brightness.
That this feature creates the 1×1 window is an inference. The test below confirms only that BetterDisplay causes the flashes.

Confirmed: after the user quit BetterDisplay, the HUD stayed on `Direct` with no flashes.
To keep direct presentation, quit BetterDisplay before playing.
Turning off only its forced EDR for the built-in display may also work. That is untested.

The `DXMT_HIDE_WINDOW_BACKDROP` experiment did not help, so it was removed from the DXMT source and the launcher.

## Why the HUD says Game Mode is on without the menu bar icon (2026-10-17)

The user saw `Game Mode: On` in the Metal HUD, but no Game Mode icon in the menu bar.
A live `gamepolicyd` capture during an SC6 launch at 16:04 shows why. The evidence is in `tests/runs/gamemode-indicator-20261017/`.

1. The helper ran `gamepolicyctl game-mode set on`. `gamepolicyd` logged `Updating game mode enablement strategy to ConsoleModeEnablementStrategy(rawValue: 2)`, then `Game mode enabled.`
2. The assertion that it acquired targets `gamepolicyd` itself (`euid=0`), not the game. So this is the global switch, forced on.
3. RunningBoard (the macOS service that tracks app state) tracked the game process, PID 80259, as `anon<rpcss.exe>`. `anon` means that macOS has no app identity for the process. The name is the first Windows program that this Wine process ran.
4. RunningBoard reported the process only as `running-NotVisible`. It never reported it as active or visible, so `gamepolicyd` never saw a game in front.
5. `gamepolicyd` logged no gaming session for SC6.

The HUD probably shows the global status, which the forced switch turned on.
The menu bar icon probably appears only for a game session that `gamepolicyd` starts for a recognized foreground app.
Both points are inferences from these logs, not tested separately.
Which Game Mode benefits SC6 receives from the forced global switch alone is unverified.

## LaunchServices bundle test (2026-10-17)

Goal: give the SC6 process an app identity, so that `gamepolicyd` starts a Game Mode session by itself.
The builder is `tools/gamemode/build-ls-app.sh`. It makes `~/Applications/SC6-VRR/SoulCalibur VI.app`.

How it works:

1. CrossOver's `wine` script always runs `winewrapper.exe`, which starts the game as a child process. A child of a LaunchServices app does not get the app identity.
2. The loader can run a program directly, without `winewrapper.exe`. With CrossOver's environment, `wine cmd /c ver` printed the Windows version and exited with code 0.
3. The bundle's executable loads CrossOver's environment (captured at build time) and then execs the loader on `SoulcaliburVI.exe`. exec keeps the process that LaunchServices started.
4. The loader loads `ntdll.so` from its own folder. So the bundle holds a copy of the loader and a symlink to CrossOver's `ntdll.so`.

Results:

- `lsappinfo` listed the game process as `SoulCalibur VI`, `bundleID="local.sc6vrr.soulcalibur6"`, `type="Foreground"`. The executable path was inside the bundle.
- RunningBoard tracked it as `app<application.local.sc6vrr.soulcalibur6…>`, `running-active`, not `anon`.
- The game entered native fullscreen and stayed in front for about 75 seconds in two runs. The policy was automatic.
- Game Mode stayed off. The user saw no indicator. `gamepolicyd` and `GamePolicyAgent` logged no evaluation of the app.
- In the first run, the user switched away soon after launch, and the game then left native fullscreen. It became a 1728×1117 borderless window on the desktop Space, and the HUD showed `Composited`. Switching away did not cause this in the earlier runs, which used CrossOver's launcher. The cause is unknown.

Likely blocker: macOS does not mark the bundle as a game.
`GamePolicyAgent` reads the private resource key `_NSURLApplicationIsIdentifiedGameKey`. It is `1` for DuckStation, PCSX-Redux, ARMSX2, Astris and GeneralsZH Launcher in `/Applications`, and `0` for the SC6 bundle.
These changes did not set it: ad-hoc signing the bundle, a Mach-O main executable, a minimal Info.plist (only the category), and a copy in `/Applications` checked 3 seconds after registration.
A copy of GeneralsZH Launcher in the scratchpad lost the mark, so the mark depends on more than the bundle's contents. Spotlight did not find either bundle, so Spotlight probably does not decide it.
What sets the mark is unknown.

That mark turned out to be a result of detection, not its cause (see below).

## Game Mode works through the bundle (2026-10-17)

Status: verified. The user saw the Game Mode indicator and `Direct` presentation at the same time.
This section describes the test bundle. The final form is in [Final app bundle](#final-app-bundle-2026-10-17).

How macOS detects a game, from the `gamepolicyd` and `GamePolicyAgent` logs of a DuckStation session and the SC6 sessions:

1. `gamepolicyd` labels a process as a game itself. It logged `Found game GameProcess(..., labelReason=Plist category type)` for DuckStation, and `labelReason=LSSupportsGameMode (Info.plist)` for SC6.
2. Only after that, `GamePolicyAgent` sets the `_NSURLApplicationIsIdentifiedGameKey` capability on the app. Setting that capability by hand (through `NSURL setResourceValue`) did not start Game Mode.
3. `gamepolicyd` labeled SC6 only after the running process became the bundle's declared `CFBundleExecutable`. While the bundle's executable was a script that exec'd the loader, LaunchServices knew the app, but `gamepolicyd` never labeled it. The exact lookup is inferred, not traced.
4. With the label, `gamepolicyd` acquired `IdentifiedGameGrant`, then `FrontmostGrant` when the game came to the front, then `FullScreenGrant` and `ConsoleModeGrant` in native fullscreen, and logged `Game mode enabled.` The policy stayed `automatic`.

The bundle form that works:

- `CFBundleExecutable` is a copy of CrossOver Preview's x86-64 Wine loader. The loader runs `SoulcaliburVI.exe` in the same process, without `winewrapper.exe`.
- `Contents/MacOS/ntdll.so` is a symlink to CrossOver's `ntdll.so`, because the loader loads it from its own folder.
- `LSEnvironment` in Info.plist holds CrossOver's Wine environment and this project's settings. The game path is passed with `open --args`.
- The whole bundle is signed ad hoc, and the loader keeps its hardened runtime flag and entitlements. Without a bundle signature, LaunchServices refused the launch: `Launch requires secure launch with spawn constraints, but none are present or valid` (error -54).
- The builder refuses to run while SC6 runs. The launcher refuses to start if CrossOver's loader changed since the build.

Presentation: two other apps blocked `Direct` during these tests.

- With FineTune running (`com.finetuneapp.FineTune`, an audio app that captured audio, shown as `aud` in ControlCenter's sensor log), the HUD showed `Composited` for the whole session. No window overlapped the game. After the user quit FineTune, the HUD showed `Direct`. Why FineTune blocks direct presentation is unknown.
- BetterDisplay causes the 3-second `Composited` flashes (see above).
- meDock ran screen capture (`scr`) at times. Its effect on presentation was not tested separately.

Remaining limits:

- In the bundle runs, switching away from the game sometimes made it leave native fullscreen (DXMT state `5 -> 3`, `NotCoveringScreen`). The window then stays 1728×1084, DXMT does not request fullscreen again, and the game is composited until it restarts. This was not seen with CrossOver's launcher. The cause is unknown.
- The bundle runs only the game. Steam and other Windows programs still use CrossOver's launcher.
- Battle performance with this launch path is unmeasured.

## Final app bundle (2026-10-17)

`tools/gamemode/build-ls-app.sh` builds `~/Applications/SC6-VRR/SoulCalibur VI.app`. Double-click it to play.

Two bundles are needed, because a double-click passes no arguments:

1. The outer app is a launcher with no Dock icon (`LSUIElement`). It checks the SHA-256 hashes of CrossOver's Wine loader and Preview's seven x86-64 DXMT files against the values from build time. Then it opens the inner app with the game's path and exits. It shows an alert instead if the game already runs or a hash differs.
2. The inner app, `Contents/Helpers/SoulCalibur VI Game.app`, is the game bundle described above. Its executable is the Wine loader copy.

Settings in the inner app's `LSEnvironment`:

- Presentation: the private DXMT build, `dxgi.nativeFullscreen=True`, `dxgi.metalLayerPixelFormat=bgr10a2`, MetalFX off, no pipeline archive. These come from `launch-env.sh`.
- Frame timing: stock. `WINEDLLOVERRIDES` includes `dinput8=b`, so Wine uses its built-in `dinput8` and the SC6-VRR DLL in the game folder does not load. That DLL always writes `sc6vrr.log`, and it does nothing else with frame changes and capture off. The DLL file stays installed for test launches.
- Logging: `WINEDEBUG=-all`, `DXMT_LOG_LEVEL=none`, `DXMT_LOG_PATH=none`, `DXMT_DUMP_LAYERS=0`, `MTL_HUD_ENABLED=0`. LaunchServices sends the app's output to nowhere.

Checked on 2026-10-17 with `open` on the outer app:

- `gamepolicyd` logged `Found game` and then `Game mode enabled.` with the policy on automatic.
- `lsof` showed Wine's built-in `dinput8.dll` and Preview's DXMT files in the game process. The game folder's `dinput8.dll` was not open.
- No file changed in the game folder, and no `.log` file changed in the bottle, during the first 30 seconds.

Direct presentation was not rechecked with this build, because the HUD is off. Its presentation settings match the run in which the user saw `Direct`.
Quit FineTune and BetterDisplay before playing.

## Starting the game from the SC6-FEX app (2026-11-01)

The normal way to start the game is `~/Applications/CrossOver/SoulcaliburVI (Game Mode) (SC6-FEX).app`. No shell script is needed for that:

- The SC6-FEX bottle links `C:\Games\SoulCalibur 6` to the SC6-Rosetta game folder. So the app loads the same `dinput8.dll` and reads the same `sc6vrr.ini` as every other launch.
- The bottle loads the native `dinput8.dll` for `SoulcaliburVI.exe` (per-app DLL override in `user.reg`).
- The LaunchAgent `local.sc6vrr.apply-ini` copies the `[launch]` section of `sc6vrr.ini` into the bottle's `cxbottle.conf` whenever the ini changes.
- The LaunchAgent `local.sc6vrr.install-dll` runs `dll/sync-dll.sh` whenever `dll/build/dinput8.dll` changes and at login. The script installs the new build into the game folder; if the game is running, it waits until the game exits. It keeps the old DLL as `dinput8.dll.previous` and never replaces a `dinput8.dll` that is not the sc6vrr proxy. Log: `~/Library/Logs/sc6vrr-install-dll.log`.
