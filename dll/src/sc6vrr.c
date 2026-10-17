/*
 * sc6vrr: SoulCalibur VI frame-rate unlock with a fixed 60 Hz battle simulation.
 *
 * Loaded as a proxy dinput8.dll next to SoulcaliburVI.exe. It forwards the
 * real dinput8 exports and installs these hooks (all addresses are for the
 * Steam exe with SHA-256 f8904e4b...a553; see notes/frame-cap.md):
 *
 *  - UEngine::GetMaxTickRate (0x142177dc0): with SC6VRR_UNCAP=1, clears
 *    UEngine::bUseFixedFrameRate (bit 0x40 at +0x648; DefaultEngine.ini sets it,
 *    which caps the game at 60 fps and forces a 1/60 s engine delta) and returns
 *    SC6VRR_MAX_FPS as the frame limit.
 *  - UEngine::UpdateTimeAndHandleMaxTickRate (0x142189040): runs once at the
 *    start of every engine frame on the game thread. With SC6VRR_GATE=1 it adds
 *    the real elapsed time to an accumulator and decides whether this frame runs
 *    one 60 Hz battle step ("step frame") or none.
 *  - The battle gates from the SC6 modding docs (replay-system.md, HorseMod gate
 *    stack): on frames without a step, the simulation core, six actor ticks and
 *    the time-dilation scalar return at once, and the two master-clock
 *    increments are skipped. The battle therefore advances 60 times per second
 *    while the engine renders faster.
 *
 * Settings come from environment variables (set them with `wine --env`):
 *   SC6VRR_UNCAP=1, SC6VRR_MAX_FPS=120, SC6VRR_GATE=1, SC6VRR_LOG=<Windows path>
 *   SC6VRR_MEASURE=1 records completed battle work and elapsed function times.
 *   SC6VRR_SHADER_DIR=<Windows path> records every shader the game creates,
 *   its input layouts, and the shader combinations that its draws use. With
 *   it, F10 in the game also logs one frame's calls to <dir>\frames\.
 *   SC6VRR_SKIP_GS_DRAWS=1 drops geometry-shader draws, for GPU profiling only.
 *   SC6VRR_FRAME_MODS=1 is required for every engine hook above (uncap, gate,
 *   measure). Without it, the DLL changes no frame timing.
 */
#include <windows.h>
#include <bcrypt.h>
#include <d3d11.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include "MinHook.h"
#include "measurements.h"

/* ---------- configuration and logging ---------------------------------- */

static int cfg_uncap, cfg_gate, cfg_measure, cfg_frame_timers, cfg_thread_names;
static float cfg_max_fps = 120.0f;
static FILE *log_file;

static int env_int(const char *name, int fallback) {
  char buf[32];
  DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
  return (n && n < sizeof(buf)) ? atoi(buf) : fallback;
}

static void log_msg(const char *fmt, ...) {
  if (!log_file)
    return;
  _lock_file(log_file);
  SYSTEMTIME t;
  GetLocalTime(&t);
  fprintf(log_file, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(log_file, fmt, ap);
  va_end(ap);
  fputc('\n', log_file);
  fflush(log_file);
  _unlock_file(log_file);
}

static void open_log(void) {
  char path[MAX_PATH];
  DWORD n = GetEnvironmentVariableA("SC6VRR_LOG", path, sizeof(path));
  if (!n || n >= sizeof(path)) {
    /* Default: sc6vrr.log next to the exe. */
    n = GetModuleFileNameA(NULL, path, sizeof(path));
    char *slash = strrchr(path, '\\');
    if (!slash)
      return;
    strcpy(slash + 1, "sc6vrr.log");
  }
  log_file = fopen(path, "w");
}

/* Observe the game's legacy thread names without consuming its exception. */
typedef HRESULT (WINAPI *set_thread_description_fn)(HANDLE, PCWSTR);
static set_thread_description_fn set_thread_description;
static PVOID thread_name_observer;

static LONG CALLBACK observe_thread_name(EXCEPTION_POINTERS *exception) {
  EXCEPTION_RECORD *record = exception->ExceptionRecord;
  if (record->ExceptionCode != 0x406D1388 || record->NumberParameters != 3 ||
      record->ExceptionInformation[0] != 0x1000 ||
      (record->ExceptionInformation[2] >> 32) != 0)
    return EXCEPTION_CONTINUE_SEARCH;
  char name[64] = {0};
  SIZE_T got = 0;
  ReadProcessMemory(GetCurrentProcess(), (const void *)record->ExceptionInformation[1],
                    name, sizeof(name) - 1, &got);
  if (!got)
    return EXCEPTION_CONTINUE_SEARCH;
  name[got] = 0;
  for (unsigned i = 0; name[i]; i++)
    if ((unsigned char)name[i] < 32 || (unsigned char)name[i] > 126)
      name[i] = '_';
  DWORD caller = GetCurrentThreadId();
  DWORD target = (DWORD)record->ExceptionInformation[2];
  if (target == (DWORD)-1)
    target = caller;
  HRESULT result = E_NOTIMPL;
  if (target == caller && set_thread_description) {
    wchar_t wide[64];
    for (unsigned i = 0; i <= strlen(name); i++)
      wide[i] = (unsigned char)name[i];
    result = set_thread_description(GetCurrentThread(), wide);
  }
  log_msg("thread_name_v=1 win_tid=%lu caller_tid=%lu hr=0x%08lx name=%s",
          (unsigned long)target, (unsigned long)caller, (unsigned long)result, name);
  return EXCEPTION_CONTINUE_SEARCH;
}

static void install_thread_name_observer(void) {
  set_thread_description = (set_thread_description_fn)GetProcAddress(
      GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription");
  thread_name_observer = AddVectoredExceptionHandler(1, observe_thread_name);
  log_msg("thread names: observer=%s native_naming=%s; original exception handling is preserved",
          thread_name_observer ? "enabled" : "failed", set_thread_description ? "available" : "unavailable");
}

/* ---------- exe identity ------------------------------------------------- */

static const char kExpectedSha256[] = "f8904e4b04bca3b47bc52a683f6190365d2eb89ee8f44f8072759e9c5e04a553";

static int exe_hash_matches(void) {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(NULL, path, MAX_PATH);
  HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
  if (f == INVALID_HANDLE_VALUE)
    return 0;
  BCRYPT_ALG_HANDLE alg = NULL;
  BCRYPT_HASH_HANDLE hash = NULL;
  UCHAR digest[32];
  int ok = 0;
  static UCHAR buf[1 << 20];
  DWORD got;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 &&
      BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) == 0) {
    while (ReadFile(f, buf, sizeof(buf), &got, NULL) && got)
      BCryptHashData(hash, buf, got, 0);
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
      char hex[65];
      for (int i = 0; i < 32; i++)
        sprintf(hex + 2 * i, "%02x", digest[i]);
      ok = strcmp(hex, kExpectedSha256) == 0;
      log_msg("exe sha256 %s (%s)", hex, ok ? "expected build" : "UNKNOWN BUILD");
    }
  }
  if (hash) BCryptDestroyHash(hash);
  if (alg) BCryptCloseAlgorithmProvider(alg, 0);
  CloseHandle(f);
  return ok;
}

/* ---------- addresses (absolute, image base 0x140000000) ----------------- */

#define IMAGE_BASE 0x140000000ull
#define ADDR(a) ((void *)((uintptr_t)(a) - IMAGE_BASE + (uintptr_t)GetModuleHandleW(NULL)))

typedef struct {
  const char *name;
  uint64_t addr;
  const char *prologue_hex; /* first bytes, checked before hooking */
  int returns_float;        /* gate stub returns 0.0f in xmm0 */
} gate_site;

/* Battle gates (SC6 docs: replay-system.md "Site 9 plus seven Actor::Tick gates"). */
static const gate_site kGates[] = {
  {"Site9 LuxBattle_PerFrameTick",          0x1402DBC60, "4c8bdc49895b1049896b1856", 0},
  {"Site11 Chara_AdvanceReplayFrame",       0x1403F8410, "40534883ec3083b900440000", 0},
  {"Site20 FrameInputLog_TickActor",        0x1403FBDF0, "40534883ec20488bd9e82265", 0},
  {"Site21 BM_MainStateMachine",            0x1403FBF30, "40534883ec30488bd90f2974", 0},
  {"Site21b BM_Update_Impl",                0x140437590, "488bc4554156488d68a14881", 0},
  {"Site22 Chara_TickActor",                0x1403D0590, "4055535641544155488dac24", 0},
  {"Site22b DemoHuman_TickActor",           0x1404865B0, "488bc45556488d68a14881ec", 0},
  {"MoveVM_GetTimeDilationScalar",          0x14030A8C0, "0fb6913c0200004c8d0d02ba", 1},
};

/* Master-clock increments: `inc dword [rbx+0x3A4]` (FF 83 A4 03 00 00). */
static const uint64_t kClockIncs[] = {0x1403E1FEA /* R1+0x2A */, 0x1403E2033 /* R2+0x33 */};

#define ADDR_GET_MAX_TICK_RATE 0x142177dc0ull
#define ADDR_UPDATE_TIME       0x142189040ull
#define ADDR_GENGINE           0x1443b3068ull /* UEngine* GEngine */
#define ADDR_TMAXFPS_VALUE     0x1443b3578ull /* float* to t.MaxFPS game-thread value */
#define ADDR_WORLD_TICK        0x141f02230ull
#define ADDR_BATTLE_COUNTER    0x14470d0c4ull
#define ADDR_BATTLE_COUNTER_INC 0x1402dc34cull
#define ADDR_FRAME_LOOP        0x140396450ull
#define ADDR_GAME_TICK         0x141e38f70ull
#define ADDR_FRAME_SYNC        0x1421876d0ull
#define ADDR_FENCE_WAIT        0x1415efec0ull

static int bytes_match(const void *p, const char *hex) {
  const uint8_t *b = p;
  for (size_t i = 0; hex[2 * i]; i++) {
    unsigned v;
    sscanf(hex + 2 * i, "%2x", &v);
    if (b[i] != v)
      return 0;
  }
  return 1;
}

/* ---------- executable memory near the exe (for rel32 calls) ------------- */

static uint8_t *alloc_near(void *target, size_t size) {
  uintptr_t t = (uintptr_t)target & ~(uintptr_t)0xFFFF;
  for (uintptr_t off = 0x10000; off < 0x7FF00000; off += 0x10000) {
    void *p = VirtualAlloc((void *)(t - off), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (p)
      return p;
  }
  return NULL;
}

/* ---------- gating ------------------------------------------------------- */

/* 1 = this engine frame has no battle step: gated paths return at once.
 * Read by generated machine code, so it is a plain byte. */
static volatile uint8_t g_skip_battle = 0;
static volatile uint64_t g_clock_advances[2];

/* These measurements run on the engine thread. Read the battle counter at
 * frame boundaries instead of adding another detour to the simulation core. */
static struct {
  double last_frame, window_start;
  DWORD engine_thread;
  uint32_t last_battle;
  uint64_t last_clock[2], engine_frames, battle_completed, counter_resets;
  uint64_t clock[2];
  sc6_duration world, engine_time;
} measurements;

static double qpc_seconds(void) {
  static LARGE_INTEGER freq;
  LARGE_INTEGER now;
  if (!freq.QuadPart)
    QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&now);
  return (double)now.QuadPart / (double)freq.QuadPart;
}

/* Entry stub: mov rax,&g_skip_battle; cmp byte [rax],0; jne skip;
 * mov rax,<trampoline>; jmp rax; skip: [xorps xmm0,xmm0;] ret.
 * Only rax (volatile, never an argument register) is touched, so the stub
 * works for any signature. */
static uint8_t *make_gate_stub(uint8_t *mem, int returns_float, void ***tramp_slot) {
  uint8_t *p = mem;
  *p++ = 0x48; *p++ = 0xB8; *(uint64_t *)p = (uint64_t)(uintptr_t)&g_skip_battle; p += 8;
  *p++ = 0x80; *p++ = 0x38; *p++ = 0x00;
  *p++ = 0x75; *p++ = 0x0C;
  *p++ = 0x48; *p++ = 0xB8; *tramp_slot = (void **)p; *(uint64_t *)p = 0; p += 8;
  *p++ = 0xFF; *p++ = 0xE0;
  if (returns_float) { *p++ = 0x0F; *p++ = 0x57; *p++ = 0xC0; }
  *p++ = 0xC3;
  return p;
}

/* The clock cave runs in place of the original INC. Restore incoming flags
 * before INC, because INC preserves the carry flag. Restore them on skips too. */
static uint8_t *make_clock_cave(uint8_t *mem, unsigned clock_index) {
  uint8_t *p = mem;
  *p++ = 0x9C; /* pushfq */
  *p++ = 0x50;
  *p++ = 0x48; *p++ = 0xB8; *(uint64_t *)p = (uint64_t)(uintptr_t)&g_skip_battle; p += 8;
  *p++ = 0x80; *p++ = 0x38; *p++ = 0x00;
  *p++ = 0x58;
  *p++ = 0x75;
  uint8_t *skip_offset = p++;
  *p++ = 0x9D; /* popfq: restore the original carry flag before INC */
  *p++ = 0xFF; *p++ = 0x83; *p++ = 0xA4; *p++ = 0x03; *p++ = 0x00; *p++ = 0x00;
  if (cfg_measure) {
    /* Count only an executed INC. Preserve its flags and all registers.
     * lock inc keeps counts valid if a second thread reaches a clock site. */
    *p++ = 0x9C; /* pushfq */
    *p++ = 0x50; /* push rax */
    *p++ = 0x48; *p++ = 0xB8;
    *(uint64_t *)p = (uint64_t)(uintptr_t)&g_clock_advances[clock_index]; p += 8;
    *p++ = 0xF0; *p++ = 0x48; *p++ = 0xFF; *p++ = 0x00; /* lock inc qword [rax] */
    *p++ = 0x58; /* pop rax */
    *p++ = 0x9D; /* popfq */
  }
  *p++ = 0xC3;
  *skip_offset = (uint8_t)(p - skip_offset - 1);
  *p++ = 0x9D; /* skipped INC: restore the original flags */
  *p++ = 0xC3;
  return p;
}

