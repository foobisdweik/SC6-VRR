# Identification of the busy thread, 2026-10-16

The reproduced thread using nearly one full CPU core is SC6's **`RenderThread 2`**. The thread prepares the game's rendering work. DXMT's named encoding thread is separate and used about 41% CPU.

The result identifies the thread's role. It does not identify the expensive function or establish whether all of its CPU time performs useful work.

## Direct identification

Run: `tests/runs/20261016-152131-19-thread-names`.

The game ran through DXMT with Geralt and Groh idle on Kaer Morhen. There was no stack sampling in this run. The recorder verified the game process and a valid foreground interval. The analysis uses the final 35 seconds of the 90-second recording.

| Role | Windows thread ID | OS counter handle | CPU use | Measured time |
| --- | ---: | --- | ---: | ---: |
| `RenderThread 2` | 688 | `0x7ff2067740e0` | 99.286% | 34.123317 s |
| Game thread | 340 | `0x7ff2061fa0e0` | 26.749% | 34.123319 s |
| `dxmt-encode-thr` | Not queried | `0x7ff2067660e0` | 41.222% | 34.123318 s |

100% CPU is one core's execution capacity over the measured interval. It does not mean the thread stayed on one physical core.

The native game process ID was `21249`. Its Windows process ID was `336`. These IDs belong to different systems and must not be substituted for each other.

The identification follows this chain:

1. The game reported `RenderThread 2` for Windows thread `688` through its legacy naming exception.
2. A read-only Windows helper queried thread `688` and obtained its Windows thread record.
3. The helper read that record without suspending the thread.
4. The record's native thread-data field at offset `0x320` contained `0x7ff2067740e0`.
5. That value exactly matched the handle used by the OS CPU counters for the 99.286% thread.
6. An independent check of game thread `340` matched a different handle and its 26.749% CPU measurement.

A Thread Environment Block (TEB) is Windows' record for one thread. The helper obtained its address through `NtQueryInformationThread`. The record's self-pointer, process ID, and thread ID were checked before reading the native field.

Wine 11.0 identifies the field at `TEB + 0x320` as `pthread_teb`. This private layout is specific to the relevant architecture and Wine implementation. The live field values were matched to the actual counters rather than treated as portable constants. [Wine's source for the field and its offset](https://github.com/wine-mirror/wine/blob/wine-11.0/dlls/ntdll/unix/signal_x86_64.c#L463-L478)

This is a direct match through thread records. It does not depend on interpreting incomplete call stacks or assuming that two similar CPU percentages identify the same thread.

## Why the name was missing

SC6's naming function at `0x140e1f3d0` raises exception `0x406D1388` with the requested name and Windows thread ID. The call at `0x140e1f413` supplies three parameters in the 64-bit layout. The supported executable's bytes were inspected directly. No Ghidra project was changed.

This older naming mechanism communicates with debuggers. Its names do not automatically appear in performance tools. [Microsoft's explanation of legacy thread naming](https://learn.microsoft.com/en-us/visualstudio/debugger/tips-for-debugging-threads)

The diagnostic DLL added an optional exception observer. It records the game's requested names and then returns `EXCEPTION_CONTINUE_SEARCH`. Therefore, the game's original exception handling still runs. The observer attempts `SetThreadDescription` only when the name targets the current thread.

SC6 requested these render-thread names during startup:

- Windows thread `620`: `RenderThread 0`.
- Windows thread `644`: `RenderThread 1`.
- Windows thread `688`: `RenderThread 2`.

The creator, Windows thread `340`, emitted these requests. Because they targeted other threads, the observer did not rename the current thread. Its `hr=0x80004001` records that it did not attempt that propagation. It is not evidence that a native naming API call failed.

Thread `620` had already exited when queried. The active thread was `688`, so the final proof uses its record. The two earlier render threads must not be confused with the thread used during Training.

## First capture and its limits

Run: `tests/runs/20261016-151102-18-thread-identity`.

The first diagnostic run recorded OS thread IDs with CPU counters. Before profiling, thread `143636` used 99.411% CPU over 9.345343 seconds. The named DXMT encoding thread used 40.935%. One eight-second Time Profiler capture recorded the selected thread.

The trace contains incomplete Windows stacks under Rosetta. Some reconstructed frames contain invalid return addresses or point into padding between functions. Some clock calls resolve, but that does not prove a busy-wait mechanism. Do not use those stacks to assign precise function costs.

OS thread IDs were obtained through `PROC_PIDLISTTHREADIDS`, then queried with `PROC_PIDTHREADID64INFO`. The mapping was tested against Python's own OS thread ID and thread CPU clock. The existing collector's handle-based format remains unchanged. [Apple's thread-list and lookup implementation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/proc_info.c)

The first run's thread ID belongs to that exited process. The second run reproduced the near-one-core workload and identified its role directly. This does not retroactively attach a new process's IDs to earlier captures.

## Other attempted measurement

`GetThreadTimes` returned zero CPU time for the active Windows render thread on this CrossOver installation. The OS counters independently showed substantial CPU use. Therefore, the zero values were rejected as an identification method. They do not show that the render thread was idle.

The final result uses the read-only thread-record match instead.

## Installed diagnostic and verification

`SC6VRR_THREAD_NAMES=1` enables the observer. It is disabled by default. Case `19-thread-names` enables it. Case `18-thread-identity` uses the separate profiler capture. Neither diagnostic run is used to compare frame rates.

The installed `dinput8.dll` has SHA-256 `3f221a93f84ac6435236b12de4e18c5b013aa81b4e2794e912624a8e230cf31e`.

The previous measurement DLL is backed up at:

```text
/Users/iphoobis/Library/Application Support/SC6-VRR/backups/thread-names-dll-20261016-152115/dinput8.dll
```

Validation passed:

- All 27 Python tests passed, including the OS thread-ID and CPU-clock checks.
- The DLL built successfully.
- The Wine test confirmed name propagation for the current thread and rejection of invalid naming records.
- The Wine test confirmed that the observer preserves original exception handling.
- Existing clock, register, counter, and frame-timer tests passed.
- The installed DLL passed the executable and installation checks.
- Both thread records passed the self-pointer and Windows ID checks before matching their OS counter handles.

No DXMT source was changed. No optimization was applied. The next investigation should measure the render thread's work through a method that handles Windows frames reliably.

## Artifacts

Case 19 preserves:

- `thread-role-proof.json`: the final names, thread records, exact handle matches, and CPU measurements.
- `sc6vrr.log`: the game's naming messages and completed-work counters.
- `teb-688.json` and `teb-688.bin`: the render thread's record.
- `teb-340.json` and `teb-340.bin`: the independent game-thread check.
- `native-threads.tsv` and `threads.tsv`: the two OS counter views.
- `thread-names-result.json`: the ranking by OS thread ID.

Case 18 preserves the native trace, selected data-table exports, and `target-stacks.json`. The latter records unresolved or invalid stacks and must not be treated as a reliable function-cost profile.

Game Mode is off, and its policy is automatic. The game and remaining bottle services have exited.

## Profiler output handling

An early profiler preflight printed environment values and exposed a Hugging Face token in tool output. The user was notified and advised to rotate it. The scratch trace was removed. No token value was written into project source or this report.

Subsequent launches use an environment allowlist. Credential-named variables are excluded. Only selected data tables are exported for inspection. The live trace's index was checked without printing its values. It contained zero credential-named environment entries. The saved `trace-index-redacted.xml` omits environment values.

Do not print an unfiltered `xctrace export --toc` result. Its environment section can contain credentials.
