# Measuring battle progress and performance

Step 1 now has corrected summaries and an optional measurement mode in the DLL. The first live Training run completed on 2026-10-15. Its results are recorded in [test results](test-results.md#test-10-measurement-without-sample).

The measurement DLL was installed on 2026-10-15 at 17:50 PDT. The previous DLL is saved at `/Users/iphoobis/Library/Application Support/SC6-VRR/backups/measurement-dll-20261015-175027/dinput8.dll`. To restore it, quit the game and copy that file over the installed `dinput8.dll`.

An extended build was installed at 18:34 PDT for the [prepared frame tests](frame-tests.md). Its previous build is saved at `/Users/iphoobis/Library/Application Support/SC6-VRR/backups/frame-timers-dll-20261015-183408/dinput8.dll`. The new frame timers require `SC6VRR_FRAME_TIMERS=1`. Cases 11–13 are prepared, but no new game run has started.

## What changed

The collector records each process separately. It verifies the game's native Wine path and the program at the start of its command. Therefore, a launcher that merely names the game in its arguments does not become the game process.

The live run showed that Wine later reports a temporary executable path. The collector now retains verification across that change while the command and CPU time remain continuous. The summary applies the same rule to the original records. It preserves those records without rewriting them.

CPU time is the total time that a process uses across CPU cores. The new summary divides the increase in CPU time by elapsed time. For example, 2 CPU seconds during 1 elapsed second means 200% CPU. Resident memory is the process memory currently held in RAM. The summary records its largest observed value.

Old runs did not record CPU time or executable paths. Their summaries therefore show each process separately and label its identity as unverified. They do not infer which process was the game from CPU use.

The Metal HUD (heads-up display) reports a frame counter. The summary calculates fps from the total accepted frame count divided by total accepted time. It keeps processes separate and rejects these intervals:

- An interval starts from a zero counter.
- The counter decreases.
- The timestamps do not increase.
- The calculated rate exceeds the declared ceiling of 240 fps.

The ceiling filters implausible data. It does not prove that a remaining interval is correct. Each summary prints the rejected counts. New runs include only verified game processes in the HUD summary. A HUD counter does not identify the scene or distinguish multiple swapchains within one process. A swapchain holds the images that an application presents to the display.

## What the DLL records

Set `SC6VRR_MEASURE=1` to enable measurement. The variable is off by default. Case `10-measure-battle` enables it.

The DLL records these values approximately once per second:

| Field | Meaning |
| --- | --- |
| `elapsed_s` | The actual elapsed time covered by the record. Rates use this value rather than assuming one second. |
| `engine_frames` | The number of intervals between engine frame boundaries. |
| `battle_completed` | The observed increase in the game's counter at `0x14470d0c4`. The battle function increments it near its return. |
| `battle_counter` | The counter's current raw value. |
| `counter_resets` | The number of observed jumps above 600 steps between engine frames. These jumps are excluded from `battle_completed`. |
| `clock_r1`, `clock_r2` | The number of executed master-clock increments at the two patched sites. Skipped increments do not count. |
| `world_calls` | The number of completed `UWorld::Tick` calls on the engine thread. |
| `world_total_ms`, `world_max_ms` | The total and largest elapsed duration of those calls. |
| `engine_time_total_ms`, `engine_time_max_ms` | The total and largest duration of `UpdateTimeAndHandleMaxTickRate`. This function can wait to enforce a frame limit. |
| `unix_s` | The UTC timestamp at the end of the record, expressed as seconds since the Unix epoch. |

These timers measure elapsed time, including waits. They do not measure CPU execution time. They cannot identify which other thread caused a wait.

The DLL reads the counter at frame boundaries. A reset and later advances within the same interval can be ambiguous. The raw counter and discontinuity count make this limit visible. A discontinuity means that the reported battle rate is incomplete.

The instrumentation checks the executable hash, the world function's first bytes, and the counter's increment instruction before enabling measurement. It retains the existing battle gates. It does not fix scheduling below 60 fps or add the planned online protection.

The clock instrumentation preserves the original flags and registers. Its counts use an atomic increment, so concurrent calls do not lose counts. It writes one aggregate record per reporting window rather than writing one line per frame. Its overhead in the actual game remains unmeasured.

## Run a measurement

The harness needs the newly built DLL beside the game executable. It records that DLL's hash in `meta.txt`.

1. Quit CrossOver programs before starting the harness.
2. Run `tests/run.sh 10-measure-battle` from the project directory.
3. Enter Training and wait until loading finishes.
4. Run `tests/mark.sh latest battle-start` in another terminal.
5. Remain in the same Training scene for at least 30 seconds.
6. Perform the same short sequence of movement and attacks for each comparison.
7. Run `tests/mark.sh latest battle-end` before leaving Training.
8. Quit the game so that the harness writes `summary.txt`.

The summary shows the whole run and the marked battle interval separately. It includes only complete measurement intervals within the markers. Without a complete pair of markers, it explicitly says that battle performance was not isolated.

Return to the game immediately after the start marker, and keep it in the foreground. The first live run used the foreground transition in `gamepolicyd.log` to exclude the initial chat interaction. Explicit UTC boundaries can reproduce that selection with `--from-unix` and `--to-unix` on `tests/measurements.py summary`.

Case 10 enables `DXMT_LOG_FRAME_STATS=1` and disables macOS `sample`. DXMT's frame-statistics summary covers the whole run because its log records have no timestamps. Do not use case 09 to compare absolute performance because its sampling repeatedly pauses the game. The harness still clears caches before each run. Tests of archive reuse require the later cache-control change.

## Verify the measurement tools

Run `tests/check-measurements.sh --wine` to build the DLL and execute the tests. The Wine test runs a small test executable. It does not load the game or call the DLL's setup function.

The checks cover these cases:

- Separate CPU data for a launcher and a game process.
- Rates derived from CPU time and elapsed time.
- HUD counter restarts, jumps, process changes, and midnight.
- Whole intervals selected by battle markers.
- Completed-work records kept separate from legacy scheduler decisions.
- Counter wraparound and discontinuities.
- Actual generated x64 code for executed and skipped clock increments.
- Preservation of flags, registers, and the arguments passed to `UWorld::Tick`.

A separate collector check ran a harmless `cmd` process under Wine. It verified the native Wine path and kept the launcher separate. No game performance claim follows from that check.

## Corrections to older results

Test 08's old CPU summary combined two processes into approximately 97% CPU. Their separate averages were 195.1% and 0.3%. These are averages of stored `ps` observations, not the new measurement of CPU time.

Test 09's old HUD summary reported a maximum of 4,754.5 fps and an average of 131.5 fps. Its counter changed from 0 to 4,455 in 0.937 seconds. The corrected parser rejects that interval. It produces 19.6 fps across accepted whole-run intervals, with a maximum of 120.1 fps. That whole-run value includes loading and the pauses caused by sampling. It does not measure normal Training performance.

The earlier `steps=` values count scheduling decisions. They do not prove that the battle function completed that many steps. The new `battle_completed` and clock counts provide independent observations for the next game run.