static int install_gates(void) {
  uint8_t *mem = alloc_near(ADDR(0x1402DBC60), 0x1000);
  if (!mem) {
    log_msg("gates: no executable memory near the exe");
    return 0;
  }
  uint8_t *p = mem;
  size_t gate_count = cfg_gate ? sizeof(kGates) / sizeof(kGates[0]) : 0;
  for (size_t i = 0; i < gate_count; i++) {
    void *target = ADDR(kGates[i].addr);
    if (!bytes_match(target, kGates[i].prologue_hex)) {
      log_msg("gates: prologue mismatch at %s; not hooking anything", kGates[i].name);
      return 0;
    }
  }
  for (size_t i = 0; i < sizeof(kClockIncs) / sizeof(kClockIncs[0]); i++) {
    if (!bytes_match(ADDR(kClockIncs[i]), "ff83a4030000")) {
      log_msg("gates: clock INC mismatch at 0x%llx; not hooking anything", (unsigned long long)kClockIncs[i]);
      return 0;
    }
  }
  for (size_t i = 0; i < gate_count; i++) {
    void **tramp_slot;
    uint8_t *stub = p;
    p = make_gate_stub(p, kGates[i].returns_float, &tramp_slot);
    void *trampoline = NULL;
    MH_STATUS st = MH_CreateHook(ADDR(kGates[i].addr), stub, &trampoline);
    if (st != MH_OK) {
      log_msg("gates: MH_CreateHook(%s) failed: %d", kGates[i].name, st);
      return 0;
    }
    *tramp_slot = trampoline;
    log_msg("gate: %s", kGates[i].name);
  }
  for (size_t i = 0; i < sizeof(kClockIncs) / sizeof(kClockIncs[0]); i++) {
    uint8_t *cave = p;
    p = make_clock_cave(p, (unsigned)i);
    uint8_t *site = ADDR(kClockIncs[i]);
    int64_t rel = (int64_t)(cave - (site + 5));
    if (rel != (int32_t)rel) {
      log_msg("gates: cave out of rel32 range");
      return 0;
    }
    DWORD old;
    VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old);
    site[0] = 0xE8;
    *(int32_t *)(site + 1) = (int32_t)rel;
    site[5] = 0x90;
    VirtualProtect(site, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    log_msg("gate: master clock INC at 0x%llx", (unsigned long long)kClockIncs[i]);
  }
  return 1;
}

/* ---------- engine hooks ------------------------------------------------- */

typedef float (*get_max_tick_rate_fn)(void *self, float delta, BOOL allow_smoothing);
typedef void (*update_time_fn)(void *self);
/* The target prologue consumes RCX (world), EDX (tick type), and XMM2 (delta).
 * This is UWorld::Tick(ELevelTick, float) in the checked UE4 executable. */
typedef void (*world_tick_fn)(void *world, int tick_type, float delta);
static get_max_tick_rate_fn orig_get_max_tick_rate;
static update_time_fn orig_update_time;
static world_tick_fn orig_world_tick;

typedef void (*frame_loop_fn)(void *loop);
typedef void (*game_tick_fn)(void *engine, float delta, uint8_t idle);
typedef void (*sync_fn)(void *self, uint8_t option);
static frame_loop_fn orig_frame_loop;
static game_tick_fn orig_game_tick;
static sync_fn orig_frame_sync, orig_fence_wait;
static struct {
  DWORD thread;
  unsigned frame_depth, game_depth, sync_depth, fence_depth;
  double window_start;
  uint64_t sync_in_game;
  sc6_duration loop, game, world, sync, fence;
} frame_timers;

static double unix_seconds(void) {
  FILETIME stamp;
  ULARGE_INTEGER ticks;
  GetSystemTimeAsFileTime(&stamp);
  ticks.LowPart = stamp.dwLowDateTime;
  ticks.HighPart = stamp.dwHighDateTime;
  return (double)(ticks.QuadPart - 116444736000000000ull) / 10000000.0;
}

static int inside_measured_frame(void) {
  return cfg_frame_timers && frame_timers.frame_depth &&
         GetCurrentThreadId() == frame_timers.thread;
}

static void hook_game_tick(void *engine, float delta, uint8_t idle) {
  if (!inside_measured_frame()) {
    orig_game_tick(engine, delta, idle);
    return;
  }
  unsigned depth = frame_timers.game_depth++;
  double start = depth ? 0.0 : qpc_seconds();
  orig_game_tick(engine, delta, idle);
  frame_timers.game_depth--;
  if (!depth)
    sc6_duration_add(&frame_timers.game, (qpc_seconds() - start) * 1000.0);
}

static void hook_frame_sync(void *self, uint8_t allow_lag) {
  if (!inside_measured_frame()) {
    orig_frame_sync(self, allow_lag);
    return;
  }
  unsigned depth = frame_timers.sync_depth++;
  double start = depth ? 0.0 : qpc_seconds();
  if (!depth && frame_timers.game_depth)
    frame_timers.sync_in_game++;
  orig_frame_sync(self, allow_lag);
  frame_timers.sync_depth--;
  if (!depth)
    sc6_duration_add(&frame_timers.sync, (qpc_seconds() - start) * 1000.0);
}

static void hook_fence_wait(void *self, uint8_t process_tasks) {
  if (!inside_measured_frame()) {
    orig_fence_wait(self, process_tasks);
    return;
  }
  unsigned depth = frame_timers.fence_depth++;
  double start = depth ? 0.0 : qpc_seconds();
  orig_fence_wait(self, process_tasks);
  frame_timers.fence_depth--;
  if (!depth)
    sc6_duration_add(&frame_timers.fence, (qpc_seconds() - start) * 1000.0);
}

static void hook_frame_loop(void *loop) {
  DWORD thread = GetCurrentThreadId();
  if (!cfg_frame_timers || (frame_timers.thread && frame_timers.thread != thread) || frame_timers.frame_depth) {
    orig_frame_loop(loop);
    return;
  }
  frame_timers.thread = thread;
  double start = qpc_seconds();
  if (!frame_timers.window_start)
    frame_timers.window_start = start;
  frame_timers.frame_depth++;
  orig_frame_loop(loop);
  frame_timers.frame_depth--;
  double end = qpc_seconds();
  sc6_duration_add(&frame_timers.loop, (end - start) * 1000.0);
  double elapsed = end - frame_timers.window_start;
  if (elapsed < 1.0)
    return;
  log_msg("frame_detail_v=1 unix_s=%.6f elapsed_s=%.6f frames=%llu "
          "loop_total_ms=%.6f loop_max_ms=%.6f "
          "game_calls=%llu game_total_ms=%.6f game_max_ms=%.6f "
          "world_calls=%llu world_total_ms=%.6f world_max_ms=%.6f "
          "sync_calls=%llu sync_total_ms=%.6f sync_max_ms=%.6f "
          "fence_calls=%llu fence_total_ms=%.6f fence_max_ms=%.6f sync_in_game=%llu",
          unix_seconds(), elapsed, (unsigned long long)frame_timers.loop.count,
          frame_timers.loop.total_ms, frame_timers.loop.max_ms,
          (unsigned long long)frame_timers.game.count, frame_timers.game.total_ms, frame_timers.game.max_ms,
          (unsigned long long)frame_timers.world.count, frame_timers.world.total_ms, frame_timers.world.max_ms,
          (unsigned long long)frame_timers.sync.count, frame_timers.sync.total_ms, frame_timers.sync.max_ms,
          (unsigned long long)frame_timers.fence.count, frame_timers.fence.total_ms, frame_timers.fence.max_ms,
          (unsigned long long)frame_timers.sync_in_game);
  /* The following window starts at the next entry. Do not count log I/O as
   * measured frame-loop work. The frame interval still includes that cost. */
  frame_timers.window_start = 0.0;
  frame_timers.loop = frame_timers.game = frame_timers.world = (sc6_duration){0};
  frame_timers.sync = frame_timers.fence = (sc6_duration){0};
  frame_timers.sync_in_game = 0;
}

static int install_frame_timers(void) {
  struct {
    const char *name;
    uint64_t addr;
    const char *bytes;
    void *hook;
    void **original;
  } sites[] = {
    {"frame loop", ADDR_FRAME_LOOP, "488bc448895810488970185557415441", hook_frame_loop, (void **)&orig_frame_loop},
    {"game engine tick", ADDR_GAME_TICK, "488bc444884018488948085553565741", hook_game_tick, (void **)&orig_game_tick},
    {"frame-end sync", ADDR_FRAME_SYNC, "48895c24084889742410574883ec2048", hook_frame_sync, (void **)&orig_frame_sync},
    {"render fence wait", ADDR_FENCE_WAIT, "48895c2408574883ec20803d6f19d602", hook_fence_wait, (void **)&orig_fence_wait},
  };
  unsigned count = sizeof(sites) / sizeof(sites[0]);
  for (unsigned i = 0; i < count; i++) {
    if (!bytes_match(ADDR(sites[i].addr), sites[i].bytes)) {
      log_msg("frame timers: bytes differ at %s; no detail hooks installed", sites[i].name);
      return 0;
    }
  }
  for (unsigned i = 0; i < count; i++) {
    MH_STATUS status = MH_CreateHook(ADDR(sites[i].addr), sites[i].hook, sites[i].original);
    if (status != MH_OK) {
      log_msg("frame timers: create failed at %s (%d); removing detail hooks", sites[i].name, status);
      while (i)
        MH_RemoveHook(ADDR(sites[--i].addr));
      return 0;
    }
  }
  log_msg("frame timers: four detail hooks created; durations include nested work and waits");
  return 1;
}

static void hook_world_tick(void *world, int tick_type, float delta) {
  if (!cfg_measure || GetCurrentThreadId() != measurements.engine_thread) {
    orig_world_tick(world, tick_type, delta);
    return;
  }
  double start = qpc_seconds();
  orig_world_tick(world, tick_type, delta);
  double elapsed_ms = (qpc_seconds() - start) * 1000.0;
  sc6_duration_add(&measurements.world, elapsed_ms);
  if (inside_measured_frame())
    sc6_duration_add(&frame_timers.world, elapsed_ms);
}

static void measure_frame(double now, double timing_start) {
  uint32_t battle = *(volatile uint32_t *)ADDR(ADDR_BATTLE_COUNTER);
  if (measurements.last_frame == 0.0) {
    measurements.engine_thread = GetCurrentThreadId();
    measurements.last_frame = measurements.window_start = now;
    measurements.last_battle = battle;
    for (unsigned i = 0; i < 2; i++)
      measurements.last_clock[i] = g_clock_advances[i];
    log_msg("measurement: engine_thread=%lu battle_counter=0x%llx; "
            "durations include waits; only world ticks on this thread are timed",
            (unsigned long)measurements.engine_thread, (unsigned long long)ADDR_BATTLE_COUNTER);
    return;
  }
  measurements.engine_frames++;
  sc6_duration_add(&measurements.engine_time, (now - timing_start) * 1000.0);
  measurements.battle_completed += sc6_counter_delta(measurements.last_battle, battle,
                                                     &measurements.counter_resets);
  measurements.last_battle = battle;
  measurements.last_frame = now;
  for (unsigned i = 0; i < 2; i++) {
    uint64_t current = g_clock_advances[i];
    measurements.clock[i] += current - measurements.last_clock[i];
    measurements.last_clock[i] = current;
  }
  double elapsed = now - measurements.window_start;
  if (elapsed < 1.0)
    return;
  log_msg("metrics_v=1 unix_s=%.6f elapsed_s=%.6f engine_frames=%llu "
          "battle_completed=%llu battle_counter=%u counter_resets=%llu clock_r1=%llu clock_r2=%llu "
          "world_calls=%llu world_total_ms=%.6f world_max_ms=%.6f "
          "engine_time_total_ms=%.6f engine_time_max_ms=%.6f",
          unix_seconds(), elapsed, (unsigned long long)measurements.engine_frames,
          (unsigned long long)measurements.battle_completed, battle,
          (unsigned long long)measurements.counter_resets,
          (unsigned long long)measurements.clock[0], (unsigned long long)measurements.clock[1],
          (unsigned long long)measurements.world.count, measurements.world.total_ms, measurements.world.max_ms,
          measurements.engine_time.total_ms, measurements.engine_time.max_ms);
  measurements.window_start = now;
  measurements.engine_frames = measurements.battle_completed = measurements.counter_resets = 0;
  measurements.clock[0] = measurements.clock[1] = 0;
  measurements.world = (sc6_duration){0};
  measurements.engine_time = (sc6_duration){0};
}

static void log_engine_settings(void *engine) {
  uint8_t *e = engine;
  float *maxfps = *(float **)ADDR(ADDR_TMAXFPS_VALUE);
  log_msg("GEngine %p: +0x648=0x%08x (bUseFixedFrameRate=%u) FixedFrameRate=%.3f "
          "SmoothRange lower(type %u, %.2f) upper(type %u, %.2f) t.MaxFPS=%.2f",
          engine, *(uint32_t *)(e + 0x648), (*(uint32_t *)(e + 0x648) >> 6) & 1, *(float *)(e + 0x64C),
          e[0x650], *(float *)(e + 0x654), e[0x658], *(float *)(e + 0x65C), maxfps ? *maxfps : -1.0f);
}

static float hook_get_max_tick_rate(void *self, float delta, BOOL allow_smoothing) {
  static int logged;
  if (!logged && self == *(void **)ADDR(ADDR_GENGINE)) {
    logged = 1;
    log_engine_settings(self);
  }
  if (cfg_uncap) {
    *(volatile uint32_t *)((uint8_t *)self + 0x648) &= ~0x40u; /* bUseFixedFrameRate = false */
    return cfg_max_fps;
  }
  return orig_get_max_tick_rate(self, delta, allow_smoothing);
}

static void hook_update_time(void *self) {
  double timing_start = cfg_measure ? qpc_seconds() : 0.0;
  orig_update_time(self);

  static double last, acc, window_start;
  static unsigned frames, steps;
  const double step = 1.0 / 60.0;
  double now = qpc_seconds();
  if (cfg_measure)
    measure_frame(now, timing_start);
  if (last == 0.0) {
    last = now;
    window_start = now;
  }
  double dt = now - last;
  last = now;

  if (cfg_gate) {
    acc += dt;
    if (acc > 0.1) /* after a long hitch, do not try to catch up */
      acc = 0.1;
    if (acc >= step) {
      acc -= step;
      g_skip_battle = 0;
      steps++;
    } else {
      g_skip_battle = 1;
    }
  }
  frames++;
  if (now - window_start >= 1.0) {
    log_msg("frames=%u steps=%u (%.6f s); steps are scheduler decisions", frames, cfg_gate ? steps : frames, now - window_start);
    frames = steps = 0;
    window_start = now;
  }
}

/* ---------- shader capture ----------------------------------------------- */

/* With SC6VRR_SHADER_DIR=<Windows path>, every ID3D11Device::Create*Shader call
 * is recorded for notes/native-metal.md step 1. Each unique bytecode blob is
 * written once to <dir>\dxbc\<sha256>.dxbc, and every call adds one line to
 * <dir>\index-<time>-<pid>.tsv. Each unique input layout (the description of
 * the vertex buffer contents that a vertex shader reads) goes to
 * <dir>\layouts\<sha256>.txt, and each unique blend state to
 * <dir>\blend\<sha256>.txt. Each new combination of the state that a Metal
 * render pipeline depends on (shaders, input layout, vertex strides, topology,
 * target formats, blend state) adds one line to <dir>\bindings-<time>-<pid>.tsv.
 * The output is game material: keep it private.
 *
 * The device and its immediate context are hooked through their vtables, so the
 * hooks go in after the game creates its first device. d3d11.dll is a static
 * import of the exe, so its creation exports can be hooked from DllMain. */

/* Vtable slot numbers come from the C interface in d3d11.h, so they cannot
 * drift from the declaration order. */
#define DEVICE_SLOT(m) ((int)(offsetof(ID3D11DeviceVtbl, m) / sizeof(void *)))
#define CONTEXT_SLOT(m) ((int)(offsetof(ID3D11DeviceContextVtbl, m) / sizeof(void *)))

enum { STAGE_VS, STAGE_GS, STAGE_GS_SO, STAGE_PS, STAGE_HS, STAGE_DS, STAGE_CS, STAGE_COUNT };
static const char *const kStageNames[STAGE_COUNT] = {"vs", "gs", "gs_so", "ps", "hs", "ds", "cs"};
/* The documented ID3D11Device order; a mismatch here means a different header. */
_Static_assert(DEVICE_SLOT(CreateInputLayout) == 11 && DEVICE_SLOT(CreateVertexShader) == 12 &&
                   DEVICE_SLOT(CreateComputeShader) == 18 && DEVICE_SLOT(GetImmediateContext) == 40,
               "unexpected ID3D11Device vtable layout");
_Static_assert(CONTEXT_SLOT(PSSetShader) == 9 && CONTEXT_SLOT(VSSetShader) == 11 && CONTEXT_SLOT(DrawIndexed) == 12 &&
                   CONTEXT_SLOT(Draw) == 13 && CONTEXT_SLOT(IASetInputLayout) == 17 &&
                   CONTEXT_SLOT(DrawInstancedIndirect) == 40,
               "unexpected ID3D11DeviceContext vtable layout");
static const int kStageSlots[STAGE_COUNT] = {
    DEVICE_SLOT(CreateVertexShader), DEVICE_SLOT(CreateGeometryShader),
    DEVICE_SLOT(CreateGeometryShaderWithStreamOutput), DEVICE_SLOT(CreatePixelShader),
    DEVICE_SLOT(CreateHullShader), DEVICE_SLOT(CreateDomainShader), DEVICE_SLOT(CreateComputeShader)};

/* DXBC is the Direct3D 10/11 shader container: "DXBC", a 16-byte checksum,
 * version 1, total size, chunk count, then chunk offsets. The first token of
 * the SHEX (shader model 5) or SHDR (shader model 4) chunk holds the program
 * type in its high 16 bits and the model in its low byte. */
typedef struct {
  int valid;
  unsigned program_type, major, minor;
} dxbc_info;

static dxbc_info parse_dxbc(const uint8_t *p, size_t n) {
  dxbc_info info = {0};
  uint32_t total, count;
  if (n < 32 || memcmp(p, "DXBC", 4))
    return info;
  memcpy(&total, p + 24, 4);
  memcpy(&count, p + 28, 4);
  if (total < 32 || total > n || count > (total - 32) / 4)
    return info;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t off, size, token;
    memcpy(&off, p + 32 + 4 * i, 4);
    if (off < 32 || off > total - 12)
      continue;
    memcpy(&size, p + off + 4, 4);
    if ((memcmp(p + off, "SHEX", 4) && memcmp(p + off, "SHDR", 4)) || size < 4 || size > total - off - 8)
      continue;
    memcpy(&token, p + off + 8, 4);
    info.valid = 1;
    info.program_type = token >> 16;
    info.major = (token >> 4) & 0xF;
    info.minor = token & 0xF;
    return info;
  }
  return info;
}

/* Writes "vs_5_0" style names; program types 0-5 are ps, vs, gs, hs, ds, cs. */
static void dxbc_model_name(dxbc_info info, char out[16]) {
  static const char *const kTypes[] = {"ps", "vs", "gs", "hs", "ds", "cs"};
  if (info.valid && info.program_type < 6)
    snprintf(out, 16, "%s_%u_%u", kTypes[info.program_type], info.major, info.minor);
  else
    snprintf(out, 16, "unknown");
}

static int sha256_raw(const void *data, size_t n, uint8_t digest[32]) {
  BCRYPT_ALG_HANDLE alg = NULL;
  BCRYPT_HASH_HANDLE hash = NULL;
  int ok = n <= 0xFFFFFFFFu && BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 &&
           BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) == 0 &&
           BCryptHashData(hash, (PUCHAR)data, (ULONG)n, 0) == 0 &&
           BCryptFinishHash(hash, digest, 32, 0) == 0;
  if (hash) BCryptDestroyHash(hash);
  if (alg) BCryptCloseAlgorithmProvider(alg, 0);
  return ok;
}

static void digest_hex(const uint8_t digest[32], char hex[65]) {
  for (int i = 0; i < 32; i++)
    sprintf(hex + 2 * i, "%02x", digest[i]);
}

static int sha256_hex(const void *data, size_t n, char hex[65]) {
  uint8_t digest[32];
  if (!sha256_raw(data, n, digest))
    return 0;
  digest_hex(digest, hex);
  return 1;
}

/* Content identities. Each unique SHA-256 (of a shader's bytecode or of a
 * state description) gets a small number, its ID. The object table maps each
 * object pointer that the game binds to the ID of what created it. The game
 * recreates equal objects at new addresses, so recorded pipelines are keyed by
 * IDs, not by addresses: equal content is one pipeline, however often it is
 * recreated. The device can reuse a freed address only for a new object, and
 * that object's creation overwrites the entry. object_lock guards both tables;
 * draw-time lookups take it shared. */
#define OBJECT_SLOTS (1 << 18)
#define ID_LIMIT (1 << 16)
#define ID_NONE 0u
#define ID_UNKNOWN 0xFFFFFFFFu
static struct {
  void *object;
  uint32_t id, extra;
} object_ids[OBJECT_SLOTS];
static uint8_t id_digests[ID_LIMIT][32];
static uint32_t id_slots[ID_LIMIT * 2];
static uint32_t id_count;
static size_t object_count;
static int object_table_full_logged, id_table_full_logged;
static SRWLOCK object_lock = SRWLOCK_INIT;

static size_t pointer_slot(const void *p, size_t slots) {
  return (size_t)((((uint64_t)(uintptr_t)p >> 4) * 0x9E3779B97F4A7C15ull) >> 32) & (slots - 1);
}

/* Caller holds object_lock exclusively. The table stays at most half full. */
static uint32_t intern_digest(const uint8_t digest[32]) {
  uint64_t h;
  memcpy(&h, digest, sizeof(h));
  for (size_t i = h & (ID_LIMIT * 2 - 1);; i = (i + 1) & (ID_LIMIT * 2 - 1)) {
    uint32_t id = id_slots[i];
    if (!id) {
      if (id_count >= ID_LIMIT - 1) {
        if (!id_table_full_logged++)
          log_msg("shaders: ID table full; later objects show 'unknown'");
        return ID_UNKNOWN;
      }
      id = ++id_count;
      memcpy(id_digests[id], digest, 32);
      id_slots[i] = id;
      return id;
    }
    if (!memcmp(id_digests[id], digest, 32))
      return id;
  }
}

/* `extra` is kept with the pointer; for input layouts it is the mask of
 * vertex buffer slots that the layout reads. */
static void object_id_set(void *object, const uint8_t digest[32], uint32_t extra) {
  AcquireSRWLockExclusive(&object_lock);
  uint32_t id = intern_digest(digest);
  for (size_t i = pointer_slot(object, OBJECT_SLOTS);; i = (i + 1) & (OBJECT_SLOTS - 1)) {
    if (object_ids[i].object == object) {
      object_ids[i].id = id;
      object_ids[i].extra = extra;
      break;
    }
    if (!object_ids[i].object) {
      /* Keep a quarter free so that probes stay short and always end. */
      if (object_count >= OBJECT_SLOTS / 4 * 3) {
        if (!object_table_full_logged++)
          log_msg("shaders: object table full; later bindings show 'unknown'");
        break;
      }
      object_ids[i].object = object;
      object_ids[i].id = id;
      object_ids[i].extra = extra;
      object_count++;
      break;
    }
  }
  ReleaseSRWLockExclusive(&object_lock);
}

/* Returns ID_NONE for NULL and ID_UNKNOWN for an object created before the hooks. */
static uint32_t object_lookup(void *object, uint32_t *extra) {
  uint32_t id = object ? ID_UNKNOWN : ID_NONE;
  if (extra)
    *extra = 0;
  if (!object)
    return id;
  AcquireSRWLockShared(&object_lock);
  for (size_t i = pointer_slot(object, OBJECT_SLOTS); object_ids[i].object; i = (i + 1) & (OBJECT_SLOTS - 1))
    if (object_ids[i].object == object) {
      id = object_ids[i].id;
      if (extra)
        *extra = object_ids[i].extra;
      break;
    }
  ReleaseSRWLockShared(&object_lock);
  return id;
}

static void id_hex(uint32_t id, char hex[65]) {
  if (id == ID_NONE) {
    strcpy(hex, "none");
  } else if (id == ID_UNKNOWN) {
    strcpy(hex, "unknown");
  } else {
    AcquireSRWLockShared(&object_lock);
    digest_hex(id_digests[id], hex);
    ReleaseSRWLockShared(&object_lock);
  }
}

static char shader_dir[MAX_PATH];
static FILE *shader_index, *binding_file;
static CRITICAL_SECTION shader_lock, binding_lock;
static volatile LONG shader_hooks_installed;
static uint64_t shader_calls, shader_unique;

static int shader_capture_open(const char *dir) {
  char path[MAX_PATH + 64];
  SYSTEMTIME t;
  if (strlen(dir) >= sizeof(shader_dir) - 80)
    return 0;
  strcpy(shader_dir, dir);
  CreateDirectoryA(shader_dir, NULL);
  snprintf(path, sizeof(path), "%s\\dxbc", shader_dir);
  CreateDirectoryA(path, NULL);
  snprintf(path, sizeof(path), "%s\\layouts", shader_dir);
  CreateDirectoryA(path, NULL);
  snprintf(path, sizeof(path), "%s\\blend", shader_dir);
  CreateDirectoryA(path, NULL);
  snprintf(path, sizeof(path), "%s\\frames", shader_dir);
  CreateDirectoryA(path, NULL);
  snprintf(path, sizeof(path), "%s\\depth", shader_dir);
  CreateDirectoryA(path, NULL);
  snprintf(path, sizeof(path), "%s\\raster", shader_dir);
  CreateDirectoryA(path, NULL);
  snprintf(path, sizeof(path), "%s\\sampler", shader_dir);
  CreateDirectoryA(path, NULL);
  GetLocalTime(&t);
  snprintf(path, sizeof(path), "%s\\bindings-%04d%02d%02d-%02d%02d%02d-%lu.tsv", shader_dir, t.wYear,
           t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId());
  binding_file = fopen(path, "w");
  snprintf(path, sizeof(path), "%s\\index-%04d%02d%02d-%02d%02d%02d-%lu.tsv", shader_dir, t.wYear, t.wMonth,
           t.wDay, t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId());
  shader_index = fopen(path, "w");
  if (!shader_index || !binding_file)
    return 0;
  InitializeCriticalSection(&shader_lock);
  InitializeCriticalSection(&binding_lock);
  fprintf(shader_index, "seconds\tthread\tstage\tmodel\tbytes\tsha256\thresult\tnew\n");
  fflush(shader_index);
  fprintf(binding_file, "seconds\tthread\ttopology\tinput_layout\tvs\tgs\tps\tblend\trt_formats\tdsv_format"
                        "\tmultisample\tstrides\n");
  fflush(binding_file);
  log_msg("shaders: recording to %s", path);
  return 1;
}

/* Records one Create*Shader call. Runs after the real call so that the result
 * code is known; the bytecode stays valid because the caller still owns it. */
static void capture_shader(int stage, const void *code, SIZE_T length, HRESULT hr, void *object) {
  char hex[65] = "none", model[16] = "none", path[MAX_PATH + 96], tmp[MAX_PATH + 100];
  uint8_t digest[32];
  int is_new = 0, hashed = 0;
  if (code && length) {
    hashed = sha256_raw(code, length, digest);
    if (hashed)
      digest_hex(digest, hex);
    else
      strcpy(hex, "hash-failed");
    dxbc_model_name(parse_dxbc(code, length), model);
  }
  EnterCriticalSection(&shader_lock);
  shader_calls++;
  if (hashed && object)
    object_id_set(object, digest, 0);
  if (code && length && hex[0] != 'h') {
    snprintf(path, sizeof(path), "%s\\dxbc\\%s.dxbc", shader_dir, hex);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
      /* Write to a temporary name first, so an interrupted write never looks
       * like a complete shader on the next run. */
      snprintf(tmp, sizeof(tmp), "%s.tmp", path);
      FILE *f = fopen(tmp, "wb");
      size_t written = f ? fwrite(code, 1, length, f) : 0;
      if (f && fclose(f) == 0 && written == length && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        is_new = 1;
        shader_unique++;
      } else {
        DeleteFileA(tmp);
        log_msg("shaders: could not write %s", path);
      }
    }
  }
  fprintf(shader_index, "%.6f\t%lu\t%s\t%s\t%llu\t%s\t0x%08lx\t%d\n", qpc_seconds(), GetCurrentThreadId(),
          kStageNames[stage], model, (unsigned long long)length, hex, (unsigned long)hr, is_new);
  fflush(shader_index);
  if (shader_calls == 1 || shader_calls % 1000 == 0)
    log_msg("shaders: %llu calls, %llu new files this run", (unsigned long long)shader_calls,
            (unsigned long long)shader_unique);
  LeaveCriticalSection(&shader_lock);
}

typedef HRESULT(WINAPI *create_shader_fn)(void *self, const void *code, SIZE_T length, void *linkage, void **out);
typedef HRESULT(WINAPI *create_gs_so_fn)(void *self, const void *code, SIZE_T length, const void *entries,
                                         UINT entry_count, const UINT *strides, UINT stride_count,
                                         UINT rasterized_stream, void *linkage, void **out);
static create_shader_fn orig_create_shader[STAGE_COUNT];
static create_gs_so_fn orig_create_gs_so;

#define SHADER_HOOK(stage)                                                                              \
  static HRESULT WINAPI hook_create_##stage(void *self, const void *code, SIZE_T length, void *linkage, \
                                            void **out) {                                               \
    HRESULT hr = orig_create_shader[STAGE_##stage](self, code, length, linkage, out);                   \
    capture_shader(STAGE_##stage, code, length, hr, SUCCEEDED(hr) && out ? *out : NULL);                \
    return hr;                                                                                          \
  }
SHADER_HOOK(VS)
SHADER_HOOK(GS)
SHADER_HOOK(PS)
SHADER_HOOK(HS)
SHADER_HOOK(DS)
SHADER_HOOK(CS)

static HRESULT WINAPI hook_create_GS_SO(void *self, const void *code, SIZE_T length, const void *entries,
                                        UINT entry_count, const UINT *strides, UINT stride_count,
                                        UINT rasterized_stream, void *linkage, void **out) {
  HRESULT hr = orig_create_gs_so(self, code, length, entries, entry_count, strides, stride_count,
                                 rasterized_stream, linkage, out);
  capture_shader(STAGE_GS_SO, code, length, hr, SUCCEEDED(hr) && out ? *out : NULL);
  return hr;
}

/* Writes one described state object (an input layout or a blend state) once to
 * <dir>\<folder>\<sha256>.txt and adds an index line. Its identity is the
 * SHA-256 of its text form, so the same description created twice gets the
 * same file. Enum values stay numeric (DXGI_FORMAT, D3D11_BLEND, and so on).
 * `extra` is kept with the object pointer; for layouts it is the mask of
 * vertex buffer slots that the layout reads. */
static void capture_described_object(const char *folder, const char *stage, const char *header,
                                     const char *text, size_t used, void *object, uint32_t extra) {
  char hex[65], path[MAX_PATH + 96], tmp[MAX_PATH + 100];
  uint8_t digest[32];
  int is_new = 0;
  if (!sha256_raw(text, used, digest))
    return;
  digest_hex(digest, hex);
  EnterCriticalSection(&shader_lock);
  object_id_set(object, digest, extra);
  snprintf(path, sizeof(path), "%s\\%s\\%s.txt", shader_dir, folder, hex);
  if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (f) {
      fprintf(f, "%s%s", header, text);
      if (fclose(f) == 0 && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
        is_new = 1;
      else
        DeleteFileA(tmp);
    }
  }
  fprintf(shader_index, "%.6f\t%lu\t%s\t%s\t%llu\t%s\t0x00000000\t%d\n", qpc_seconds(), GetCurrentThreadId(), stage,
          folder, (unsigned long long)used, hex, is_new);
  fflush(shader_index);
  LeaveCriticalSection(&shader_lock);
}

static void capture_input_layout(const D3D11_INPUT_ELEMENT_DESC *elements, UINT count, const void *code,
                                 SIZE_T length, void *layout) {
  char text[D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT * 160] = "", header[160], vs_hex[65] = "none";
  size_t used = 0;
  uint32_t slots = 0;
  for (UINT i = 0; elements && i < count && i < D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT; i++) {
    const D3D11_INPUT_ELEMENT_DESC *e = &elements[i];
    int n = snprintf(text + used, sizeof(text) - used, "%.64s\t%u\t%u\t%u\t%u\t%u\t%u\n",
                     e->SemanticName ? e->SemanticName : "", e->SemanticIndex, (unsigned)e->Format,
                     e->InputSlot, e->AlignedByteOffset, (unsigned)e->InputSlotClass, e->InstanceDataStepRate);
    if (n < 0 || (size_t)n >= sizeof(text) - used)
      break;
    used += n;
    if (e->InputSlot < 32)
      slots |= 1u << e->InputSlot;
  }
  if (code && length)
    sha256_hex(code, length, vs_hex);
  snprintf(header, sizeof(header),
           "# vertex shader signature: %s\nsemantic\tindex\tformat\tslot\toffset\tclass\tstep_rate\n", vs_hex);
  capture_described_object("layouts", "il", header, text, used, layout, slots);
}

static void capture_blend_state(const D3D11_BLEND_DESC *desc, void *state) {
  char text[1024];
  size_t used = 0;
  if (!desc)
    return;
  used += snprintf(text, sizeof(text), "alpha_to_coverage\t%d\nindependent_blend\t%d\n",
                   (int)desc->AlphaToCoverageEnable, (int)desc->IndependentBlendEnable);
  for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) {
    const D3D11_RENDER_TARGET_BLEND_DESC *r = &desc->RenderTarget[i];
    used += snprintf(text + used, sizeof(text) - used, "rt%d\t%d\t%u\t%u\t%u\t%u\t%u\t%u\t%u\n", i,
                     (int)r->BlendEnable, (unsigned)r->SrcBlend, (unsigned)r->DestBlend, (unsigned)r->BlendOp,
                     (unsigned)r->SrcBlendAlpha, (unsigned)r->DestBlendAlpha, (unsigned)r->BlendOpAlpha,
                     (unsigned)r->RenderTargetWriteMask);
  }
  capture_described_object("blend", "bs", "# rtN: enable src dst op src_alpha dst_alpha op_alpha write_mask\n",
                           text, used, state, 0);
}

typedef HRESULT(WINAPI *create_input_layout_fn)(void *self, const D3D11_INPUT_ELEMENT_DESC *elements, UINT count,
                                                const void *code, SIZE_T length, void **out);
typedef HRESULT(WINAPI *create_blend_state_fn)(void *self, const D3D11_BLEND_DESC *desc, void **out);
static create_input_layout_fn orig_create_input_layout;
static create_blend_state_fn orig_create_blend_state;

static HRESULT WINAPI hook_create_input_layout(void *self, const D3D11_INPUT_ELEMENT_DESC *elements, UINT count,
                                               const void *code, SIZE_T length, void **out) {
  HRESULT hr = orig_create_input_layout(self, elements, count, code, length, out);
  if (SUCCEEDED(hr) && out && *out)
    capture_input_layout(elements, count, code, length, *out);
  return hr;
}

static HRESULT WINAPI hook_create_blend_state(void *self, const D3D11_BLEND_DESC *desc, void **out) {
  HRESULT hr = orig_create_blend_state(self, desc, out);
  if (SUCCEEDED(hr) && out && *out)
    capture_blend_state(desc, *out);
  return hr;
}

/* Depth-stencil, rasterizer, and sampler states. Metal keeps these outside the
 * render pipeline, so they are not part of the recorded pipeline combinations.
 * Their descriptions are stored like blend states, and the frame log names the
 * ones bound at each draw. Index stage codes: dss, rss, sss. */
static void capture_depth_state(const D3D11_DEPTH_STENCIL_DESC *d, void *state) {
  char text[512];
  if (!d)
    return;
  int used = snprintf(text, sizeof(text),
                      "depth_enable\t%d\ndepth_write_mask\t%u\ndepth_func\t%u\nstencil_enable\t%d\n"
                      "stencil_read_mask\t%u\nstencil_write_mask\t%u\n"
                      "front\t%u\t%u\t%u\t%u\nback\t%u\t%u\t%u\t%u\n",
                      (int)d->DepthEnable, (unsigned)d->DepthWriteMask, (unsigned)d->DepthFunc,
                      (int)d->StencilEnable, d->StencilReadMask, d->StencilWriteMask,
                      (unsigned)d->FrontFace.StencilFailOp, (unsigned)d->FrontFace.StencilDepthFailOp,
                      (unsigned)d->FrontFace.StencilPassOp, (unsigned)d->FrontFace.StencilFunc,
                      (unsigned)d->BackFace.StencilFailOp, (unsigned)d->BackFace.StencilDepthFailOp,
                      (unsigned)d->BackFace.StencilPassOp, (unsigned)d->BackFace.StencilFunc);
  capture_described_object("depth", "dss",
                           "# front/back: stencil_fail depth_fail pass func (D3D11 enum values)\n", text,
                           (size_t)used, state, 0);
}

static void capture_raster_state(const D3D11_RASTERIZER_DESC *d, void *state) {
  char text[512];
  if (!d)
    return;
  int used = snprintf(text, sizeof(text),
                      "fill\t%u\ncull\t%u\nfront_counter_clockwise\t%d\ndepth_bias\t%d\ndepth_bias_clamp\t%g\n"
                      "slope_scaled_depth_bias\t%g\ndepth_clip\t%d\nscissor\t%d\nmultisample\t%d\n"
                      "antialiased_line\t%d\n",
                      (unsigned)d->FillMode, (unsigned)d->CullMode, (int)d->FrontCounterClockwise, d->DepthBias,
                      d->DepthBiasClamp, d->SlopeScaledDepthBias, (int)d->DepthClipEnable, (int)d->ScissorEnable,
                      (int)d->MultisampleEnable, (int)d->AntialiasedLineEnable);
  capture_described_object("raster", "rss", "# D3D11 enum values\n", text, (size_t)used, state, 0);
}

static void capture_sampler_state(const D3D11_SAMPLER_DESC *d, void *state) {
  char text[512];
  if (!d)
    return;
  int used = snprintf(text, sizeof(text),
                      "filter\t%u\naddress\t%u\t%u\t%u\nmip_lod_bias\t%g\nmax_anisotropy\t%u\ncomparison\t%u\n"
                      "border_color\t%g\t%g\t%g\t%g\nlod_range\t%g\t%g\n",
                      (unsigned)d->Filter, (unsigned)d->AddressU, (unsigned)d->AddressV, (unsigned)d->AddressW,
                      d->MipLODBias, d->MaxAnisotropy, (unsigned)d->ComparisonFunc, d->BorderColor[0],
                      d->BorderColor[1], d->BorderColor[2], d->BorderColor[3], d->MinLOD, d->MaxLOD);
  capture_described_object("sampler", "sss", "# D3D11 enum values; address is u v w\n", text, (size_t)used,
                           state, 0);
}

typedef HRESULT(WINAPI *create_depth_state_fn)(void *self, const D3D11_DEPTH_STENCIL_DESC *desc, void **out);
typedef HRESULT(WINAPI *create_raster_state_fn)(void *self, const D3D11_RASTERIZER_DESC *desc, void **out);
typedef HRESULT(WINAPI *create_sampler_state_fn)(void *self, const D3D11_SAMPLER_DESC *desc, void **out);
static create_depth_state_fn orig_create_depth_state;
static create_raster_state_fn orig_create_raster_state;
static create_sampler_state_fn orig_create_sampler_state;

static HRESULT WINAPI hook_create_depth_state(void *self, const D3D11_DEPTH_STENCIL_DESC *desc, void **out) {
  HRESULT hr = orig_create_depth_state(self, desc, out);
  if (SUCCEEDED(hr) && out && *out)
    capture_depth_state(desc, *out);
  return hr;
}
static HRESULT WINAPI hook_create_raster_state(void *self, const D3D11_RASTERIZER_DESC *desc, void **out) {
  HRESULT hr = orig_create_raster_state(self, desc, out);
  if (SUCCEEDED(hr) && out && *out)
    capture_raster_state(desc, *out);
  return hr;
}
static HRESULT WINAPI hook_create_sampler_state(void *self, const D3D11_SAMPLER_DESC *desc, void **out) {
  HRESULT hr = orig_create_sampler_state(self, desc, out);
  if (SUCCEEDED(hr) && out && *out)
    capture_sampler_state(desc, *out);
  return hr;
}


/* Everything a Metal render pipeline depends on, for one draw: the shaders, the
 * vertex layout and strides, the primitive class, the target formats, and the
 * blend state. All fields are 4 bytes, so there is no padding, and memcmp and
 * a byte hash see only real values. */
#define STRIDE_SLOTS 16
typedef struct {
  uint32_t layout, vs, gs, ps, blend; /* content IDs */
  UINT topology, rt_count, dsv_format, multisample_mask;
  UINT rt_formats[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
  UINT strides[STRIDE_SLOTS];
  UINT used;
} binding_key;
_Static_assert(sizeof(binding_key) == 4 * (5 + 4 + D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT + STRIDE_SLOTS + 1),
               "binding_key has padding");

/* The bound state of the immediate context. The Set hooks write it and the
 * draw hooks read it. UE4 4.17 issues both from its one RHI thread, so plain
 * stores suffice; binding_lock only protects the table of seen combinations.
 * Strides are kept for every slot, but a draw records only the slots that its
 * input layout reads, so stale strides in unused slots add no combinations. */
static binding_key bound;
static UINT bound_strides[STRIDE_SLOTS];
static uint32_t bound_layout_slots;

#define BINDING_SLOTS (1 << 16)
static binding_key bindings_seen[BINDING_SLOTS];
static size_t binding_count;
static int binding_table_full_logged;
static uint64_t draw_calls;

static uint64_t fnv1a(const void *data, size_t n) {
  const uint8_t *p = data;
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; i++)
    h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}

static void join_uints(char *out, size_t size, const UINT *values, UINT count, uint32_t mask, int with_slots) {
  size_t used = 0;
  out[0] = 0;
  for (UINT i = 0; i < count && used < size; i++) {
    if (!(mask >> i & 1))
      continue;
    int n = with_slots ? snprintf(out + used, size - used, "%s%u:%u", used ? "," : "", i, values[i])
                       : snprintf(out + used, size - used, "%s%u", used ? "," : "", values[i]);
    if (n < 0)
      break;
    used += n;
  }
  if (!used)
    snprintf(out, size, "none");
}

/* Called on every draw. Only a combination not seen before costs more than
 * one hash probe; its IDs are written out as hashes. */
static void note_draw(void) {
  binding_key key;
  memcpy(&key, &bound, sizeof(key));
  for (int i = 0; i < STRIDE_SLOTS; i++)
    key.strides[i] = bound_layout_slots >> i & 1 ? bound_strides[i] : 0;
  key.used = 0;
  uint64_t h = fnv1a(&key, sizeof(key));
  key.used = 1;
  EnterCriticalSection(&binding_lock);
  draw_calls++;
  for (size_t i = h & (BINDING_SLOTS - 1);; i = (i + 1) & (BINDING_SLOTS - 1)) {
    binding_key *e = &bindings_seen[i];
    if (e->used && !memcmp(e, &key, sizeof(key)))
      break;
    if (!e->used) {
      if (binding_count >= BINDING_SLOTS / 4 * 3) {
        if (!binding_table_full_logged++)
          log_msg("shaders: binding table full; later combinations are not recorded");
        break;
      }
      memcpy(e, &key, sizeof(key));
      binding_count++;
      char layout[65], vs[65], gs[65], ps[65], blend[65], rts[128], strides[256];
      id_hex(key.layout, layout);
      id_hex(key.vs, vs);
      id_hex(key.gs, gs);
      id_hex(key.ps, ps);
      id_hex(key.blend, blend);
      join_uints(rts, sizeof(rts), key.rt_formats, key.rt_count, 0xFF, 0);
      join_uints(strides, sizeof(strides), key.strides, STRIDE_SLOTS, bound_layout_slots, 1);
      fprintf(binding_file, "%.6f\t%lu\t%u\t%s\t%s\t%s\t%s\t%s\t%s\t%u\t0x%x\t%s\n", qpc_seconds(),
              GetCurrentThreadId(), key.topology, layout, vs, gs, ps, blend, rts, key.dsv_format,
              key.multisample_mask, strides);
      fflush(binding_file);
      if (binding_count == 1 || binding_count % 1000 == 0)
        log_msg("shaders: %llu draws, %llu pipeline combinations", (unsigned long long)draw_calls,
                (unsigned long long)binding_count);
      break;
    }
  }
  LeaveCriticalSection(&binding_lock);
}

/* ---------- one-frame log (F10) ------------------------------------------ */

/* F10 arms the log at a Present. The log then covers the next full frame, from
 * that Present to the following one, in call order: target and resource
 * bindings, clears, viewports, copies, dispatches, and each draw with its
 * pipeline state. DXMT's Metal capture (MTL_CAPTURE_ENABLED=1) reacts to the
 * same key and also captures the next frame, so both describe the same frame
 * or adjacent ones. Each hook checks frame_state before it does any logging
 * work. The immediate context is single-threaded, so the log needs no lock. */
enum { FRAME_IDLE, FRAME_ARMED, FRAME_LOGGING };
static volatile LONG frame_state;
static FILE *frame_file;
static uint64_t frame_calls, frames_logged;
static int frame_key_was_down;
static uint32_t bound_cs, bound_depth_state, bound_raster_state, bound_stencil_ref;

static int real_frame_key_down(void) { return (GetAsyncKeyState(VK_F10) & 0x8000) != 0; }
static int (*frame_key_down)(void) = real_frame_key_down; /* the test replaces it */

typedef ULONG(WINAPI *com_release_fn)(void *object);
typedef void(WINAPI *get_resource_fn)(void *view, void **resource);
typedef void(WINAPI *get_type_fn)(void *resource, D3D11_RESOURCE_DIMENSION *type);
typedef void(WINAPI *texture2d_desc_fn)(void *texture, D3D11_TEXTURE2D_DESC *desc);
typedef void(WINAPI *buffer_desc_fn)(void *buffer, D3D11_BUFFER_DESC *desc);
/* The method pointer of a COM object, by interface and method name. */
#define VTBL_METHOD(object, iface, method) \
  ((*(void ***)(object))[offsetof(iface##Vtbl, method) / sizeof(void *)])

static void frame_line(const char *fmt, ...) {
  if (!frame_file)
    return;
  fprintf(frame_file, "%llu\t", (unsigned long long)frame_calls++);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(frame_file, fmt, ap);
  va_end(ap);
  fputc('\n', frame_file);
}

/* The resource address identifies a texture across the frame, so the log shows
 * which pass writes a texture and which later pass reads it. */
static void describe_resource(void *resource, char *out, size_t size) {
  if (!resource) {
    snprintf(out, size, "none");
    return;
  }
  D3D11_RESOURCE_DIMENSION type = D3D11_RESOURCE_DIMENSION_UNKNOWN;
  ((get_type_fn)VTBL_METHOD(resource, ID3D11Resource, GetType))(resource, &type);
  if (type == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
    D3D11_TEXTURE2D_DESC d;
    ((texture2d_desc_fn)VTBL_METHOD(resource, ID3D11Texture2D, GetDesc))(resource, &d);
    snprintf(out, size, "tex2d:%p:%ux%u:format=%u:mips=%u:array=%u:samples=%u", resource, d.Width, d.Height,
             (unsigned)d.Format, d.MipLevels, d.ArraySize, d.SampleDesc.Count);
  } else if (type == D3D11_RESOURCE_DIMENSION_BUFFER) {
    D3D11_BUFFER_DESC d;
    ((buffer_desc_fn)VTBL_METHOD(resource, ID3D11Buffer, GetDesc))(resource, &d);
    snprintf(out, size, "buffer:%p:%u", resource, d.ByteWidth);
  } else {
    snprintf(out, size, "resource%d:%p", (int)type, resource);
  }
}

/* GetResource adds a reference, so release it after describing. */
static void describe_view(void *view, char *out, size_t size) {
  void *resource = NULL;
  if (!view) {
    snprintf(out, size, "none");
    return;
  }
  ((get_resource_fn)VTBL_METHOD(view, ID3D11View, GetResource))(view, &resource);
  describe_resource(resource, out, size);
  if (resource)
    ((com_release_fn)VTBL_METHOD(resource, ID3D11Resource, Release))(resource);
}

static void describe_views(const char *call, UINT start, UINT count, void *const *views) {
  char line[4096], one[160];
  size_t used = (size_t)snprintf(line, sizeof(line), "%s\tstart=%u\tcount=%u", call, start, count);
  for (UINT i = 0; views && i < count && used < sizeof(line); i++) {
    describe_view(views[i], one, sizeof(one));
    used += (size_t)snprintf(line + used, sizeof(line) - used, "\t%u=%s", start + i, one);
  }
  frame_line("%s", line);
}

/* Content IDs are written as the first 16 hex digits of their SHA-256, which is
 * enough to join with the bindings and shader files. */
static void short_id(uint32_t id, char out[65]) {
  id_hex(id, out);
  if (strlen(out) > 16)
    out[16] = 0;
}

static void frame_draw(const char *kind, const char *args) {
  char layout[65], vs[65], gs[65], ps[65], blend[65], rts[128], depth[65], raster[65];
  short_id(bound_depth_state, depth);
  short_id(bound_raster_state, raster);
  short_id(bound.layout, layout);
  short_id(bound.vs, vs);
  short_id(bound.gs, gs);
  short_id(bound.ps, ps);
  short_id(bound.blend, blend);
  join_uints(rts, sizeof(rts), bound.rt_formats, bound.rt_count, 0xFF, 0);
  frame_line("%s\t%s\ttopology=%u\tlayout=%s\tvs=%s\tgs=%s\tps=%s\tblend=%s\trt_formats=%s\tdsv_format=%u"
             "\tdepth_state=%s\tstencil_ref=%u\traster=%s",
             kind, args, bound.topology, layout, vs, gs, ps, blend, rts, bound.dsv_format, depth, bound_stencil_ref,
             raster);
}

#define FRAME_LOGGING_NOW (frame_state == FRAME_LOGGING)

/* Called at every Present, before the real one. */
static void frame_present(void) {
  int down = frame_key_down();
  int pressed = down && !frame_key_was_down;
  frame_key_was_down = down;
  if (frame_state == FRAME_IDLE) {
    if (pressed)
      frame_state = FRAME_ARMED;
  } else if (frame_state == FRAME_ARMED) {
    char path[MAX_PATH + 96];
    SYSTEMTIME t;
    GetLocalTime(&t);
    snprintf(path, sizeof(path), "%s\\frames\\frame-%04d%02d%02d-%02d%02d%02d-%llu.tsv", shader_dir, t.wYear,
             t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, (unsigned long long)frames_logged);
    frame_file = fopen(path, "w");
    frame_calls = 0;
    if (frame_file) {
      char rts[128];
      join_uints(rts, sizeof(rts), bound.rt_formats, bound.rt_count, 0xFF, 0);
      fprintf(frame_file, "# One frame from Present to Present. Pointers identify resources within this frame.\n");
      frame_line("FrameStart\trt_formats=%s\tdsv_format=%u", rts, bound.dsv_format);
      frame_state = FRAME_LOGGING;
      log_msg("frames: logging one frame to %s", path);
    } else {
      frame_state = FRAME_IDLE;
      log_msg("frames: could not open %s", path);
    }
  } else {
    frame_line("Present");
    fclose(frame_file);
    frame_file = NULL;
    frames_logged++;
    frame_state = FRAME_IDLE;
    log_msg("frames: frame log %llu written", (unsigned long long)frames_logged);
  }
}

typedef void(WINAPI *set_shader_fn)(void *ctx, void *shader, void *const *instances, UINT count);
typedef void(WINAPI *set_layout_fn)(void *ctx, void *layout);
typedef void(WINAPI *set_topology_fn)(void *ctx, UINT topology);
typedef void(WINAPI *set_vertex_buffers_fn)(void *ctx, UINT start, UINT count, void *const *buffers,
                                            const UINT *strides, const UINT *offsets);
typedef void(WINAPI *set_targets_fn)(void *ctx, UINT count, void *const *rtvs, void *dsv);
typedef void(WINAPI *set_targets_uavs_fn)(void *ctx, UINT count, void *const *rtvs, void *dsv, UINT uav_start,
                                          UINT uav_count, void *const *uavs, const UINT *initial_counts);
typedef void(WINAPI *set_blend_fn)(void *ctx, void *state, const FLOAT factor[4], UINT sample_mask);
typedef void(WINAPI *draw_fn)(void *ctx, UINT count, UINT start);
typedef void(WINAPI *draw_indexed_fn)(void *ctx, UINT count, UINT start, INT base);
typedef void(WINAPI *draw_indexed_instanced_fn)(void *ctx, UINT count, UINT instances, UINT start, INT base,
                                                UINT start_instance);
typedef void(WINAPI *draw_instanced_fn)(void *ctx, UINT count, UINT instances, UINT start, UINT start_instance);
typedef void(WINAPI *draw_auto_fn)(void *ctx);
typedef void(WINAPI *draw_indirect_fn)(void *ctx, void *args, UINT offset);
typedef void(WINAPI *rtv_desc_fn)(void *view, D3D11_RENDER_TARGET_VIEW_DESC *desc);
typedef void(WINAPI *dsv_desc_fn)(void *view, D3D11_DEPTH_STENCIL_VIEW_DESC *desc);

enum {
  CTX_VS_SET, CTX_GS_SET, CTX_PS_SET, CTX_LAYOUT, CTX_TOPOLOGY, CTX_VERTEX_BUFFERS, CTX_TARGETS,
  CTX_TARGETS_UAVS, CTX_BLEND, CTX_DRAW, CTX_DRAW_INDEXED, CTX_DRAW_INDEXED_INSTANCED, CTX_DRAW_INSTANCED,
  CTX_DRAW_AUTO, CTX_DRAW_INDEXED_INDIRECT, CTX_DRAW_INSTANCED_INDIRECT, CTX_CS_SET, CTX_PS_SRV, CTX_CS_SRV,
  CTX_CS_UAV, CTX_CLEAR_RTV, CTX_CLEAR_DSV, CTX_VIEWPORTS, CTX_DISPATCH, CTX_DISPATCH_INDIRECT, CTX_COPY_RESOURCE,
  CTX_COPY_REGION, CTX_RESOLVE, CTX_DEPTH_STATE, CTX_RASTER_STATE, CTX_VS_SRV, CTX_PS_SAMPLERS, CTX_VS_SAMPLERS,
  CTX_CS_SAMPLERS, CTX_GET_DATA, CTX_MAP, CTX_FLUSH, CTX_COUNT
};
static const int kContextSlots[CTX_COUNT] = {
    CONTEXT_SLOT(VSSetShader), CONTEXT_SLOT(GSSetShader), CONTEXT_SLOT(PSSetShader),
    CONTEXT_SLOT(IASetInputLayout), CONTEXT_SLOT(IASetPrimitiveTopology), CONTEXT_SLOT(IASetVertexBuffers),
    CONTEXT_SLOT(OMSetRenderTargets), CONTEXT_SLOT(OMSetRenderTargetsAndUnorderedAccessViews),
    CONTEXT_SLOT(OMSetBlendState), CONTEXT_SLOT(Draw), CONTEXT_SLOT(DrawIndexed),
    CONTEXT_SLOT(DrawIndexedInstanced), CONTEXT_SLOT(DrawInstanced), CONTEXT_SLOT(DrawAuto),
    CONTEXT_SLOT(DrawIndexedInstancedIndirect), CONTEXT_SLOT(DrawInstancedIndirect), CONTEXT_SLOT(CSSetShader),
    CONTEXT_SLOT(PSSetShaderResources), CONTEXT_SLOT(CSSetShaderResources), CONTEXT_SLOT(CSSetUnorderedAccessViews),
    CONTEXT_SLOT(ClearRenderTargetView), CONTEXT_SLOT(ClearDepthStencilView), CONTEXT_SLOT(RSSetViewports),
    CONTEXT_SLOT(Dispatch), CONTEXT_SLOT(DispatchIndirect), CONTEXT_SLOT(CopyResource),
    CONTEXT_SLOT(CopySubresourceRegion), CONTEXT_SLOT(ResolveSubresource), CONTEXT_SLOT(OMSetDepthStencilState),
    CONTEXT_SLOT(RSSetState), CONTEXT_SLOT(VSSetShaderResources), CONTEXT_SLOT(PSSetSamplers),
    CONTEXT_SLOT(VSSetSamplers), CONTEXT_SLOT(CSSetSamplers), CONTEXT_SLOT(GetData), CONTEXT_SLOT(Map),
    CONTEXT_SLOT(Flush)};
static const char *const kContextNames[CTX_COUNT] = {
    "VSSetShader", "GSSetShader", "PSSetShader", "IASetInputLayout", "IASetPrimitiveTopology",
    "IASetVertexBuffers", "OMSetRenderTargets", "OMSetRenderTargetsAndUnorderedAccessViews", "OMSetBlendState",
    "Draw", "DrawIndexed", "DrawIndexedInstanced", "DrawInstanced", "DrawAuto", "DrawIndexedInstancedIndirect",
    "DrawInstancedIndirect", "CSSetShader", "PSSetShaderResources", "CSSetShaderResources",
    "CSSetUnorderedAccessViews", "ClearRenderTargetView", "ClearDepthStencilView", "RSSetViewports", "Dispatch",
    "DispatchIndirect", "CopyResource", "CopySubresourceRegion", "ResolveSubresource", "OMSetDepthStencilState",
    "RSSetState", "VSSetShaderResources", "PSSetSamplers", "VSSetSamplers", "CSSetSamplers", "GetData", "Map",
    "Flush"};
static void *orig_context[CTX_COUNT];

/* Reads the formats of the bound views. A view's format can differ from its
 * texture's format, and the view's format is the one a pipeline must match. */
static void note_targets(UINT count, void *const *rtvs, void *dsv) {
  bound.rt_count = count < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT ? count : D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
  bound.multisample_mask = 0;
  for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) {
    bound.rt_formats[i] = 0;
    if (i < bound.rt_count && rtvs && rtvs[i]) {
      D3D11_RENDER_TARGET_VIEW_DESC desc;
      void **vt = *(void ***)rtvs[i];
      ((rtv_desc_fn)vt[offsetof(ID3D11RenderTargetViewVtbl, GetDesc) / sizeof(void *)])(rtvs[i], &desc);
      bound.rt_formats[i] = desc.Format;
      if (desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMS ||
          desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY)
        bound.multisample_mask |= 1u << i;
    }
  }
  bound.dsv_format = 0;
  if (dsv) {
    D3D11_DEPTH_STENCIL_VIEW_DESC desc;
    void **vt = *(void ***)dsv;
    ((dsv_desc_fn)vt[offsetof(ID3D11DepthStencilViewVtbl, GetDesc) / sizeof(void *)])(dsv, &desc);
    bound.dsv_format = desc.Format;
    if (desc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DMS ||
        desc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY)
      bound.multisample_mask |= 1u << 8;
  }
}

/* With SC6VRR_SKIP_GS_DRAWS=1, draws issued while a geometry shader is bound
 * are dropped. DXMT runs those draws as Metal mesh pipelines, and Xcode cannot
 * profile a capture that contains mesh pipelines. Dropping them changes the
 * image, so use this only for GPU profiling captures. */
static int cfg_skip_gs_draws;
static uint64_t gs_draws_skipped;

static int skip_gs_draw(void) {
  if (!cfg_skip_gs_draws || bound.gs == ID_NONE)
    return 0;
  if (++gs_draws_skipped == 1)
    log_msg("shaders: skipping draws with a geometry shader bound (SC6VRR_SKIP_GS_DRAWS=1)");
  if (FRAME_LOGGING_NOW)
    frame_line("SkippedDraw\treason=geometry shader bound");
  return 1;
}

static void WINAPI hook_vs_set(void *ctx, void *shader, void *const *instances, UINT count) {
  bound.vs = object_lookup(shader, NULL);
  ((set_shader_fn)orig_context[CTX_VS_SET])(ctx, shader, instances, count);
}
static void WINAPI hook_gs_set(void *ctx, void *shader, void *const *instances, UINT count) {
  bound.gs = object_lookup(shader, NULL);
  ((set_shader_fn)orig_context[CTX_GS_SET])(ctx, shader, instances, count);
}
static void WINAPI hook_ps_set(void *ctx, void *shader, void *const *instances, UINT count) {
  bound.ps = object_lookup(shader, NULL);
  ((set_shader_fn)orig_context[CTX_PS_SET])(ctx, shader, instances, count);
}
static void WINAPI hook_layout_set(void *ctx, void *layout) {
  bound.layout = object_lookup(layout, &bound_layout_slots);
  ((set_layout_fn)orig_context[CTX_LAYOUT])(ctx, layout);
}
static void WINAPI hook_topology_set(void *ctx, UINT topology) {
  bound.topology = topology;
  ((set_topology_fn)orig_context[CTX_TOPOLOGY])(ctx, topology);
}
static void WINAPI hook_vertex_buffers_set(void *ctx, UINT start, UINT count, void *const *buffers,
                                           const UINT *strides, const UINT *offsets) {
  for (UINT i = 0; i < count && start + i < STRIDE_SLOTS; i++)
    bound_strides[start + i] = buffers && buffers[i] && strides ? strides[i] : 0;
  ((set_vertex_buffers_fn)orig_context[CTX_VERTEX_BUFFERS])(ctx, start, count, buffers, strides, offsets);
}
static void log_targets(const char *call, UINT count, void *const *rtvs, void *dsv) {
  char line[2048], one[160];
  size_t used = (size_t)snprintf(line, sizeof(line), "%s\tcount=%u", call, count);
  for (UINT i = 0; rtvs && i < count && i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT && used < sizeof(line); i++) {
    describe_view(rtvs[i], one, sizeof(one));
    used += (size_t)snprintf(line + used, sizeof(line) - used, "\trt%u=%s", i, one);
  }
  describe_view(dsv, one, sizeof(one));
  if (used < sizeof(line))
    snprintf(line + used, sizeof(line) - used, "\tdepth=%s", one);
  frame_line("%s", line);
}

static void WINAPI hook_targets_set(void *ctx, UINT count, void *const *rtvs, void *dsv) {
  note_targets(count, rtvs, dsv);
  if (FRAME_LOGGING_NOW)
    log_targets("OMSetRenderTargets", count, rtvs, dsv);
  ((set_targets_fn)orig_context[CTX_TARGETS])(ctx, count, rtvs, dsv);
}
static void WINAPI hook_targets_uavs_set(void *ctx, UINT count, void *const *rtvs, void *dsv, UINT uav_start,
                                         UINT uav_count, void *const *uavs, const UINT *initial_counts) {
  if (count != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
    note_targets(count, rtvs, dsv);
    if (FRAME_LOGGING_NOW)
      log_targets("OMSetRenderTargetsAndUnorderedAccessViews", count, rtvs, dsv);
  }
  if (FRAME_LOGGING_NOW && uav_count != D3D11_KEEP_UNORDERED_ACCESS_VIEWS)
    describe_views("OMSetUnorderedAccessViews", uav_start, uav_count, uavs);
  ((set_targets_uavs_fn)orig_context[CTX_TARGETS_UAVS])(ctx, count, rtvs, dsv, uav_start, uav_count, uavs,
                                                        initial_counts);
}
static void WINAPI hook_blend_set(void *ctx, void *state, const FLOAT factor[4], UINT sample_mask) {
  bound.blend = object_lookup(state, NULL);
  ((set_blend_fn)orig_context[CTX_BLEND])(ctx, state, factor, sample_mask);
}
static void WINAPI hook_draw(void *ctx, UINT count, UINT start) {
  note_draw();
  if (FRAME_LOGGING_NOW) {
    char args[96];
    snprintf(args, sizeof(args), "count=%u start=%u", count, start);
    frame_draw("Draw", args);
  }
  if (skip_gs_draw())
    return;
  ((draw_fn)orig_context[CTX_DRAW])(ctx, count, start);
}
static void WINAPI hook_draw_indexed(void *ctx, UINT count, UINT start, INT base) {
  note_draw();
  if (FRAME_LOGGING_NOW) {
    char args[96];
    snprintf(args, sizeof(args), "count=%u start=%u base=%d", count, start, base);
    frame_draw("DrawIndexed", args);
  }
  if (skip_gs_draw())
    return;
  ((draw_indexed_fn)orig_context[CTX_DRAW_INDEXED])(ctx, count, start, base);
}
static void WINAPI hook_draw_indexed_instanced(void *ctx, UINT count, UINT instances, UINT start, INT base,
                                               UINT start_instance) {
  note_draw();
  if (FRAME_LOGGING_NOW) {
    char args[96];
    snprintf(args, sizeof(args), "count=%u instances=%u start=%u base=%d", count, instances, start, base);
    frame_draw("DrawIndexedInstanced", args);
  }
  if (skip_gs_draw())
    return;
  ((draw_indexed_instanced_fn)orig_context[CTX_DRAW_INDEXED_INSTANCED])(ctx, count, instances, start, base,
                                                                        start_instance);
}
static void WINAPI hook_draw_instanced(void *ctx, UINT count, UINT instances, UINT start, UINT start_instance) {
  note_draw();
  if (FRAME_LOGGING_NOW) {
    char args[96];
    snprintf(args, sizeof(args), "count=%u instances=%u start=%u", count, instances, start);
    frame_draw("DrawInstanced", args);
  }
  if (skip_gs_draw())
    return;
  ((draw_instanced_fn)orig_context[CTX_DRAW_INSTANCED])(ctx, count, instances, start, start_instance);
}
static void WINAPI hook_draw_auto(void *ctx) {
  note_draw();
  if (FRAME_LOGGING_NOW) {
    char args[96];
    snprintf(args, sizeof(args), "-");
    frame_draw("DrawAuto", args);
  }
  if (skip_gs_draw())
    return;
  ((draw_auto_fn)orig_context[CTX_DRAW_AUTO])(ctx);
}
static void WINAPI hook_draw_indexed_indirect(void *ctx, void *args, UINT offset) {
  note_draw();
  if (FRAME_LOGGING_NOW) {
    char args[96];
    snprintf(args, sizeof(args), "offset=%u", offset);
    frame_draw("DrawIndexedInstancedIndirect", args);
  }
  if (skip_gs_draw())
    return;
  ((draw_indirect_fn)orig_context[CTX_DRAW_INDEXED_INDIRECT])(ctx, args, offset);
}
static void WINAPI hook_draw_instanced_indirect(void *ctx, void *args, UINT offset) {
  note_draw();
  if (FRAME_LOGGING_NOW) {
    char args[96];
    snprintf(args, sizeof(args), "offset=%u", offset);
    frame_draw("DrawInstancedIndirect", args);
  }
  if (skip_gs_draw())
    return;
  ((draw_indirect_fn)orig_context[CTX_DRAW_INSTANCED_INDIRECT])(ctx, args, offset);
}


/* Calls that only the frame log needs. They pass straight through when no
 * frame is being logged, except CSSetShader, which tracks the compute shader. */
typedef void(WINAPI *set_views_fn)(void *ctx, UINT start, UINT count, void *const *views);
typedef void(WINAPI *set_uavs_fn)(void *ctx, UINT start, UINT count, void *const *uavs, const UINT *counts);
typedef void(WINAPI *clear_rtv_fn)(void *ctx, void *rtv, const FLOAT color[4]);
typedef void(WINAPI *clear_dsv_fn)(void *ctx, void *dsv, UINT flags, FLOAT depth, UINT8 stencil);
typedef void(WINAPI *set_viewports_fn)(void *ctx, UINT count, const D3D11_VIEWPORT *viewports);
typedef void(WINAPI *dispatch_fn)(void *ctx, UINT x, UINT y, UINT z);
typedef void(WINAPI *copy_resource_fn)(void *ctx, void *dst, void *src);
typedef void(WINAPI *copy_region_fn)(void *ctx, void *dst, UINT dst_sub, UINT x, UINT y, UINT z, void *src,
                                     UINT src_sub, const D3D11_BOX *box);
typedef void(WINAPI *resolve_fn)(void *ctx, void *dst, UINT dst_sub, void *src, UINT src_sub, DXGI_FORMAT format);

static void WINAPI hook_cs_set(void *ctx, void *shader, void *const *instances, UINT count) {
  bound_cs = object_lookup(shader, NULL);
  ((set_shader_fn)orig_context[CTX_CS_SET])(ctx, shader, instances, count);
}
static void WINAPI hook_ps_srv_set(void *ctx, UINT start, UINT count, void *const *views) {
  if (FRAME_LOGGING_NOW)
    describe_views("PSSetShaderResources", start, count, views);
  ((set_views_fn)orig_context[CTX_PS_SRV])(ctx, start, count, views);
}
static void WINAPI hook_cs_srv_set(void *ctx, UINT start, UINT count, void *const *views) {
  if (FRAME_LOGGING_NOW)
    describe_views("CSSetShaderResources", start, count, views);
  ((set_views_fn)orig_context[CTX_CS_SRV])(ctx, start, count, views);
}
static void WINAPI hook_cs_uav_set(void *ctx, UINT start, UINT count, void *const *uavs, const UINT *counts) {
  if (FRAME_LOGGING_NOW)
    describe_views("CSSetUnorderedAccessViews", start, count, uavs);
  ((set_uavs_fn)orig_context[CTX_CS_UAV])(ctx, start, count, uavs, counts);
}
static void WINAPI hook_clear_rtv(void *ctx, void *rtv, const FLOAT color[4]) {
  if (FRAME_LOGGING_NOW) {
    char view[160];
    describe_view(rtv, view, sizeof(view));
    frame_line("ClearRenderTargetView\t%s\tcolor=%g,%g,%g,%g", view, color[0], color[1], color[2], color[3]);
  }
  ((clear_rtv_fn)orig_context[CTX_CLEAR_RTV])(ctx, rtv, color);
}
static void WINAPI hook_clear_dsv(void *ctx, void *dsv, UINT flags, FLOAT depth, UINT8 stencil) {
  if (FRAME_LOGGING_NOW) {
    char view[160];
    describe_view(dsv, view, sizeof(view));
    frame_line("ClearDepthStencilView\t%s\tflags=%u\tdepth=%g\tstencil=%u", view, flags, depth, stencil);
  }
  ((clear_dsv_fn)orig_context[CTX_CLEAR_DSV])(ctx, dsv, flags, depth, stencil);
}
static void WINAPI hook_viewports_set(void *ctx, UINT count, const D3D11_VIEWPORT *v) {
  if (FRAME_LOGGING_NOW && count && v)
    frame_line("RSSetViewports\tcount=%u\t0=%g,%g,%gx%g,depth=%g-%g", count, v[0].TopLeftX, v[0].TopLeftY, v[0].Width,
               v[0].Height, v[0].MinDepth, v[0].MaxDepth);
  ((set_viewports_fn)orig_context[CTX_VIEWPORTS])(ctx, count, v);
}
static void WINAPI hook_dispatch(void *ctx, UINT x, UINT y, UINT z) {
  if (FRAME_LOGGING_NOW) {
    char cs[65];
    short_id(bound_cs, cs);
    frame_line("Dispatch\tgroups=%u,%u,%u\tcs=%s", x, y, z, cs);
  }
  ((dispatch_fn)orig_context[CTX_DISPATCH])(ctx, x, y, z);
}
static void WINAPI hook_dispatch_indirect(void *ctx, void *args, UINT offset) {
  if (FRAME_LOGGING_NOW) {
    char cs[65];
    short_id(bound_cs, cs);
    frame_line("DispatchIndirect\toffset=%u\tcs=%s", offset, cs);
  }
  ((draw_indirect_fn)orig_context[CTX_DISPATCH_INDIRECT])(ctx, args, offset);
}
static void WINAPI hook_copy_resource(void *ctx, void *dst, void *src) {
  if (FRAME_LOGGING_NOW) {
    char d[160], s[160];
    describe_resource(dst, d, sizeof(d));
    describe_resource(src, s, sizeof(s));
    frame_line("CopyResource\tdst=%s\tsrc=%s", d, s);
  }
  ((copy_resource_fn)orig_context[CTX_COPY_RESOURCE])(ctx, dst, src);
}
static void WINAPI hook_copy_region(void *ctx, void *dst, UINT dst_sub, UINT x, UINT y, UINT z, void *src,
                                    UINT src_sub, const D3D11_BOX *box) {
  if (FRAME_LOGGING_NOW) {
    char d[160], s[160];
    describe_resource(dst, d, sizeof(d));
    describe_resource(src, s, sizeof(s));
    frame_line("CopySubresourceRegion\tdst=%s\tdst_sub=%u\tsrc=%s\tsrc_sub=%u", d, dst_sub, s, src_sub);
  }
  ((copy_region_fn)orig_context[CTX_COPY_REGION])(ctx, dst, dst_sub, x, y, z, src, src_sub, box);
}
static void WINAPI hook_resolve(void *ctx, void *dst, UINT dst_sub, void *src, UINT src_sub, DXGI_FORMAT format) {
  if (FRAME_LOGGING_NOW) {
    char d[160], s[160];
    describe_resource(dst, d, sizeof(d));
    describe_resource(src, s, sizeof(s));
    frame_line("ResolveSubresource\tdst=%s\tsrc=%s\tformat=%u", d, s, (unsigned)format);
  }
  ((resolve_fn)orig_context[CTX_RESOLVE])(ctx, dst, dst_sub, src, src_sub, format);
}

typedef void(WINAPI *set_depth_state_fn)(void *ctx, void *state, UINT stencil_ref);
typedef void(WINAPI *set_raster_state_fn)(void *ctx, void *state);
typedef void(WINAPI *set_samplers_fn)(void *ctx, UINT start, UINT count, void *const *samplers);

static void WINAPI hook_depth_state_set(void *ctx, void *state, UINT stencil_ref) {
  bound_depth_state = object_lookup(state, NULL);
  bound_stencil_ref = stencil_ref;
  ((set_depth_state_fn)orig_context[CTX_DEPTH_STATE])(ctx, state, stencil_ref);
}
static void WINAPI hook_raster_state_set(void *ctx, void *state) {
  bound_raster_state = object_lookup(state, NULL);
  ((set_raster_state_fn)orig_context[CTX_RASTER_STATE])(ctx, state);
}
static void WINAPI hook_vs_srv_set(void *ctx, UINT start, UINT count, void *const *views) {
  if (FRAME_LOGGING_NOW)
    describe_views("VSSetShaderResources", start, count, views);
  ((set_views_fn)orig_context[CTX_VS_SRV])(ctx, start, count, views);
}

/* Logs the content IDs of the samplers bound to one stage. */
static void log_samplers(const char *call, UINT start, UINT count, void *const *samplers) {
  char line[2048], id[65];
  size_t used = (size_t)snprintf(line, sizeof(line), "%s\tstart=%u\tcount=%u", call, start, count);
  for (UINT i = 0; samplers && i < count && used < sizeof(line); i++) {
    short_id(object_lookup(samplers[i], NULL), id);
    used += (size_t)snprintf(line + used, sizeof(line) - used, "\t%u=%s", start + i, id);
  }
  frame_line("%s", line);
}
static void WINAPI hook_ps_samplers_set(void *ctx, UINT start, UINT count, void *const *samplers) {
  if (FRAME_LOGGING_NOW)
    log_samplers("PSSetSamplers", start, count, samplers);
  ((set_samplers_fn)orig_context[CTX_PS_SAMPLERS])(ctx, start, count, samplers);
}
static void WINAPI hook_vs_samplers_set(void *ctx, UINT start, UINT count, void *const *samplers) {
  if (FRAME_LOGGING_NOW)
    log_samplers("VSSetSamplers", start, count, samplers);
  ((set_samplers_fn)orig_context[CTX_VS_SAMPLERS])(ctx, start, count, samplers);
}
static void WINAPI hook_cs_samplers_set(void *ctx, UINT start, UINT count, void *const *samplers) {
  if (FRAME_LOGGING_NOW)
    log_samplers("CSSetSamplers", start, count, samplers);
  ((set_samplers_fn)orig_context[CTX_CS_SAMPLERS])(ctx, start, count, samplers);
}

/* Waits on the game's render thread. UE4 reads occlusion query results with a
 * loop that calls GetData until it returns S_OK, and DXMT blocks inside Map
 * until the GPU is done with a staging resource. Either one stops the thread
 * that encodes the next frame. A query wait runs from the first S_FALSE to the
 * S_OK on the same query, and Present ends any open wait, so one counted wait
 * never spans two frames. */
typedef HRESULT(WINAPI *get_data_fn)(void *ctx, void *async, void *data, UINT size, UINT flags);
typedef HRESULT(WINAPI *map_fn)(void *ctx, void *resource, UINT sub, D3D11_MAP type, UINT flags,
                                D3D11_MAPPED_SUBRESOURCE *out);
typedef void(WINAPI *flush_fn)(void *ctx);
/* D3D11_QUERY kinds 0 to 15. ID3D11Predicate and ID3D11Counter keep GetDesc in
 * the same vtable slot, with a two-UINT description whose first field is the
 * kind, so the same call reads them. Other values (counter IDs) count as
 * QUERY_OTHER. */
_Static_assert(offsetof(ID3D11QueryVtbl, GetDesc) / sizeof(void *) == 8, "unexpected ID3D11Query layout");
typedef void(WINAPI *query_desc_fn)(void *query, D3D11_QUERY_DESC *desc);
enum { QUERY_KINDS = 16, QUERY_OTHER = QUERY_KINDS };
static const char *const kQueryKindNames[QUERY_KINDS + 1] = {
    "event", "occlusion", "timestamp", "timestamp-disjoint", "pipeline-statistics", "occlusion-predicate",
    "so-statistics", "so-overflow-predicate", "so-statistics-0", "so-overflow-0", "so-statistics-1",
    "so-overflow-1", "so-statistics-2", "so-overflow-2", "so-statistics-3", "so-overflow-3", "other"};
static struct {
  uint64_t frames, polls, not_ready, query_waits, maps, flushes;
  double query_ms, query_max_ms, map_ms, map_max_ms;
  uint64_t kind_waits[QUERY_KINDS + 1];
  double kind_ms[QUERY_KINDS + 1];
} waits;

static unsigned query_kind(void *query) {
  D3D11_QUERY_DESC desc = {0};
  ((query_desc_fn)(*(void ***)query)[offsetof(ID3D11QueryVtbl, GetDesc) / sizeof(void *)])(query, &desc);
  return (unsigned)desc.Query < QUERY_KINDS ? (unsigned)desc.Query : QUERY_OTHER;
}
static void *waiting_query;
static double waiting_since;
static unsigned waiting_polls;
#define WAIT_REPORT_FRAMES 120

static HRESULT WINAPI hook_get_data(void *ctx, void *async, void *data, UINT size, UINT flags) {
  double t0 = qpc_seconds();
  HRESULT hr = ((get_data_fn)orig_context[CTX_GET_DATA])(ctx, async, data, size, flags);
  waits.polls++;
  if (hr == S_FALSE) {
    waits.not_ready++;
    if (async != waiting_query)
      waiting_query = async, waiting_since = t0, waiting_polls = 0;
    waiting_polls++;
  } else if (async == waiting_query) {
    double ms = (qpc_seconds() - waiting_since) * 1000.0;
    waits.query_waits++;
    waits.query_ms += ms;
    if (ms > waits.query_max_ms) waits.query_max_ms = ms;
    unsigned kind = query_kind(async);
    waits.kind_waits[kind]++;
    waits.kind_ms[kind] += ms;
    if (FRAME_LOGGING_NOW)
      frame_line("QueryWait\tkind=%s\tms=%.3f\tpolls=%u\tflags=0x%x\tsize=%u", kQueryKindNames[kind], ms,
                 waiting_polls + 1, flags, size);
    waiting_query = NULL;
  }
  return hr;
}
static HRESULT WINAPI hook_map(void *ctx, void *resource, UINT sub, D3D11_MAP type, UINT flags,
                               D3D11_MAPPED_SUBRESOURCE *out) {
  double t0 = qpc_seconds();
  HRESULT hr = ((map_fn)orig_context[CTX_MAP])(ctx, resource, sub, type, flags, out);
  double ms = (qpc_seconds() - t0) * 1000.0;
  waits.maps++;
  waits.map_ms += ms;
  if (ms > waits.map_max_ms) waits.map_max_ms = ms;
  if (FRAME_LOGGING_NOW) {
    char r[160];
    describe_resource(resource, r, sizeof(r));
    frame_line("Map\tresource=%s\tsub=%u\ttype=%u\tflags=0x%x\tms=%.3f\thr=0x%08lx", r, sub, (unsigned)type, flags,
               ms, (unsigned long)hr);
  }
  return hr;
}
static void WINAPI hook_flush(void *ctx) {
  waits.flushes++;
  if (FRAME_LOGGING_NOW)
    frame_line("Flush");
  ((flush_fn)orig_context[CTX_FLUSH])(ctx);
}

/* Called at every Present: ends an open query wait and logs averages. */
static void waits_present(void) {
  waiting_query = NULL;
  if (++waits.frames < WAIT_REPORT_FRAMES)
    return;
  double n = (double)waits.frames;
  char kinds[512] = "";
  size_t used = 0;
  for (int k = 0; k <= QUERY_KINDS && used < sizeof(kinds); k++)
    if (waits.kind_waits[k])
      used += (size_t)snprintf(kinds + used, sizeof(kinds) - used, "%s%s %.2f waits %.3f ms", used ? ", " : "",
                               kQueryKindNames[k], waits.kind_waits[k] / n, waits.kind_ms[k] / n);
  log_msg("waits: per frame over %llu frames: query waits %.3f ms (%.2f waits, longest %.3f ms; by kind: %s), "
          "GetData %.1f calls (%.1f not ready), Map %.3f ms (%.1f calls, longest %.3f ms), Flush %.2f calls",
          (unsigned long long)waits.frames, waits.query_ms / n, waits.query_waits / n, waits.query_max_ms,
          used ? kinds : "none", waits.polls / n, waits.not_ready / n, waits.map_ms / n, waits.maps / n,
          waits.map_max_ms, waits.flushes / n);
  memset(&waits, 0, sizeof(waits));
}

/* Replaces vtable pointers instead of patching the methods' code. Under
 * CrossOver Preview with FEX, d3d11.dll is ARM64EC: its methods are native
 * ARM64 code, so an x64 jump written over them is an illegal instruction
 * (crash 0xC000001D at the hooked CreatePixelShader, 2026-10-16). The game's
 * x64 code reads the pointer from the vtable, so swapping the pointer works
 * for x64 and ARM64EC implementations alike. */
static int swap_vtable_slots(void **vtable, const int *slots, void *const *hooks, void **originals, int count,
                             const char *const *names) {
  int low = slots[0], high = slots[0];
  for (int i = 1; i < count; i++) {
    if (slots[i] < low) low = slots[i];
    if (slots[i] > high) high = slots[i];
  }
  DWORD old;
  if (!VirtualProtect(&vtable[low], (SIZE_T)(high - low + 1) * sizeof(void *), PAGE_READWRITE, &old)) {
    log_msg("shaders: cannot unprotect the vtable at %p (%lu)", (void *)vtable, GetLastError());
    return 0;
  }
  for (int i = 0; i < count; i++) {
    originals[i] = vtable[slots[i]];
    log_msg("shaders: vtable slot %d (%s) %p -> hook", slots[i], names[i], originals[i]);
    vtable[slots[i]] = hooks[i]; /* an aligned pointer store, so callers see old or new */
  }
  VirtualProtect(&vtable[low], (SIZE_T)(high - low + 1) * sizeof(void *), old, &old);
  return 1;
}

/* The frame log needs the end of each frame, which is IDXGISwapChain::Present.
 * The exe imports CreateDXGIFactory and CreateDXGIFactory1 from dxgi.dll, so
 * those exports are hooked like the d3d11 ones. The factory's CreateSwapChain
 * and the swap chain's Present are then hooked through their vtables. */
_Static_assert(offsetof(IDXGIFactoryVtbl, CreateSwapChain) / sizeof(void *) == 10, "unexpected IDXGIFactory layout");
_Static_assert(offsetof(IDXGISwapChainVtbl, Present) / sizeof(void *) == 8, "unexpected IDXGISwapChain layout");

typedef HRESULT(WINAPI *create_factory_fn)(REFIID riid, void **factory);
typedef HRESULT(WINAPI *create_swapchain_fn)(void *factory, void *device, DXGI_SWAP_CHAIN_DESC *desc,
                                             void **swapchain);
typedef HRESULT(WINAPI *present_fn)(void *swapchain, UINT sync_interval, UINT flags);
static create_factory_fn orig_create_factory, orig_create_factory1;
static create_swapchain_fn orig_create_swapchain;
static present_fn orig_present;
static volatile LONG factory_hooked, present_hooked;

static HRESULT WINAPI hook_present(void *swapchain, UINT sync_interval, UINT flags) {
  frame_present();
  waits_present();
  return orig_present(swapchain, sync_interval, flags);
}

static void hook_swapchain(void *swapchain) {
  if (!swapchain || InterlockedCompareExchange(&present_hooked, 1, 0))
    return;
  static const int slots[1] = {offsetof(IDXGISwapChainVtbl, Present) / sizeof(void *)};
  static const char *const names[1] = {"Present"};
  void *hooks[1] = {hook_present}, *originals[1];
  if (swap_vtable_slots(*(void ***)swapchain, slots, hooks, originals, 1, names))
    orig_present = (present_fn)originals[0];
}

static HRESULT WINAPI hook_create_swapchain(void *factory, void *device, DXGI_SWAP_CHAIN_DESC *desc,
                                            void **swapchain) {
  HRESULT hr = orig_create_swapchain(factory, device, desc, swapchain);
  if (SUCCEEDED(hr) && swapchain)
    hook_swapchain(*swapchain);
  return hr;
}

static void hook_factory(void *factory) {
  if (!factory || InterlockedCompareExchange(&factory_hooked, 1, 0))
    return;
  static const int slots[1] = {offsetof(IDXGIFactoryVtbl, CreateSwapChain) / sizeof(void *)};
  static const char *const names[1] = {"CreateSwapChain"};
  void *hooks[1] = {hook_create_swapchain}, *originals[1];
  if (swap_vtable_slots(*(void ***)factory, slots, hooks, originals, 1, names))
    orig_create_swapchain = (create_swapchain_fn)originals[0];
}

static HRESULT WINAPI hook_create_factory(REFIID riid, void **factory) {
  HRESULT hr = orig_create_factory(riid, factory);
  if (SUCCEEDED(hr) && factory)
    hook_factory(*factory);
  return hr;
}

static HRESULT WINAPI hook_create_factory1(REFIID riid, void **factory) {
  HRESULT hr = orig_create_factory1(riid, factory);
  if (SUCCEEDED(hr) && factory)
    hook_factory(*factory);
  return hr;
}

/* Takes the two export addresses as arguments so that the test can pass fakes. */
static int install_frame_log(void *create_factory, void *create_factory1) {
  int hooked = 0;
  if (create_factory && MH_CreateHook(create_factory, hook_create_factory, (void **)&orig_create_factory) == MH_OK &&
      MH_EnableHook(create_factory) == MH_OK)
    hooked++;
  if (create_factory1 &&
      MH_CreateHook(create_factory1, hook_create_factory1, (void **)&orig_create_factory1) == MH_OK &&
      MH_EnableHook(create_factory1) == MH_OK)
    hooked++;
  log_msg("frames: %d factory creation hooks installed; press F10 in the game to log one frame", hooked);
  return hooked > 0;
}

typedef void(WINAPI *get_context_fn)(void *device, void **context);
typedef ULONG(WINAPI *release_fn)(void *object);

enum { CREATE_IL = STAGE_COUNT, CREATE_BLEND, CREATE_DEPTH, CREATE_RASTER, CREATE_SAMPLER, CREATE_COUNT };

static void install_device_shader_hooks(void *device) {
  if (!device || InterlockedCompareExchange(&shader_hooks_installed, 1, 0))
    return;
  void **vtable = *(void ***)device;
  void *create_hooks[CREATE_COUNT] = {hook_create_VS, hook_create_GS, hook_create_GS_SO,       hook_create_PS,
                                      hook_create_HS, hook_create_DS, hook_create_CS,          hook_create_input_layout,
                                      hook_create_blend_state, hook_create_depth_state, hook_create_raster_state,
                                      hook_create_sampler_state};
  int create_slots[CREATE_COUNT];
  const char *create_names[CREATE_COUNT];
  void *create_originals[CREATE_COUNT];
  for (int i = 0; i < STAGE_COUNT; i++) {
    create_slots[i] = kStageSlots[i];
    create_names[i] = kStageNames[i];
  }
  create_slots[CREATE_IL] = DEVICE_SLOT(CreateInputLayout);
  create_names[CREATE_IL] = "il";
  create_slots[CREATE_BLEND] = DEVICE_SLOT(CreateBlendState);
  create_names[CREATE_BLEND] = "bs";
  create_slots[CREATE_DEPTH] = DEVICE_SLOT(CreateDepthStencilState);
  create_names[CREATE_DEPTH] = "dss";
  create_slots[CREATE_RASTER] = DEVICE_SLOT(CreateRasterizerState);
  create_names[CREATE_RASTER] = "rss";
  create_slots[CREATE_SAMPLER] = DEVICE_SLOT(CreateSamplerState);
  create_names[CREATE_SAMPLER] = "sss";
  if (!swap_vtable_slots(vtable, create_slots, create_hooks, create_originals, CREATE_COUNT, create_names))
    return;
  for (int i = 0; i < STAGE_COUNT; i++) {
    if (i == STAGE_GS_SO)
      orig_create_gs_so = (create_gs_so_fn)create_originals[i];
    else
      orig_create_shader[i] = (create_shader_fn)create_originals[i];
  }
  orig_create_input_layout = (create_input_layout_fn)create_originals[CREATE_IL];
  orig_create_blend_state = (create_blend_state_fn)create_originals[CREATE_BLEND];
  orig_create_depth_state = (create_depth_state_fn)create_originals[CREATE_DEPTH];
  orig_create_raster_state = (create_raster_state_fn)create_originals[CREATE_RASTER];
  orig_create_sampler_state = (create_sampler_state_fn)create_originals[CREATE_SAMPLER];

  /* GetImmediateContext adds a reference, so release it after hooking. */
  void *context = NULL;
  ((get_context_fn)vtable[DEVICE_SLOT(GetImmediateContext)])(device, &context);
  if (!context) {
    log_msg("shaders: no immediate context; bindings are not recorded");
    return;
  }
  void **context_vtable = *(void ***)context;
  void *context_hooks[CTX_COUNT] = {
      hook_vs_set,      hook_gs_set,         hook_ps_set,       hook_layout_set,
      hook_topology_set, hook_vertex_buffers_set, hook_targets_set, hook_targets_uavs_set,
      hook_blend_set,   hook_draw,           hook_draw_indexed, hook_draw_indexed_instanced,
      hook_draw_instanced, hook_draw_auto,   hook_draw_indexed_indirect, hook_draw_instanced_indirect,
      hook_cs_set,      hook_ps_srv_set,     hook_cs_srv_set,   hook_cs_uav_set,
      hook_clear_rtv,   hook_clear_dsv,      hook_viewports_set, hook_dispatch,
      hook_dispatch_indirect, hook_copy_resource, hook_copy_region, hook_resolve,
      hook_depth_state_set, hook_raster_state_set, hook_vs_srv_set, hook_ps_samplers_set,
      hook_vs_samplers_set, hook_cs_samplers_set, hook_get_data, hook_map, hook_flush};
  swap_vtable_slots(context_vtable, kContextSlots, context_hooks, orig_context, CTX_COUNT, kContextNames);
  ((release_fn)context_vtable[2])(context);
}

typedef HRESULT(WINAPI *create_device_fn)(void *adapter, int driver_type, HMODULE software, UINT flags,
                                          const void *levels, UINT level_count, UINT sdk_version,
                                          void **device, void *level, void **context);
typedef HRESULT(WINAPI *create_device_swapchain_fn)(void *adapter, int driver_type, HMODULE software,
                                                    UINT flags, const void *levels, UINT level_count,
                                                    UINT sdk_version, const void *desc, void **swapchain,
                                                    void **device, void *level, void **context);
static create_device_fn orig_create_device;
static create_device_swapchain_fn orig_create_device_swapchain;

static HRESULT WINAPI hook_create_device(void *adapter, int driver_type, HMODULE software, UINT flags,
                                         const void *levels, UINT level_count, UINT sdk_version, void **device,
                                         void *level, void **context) {
  HRESULT hr = orig_create_device(adapter, driver_type, software, flags, levels, level_count, sdk_version,
                                  device, level, context);
  if (SUCCEEDED(hr) && device && *device)
    install_device_shader_hooks(*device);
  return hr;
}

static HRESULT WINAPI hook_create_device_swapchain(void *adapter, int driver_type, HMODULE software,
                                                   UINT flags, const void *levels, UINT level_count,
                                                   UINT sdk_version, const void *desc, void **swapchain,
                                                   void **device, void *level, void **context) {
  HRESULT hr = orig_create_device_swapchain(adapter, driver_type, software, flags, levels, level_count,
                                            sdk_version, desc, swapchain, device, level, context);
  if (SUCCEEDED(hr) && device && *device)
    install_device_shader_hooks(*device);
  if (SUCCEEDED(hr) && swapchain)
    hook_swapchain(*swapchain);
  return hr;
}

/* Takes the two creation functions as arguments so that the test can pass fakes. */
static int install_shader_capture(void *create_device, void *create_device_swapchain) {
  int hooked = 0;
  if (create_device && MH_CreateHook(create_device, hook_create_device, (void **)&orig_create_device) == MH_OK &&
      MH_EnableHook(create_device) == MH_OK)
    hooked++;
  if (create_device_swapchain &&
      MH_CreateHook(create_device_swapchain, hook_create_device_swapchain,
                    (void **)&orig_create_device_swapchain) == MH_OK &&
      MH_EnableHook(create_device_swapchain) == MH_OK)
    hooked++;
  log_msg("shaders: %d device creation hooks installed", hooked);
  return hooked > 0;
}

/* ---------- setup -------------------------------------------------------- */

static void setup(void) {
  open_log();
  cfg_uncap = env_int("SC6VRR_UNCAP", 0);
  cfg_gate = env_int("SC6VRR_GATE", 0);
  cfg_measure = env_int("SC6VRR_MEASURE", 0);
  cfg_frame_timers = env_int("SC6VRR_FRAME_TIMERS", 0);
  cfg_thread_names = env_int("SC6VRR_THREAD_NAMES", 0);
  cfg_max_fps = (float)env_int("SC6VRR_MAX_FPS", 120);
  log_msg("sc6vrr: uncap=%d gate=%d max_fps=%.0f measure=%d", cfg_uncap, cfg_gate, cfg_max_fps, cfg_measure);

  if (!exe_hash_matches()) {
    log_msg("sc6vrr: unknown exe build; no hooks installed");
    return;
  }
  if (cfg_thread_names)
    install_thread_name_observer();
  if (MH_Initialize() != MH_OK) {
    log_msg("sc6vrr: MH_Initialize failed");
    return;
  }
  /* Shader capture hooks only d3d11.dll exports, so it does not depend on the
   * engine prologue checks below and enables its own hooks. */
  char dir[MAX_PATH];
  DWORD dir_len = GetEnvironmentVariableA("SC6VRR_SHADER_DIR", dir, sizeof(dir));
  cfg_skip_gs_draws = env_int("SC6VRR_SKIP_GS_DRAWS", 0);
  if (dir_len && dir_len < sizeof(dir)) {
    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll"), dxgi = NULL;
    if (!d3d11 || !shader_capture_open(dir) ||
        !install_shader_capture((void *)GetProcAddress(d3d11, "D3D11CreateDevice"),
                                (void *)GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain")))
      log_msg("shaders: capture disabled (d3d11 %p, directory %s)", (void *)d3d11, dir);
    else if (!(dxgi = GetModuleHandleW(L"dxgi.dll")) ||
             !install_frame_log((void *)GetProcAddress(dxgi, "CreateDXGIFactory"),
                                (void *)GetProcAddress(dxgi, "CreateDXGIFactory1")))
      log_msg("frames: frame log disabled (dxgi %p)", (void *)dxgi);
  }
  /* Frame-rate changes are switched off for now (2026-10-16). The engine hooks
   * below go in only with SC6VRR_FRAME_MODS=1, whatever the other settings say. */
  if (!env_int("SC6VRR_FRAME_MODS", 0)) {
    log_msg("sc6vrr: frame-rate modifications disabled; set SC6VRR_FRAME_MODS=1 to allow them");
    return;
  }
  if (!bytes_match(ADDR(ADDR_GET_MAX_TICK_RATE), "40534883ec500f2974244048") ||
      !bytes_match(ADDR(ADDR_UPDATE_TIME), "40555357488d6c24b94881ec")) {
    log_msg("sc6vrr: engine function prologue mismatch; no hooks installed");
    return;
  }
  MH_CreateHook(ADDR(ADDR_GET_MAX_TICK_RATE), hook_get_max_tick_rate, (void **)&orig_get_max_tick_rate);
  MH_CreateHook(ADDR(ADDR_UPDATE_TIME), hook_update_time, (void **)&orig_update_time);
  if (cfg_measure) {
    if (!bytes_match(ADDR(ADDR_WORLD_TICK), "488bc455535657415441554156415748") ||
        !bytes_match(ADDR(ADDR_BATTLE_COUNTER_INC), "ff05720d4304")) {
      log_msg("measurement: world prologue or completed-counter INC mismatch; measurement disabled");
      cfg_measure = 0;
    } else {
      MH_STATUS st = MH_CreateHook(ADDR(ADDR_WORLD_TICK), hook_world_tick, (void **)&orig_world_tick);
      if (st != MH_OK) {
        log_msg("measurement: world hook creation failed (%d); measurement disabled", st);
        cfg_measure = 0;
      }
    }
  }
  if ((cfg_gate || cfg_measure) && !install_gates()) {
    log_msg("sc6vrr: gating and measurement disabled");
    cfg_gate = 0;
    cfg_measure = 0;
  }
  if (cfg_frame_timers && (!cfg_measure || !install_frame_timers())) {
    cfg_frame_timers = 0;
    log_msg("frame timers: disabled; baseline measurement remains available");
  }
  MH_STATUS st = MH_EnableHook(MH_ALL_HOOKS);
  log_msg("sc6vrr: hooks enabled (%d)", st);
}

/* ---------- dinput8 proxy ------------------------------------------------ */

static HMODULE real_dinput8;

static FARPROC real_proc(const char *name) {
  if (!real_dinput8) {
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat(path, L"\\dinput8.dll");
    real_dinput8 = LoadLibraryW(path);
  }
  return real_dinput8 ? GetProcAddress(real_dinput8, name) : NULL;
}

HRESULT WINAPI proxy_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID *out, LPUNKNOWN outer) {
  typedef HRESULT(WINAPI * fn)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
  fn f = (fn)real_proc("DirectInput8Create");
  return f ? f(inst, ver, riid, out, outer) : E_FAIL;
}
HRESULT WINAPI proxy_DllCanUnloadNow(void) {
  typedef HRESULT(WINAPI * fn)(void);
  fn f = (fn)real_proc("DllCanUnloadNow");
  return f ? f() : S_FALSE;
}
HRESULT WINAPI proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID *out) {
  typedef HRESULT(WINAPI * fn)(REFCLSID, REFIID, LPVOID *);
  fn f = (fn)real_proc("DllGetClassObject");
  return f ? f(clsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
}
HRESULT WINAPI proxy_DllRegisterServer(void) {
  typedef HRESULT(WINAPI * fn)(void);
  fn f = (fn)real_proc("DllRegisterServer");
  return f ? f() : E_FAIL;
}
HRESULT WINAPI proxy_DllUnregisterServer(void) {
  typedef HRESULT(WINAPI * fn)(void);
  fn f = (fn)real_proc("DllUnregisterServer");
  return f ? f() : E_FAIL;
}
LPCVOID WINAPI proxy_GetdfDIJoystick(void) {
  typedef LPCVOID(WINAPI * fn)(void);
  fn f = (fn)real_proc("GetdfDIJoystick");
  return f ? f() : NULL;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(inst);
    /* Runs before the game's main code; only the loader thread exists, so
     * patching code here cannot race with game threads. */
    setup();
  } else if (reason == DLL_PROCESS_DETACH && thread_name_observer) {
    RemoveVectoredExceptionHandler(thread_name_observer);
    thread_name_observer = NULL;
  }
  return TRUE;
}
