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
 */
#include <windows.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include "MinHook.h"

/* ---------- configuration and logging ---------------------------------- */

static int cfg_uncap, cfg_gate;
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
  SYSTEMTIME t;
  GetLocalTime(&t);
  fprintf(log_file, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(log_file, fmt, ap);
  va_end(ap);
  fputc('\n', log_file);
  fflush(log_file);
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

/* Clock-increment cave, reached by `call`: push rax; mov rax,&g_skip_battle;
 * cmp byte [rax],0; pop rax; jne done; inc dword [rbx+0x3A4]; done: ret. */
static uint8_t *make_clock_cave(uint8_t *mem) {
  uint8_t *p = mem;
  *p++ = 0x50;
  *p++ = 0x48; *p++ = 0xB8; *(uint64_t *)p = (uint64_t)(uintptr_t)&g_skip_battle; p += 8;
  *p++ = 0x80; *p++ = 0x38; *p++ = 0x00;
  *p++ = 0x58;
  *p++ = 0x75; *p++ = 0x06;
  *p++ = 0xFF; *p++ = 0x83; *p++ = 0xA4; *p++ = 0x03; *p++ = 0x00; *p++ = 0x00;
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
  for (size_t i = 0; i < sizeof(kGates) / sizeof(kGates[0]); i++) {
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
  for (size_t i = 0; i < sizeof(kGates) / sizeof(kGates[0]); i++) {
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
    p = make_clock_cave(p);
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
static get_max_tick_rate_fn orig_get_max_tick_rate;
static update_time_fn orig_update_time;

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
  orig_update_time(self);

  static double last, acc, window_start;
  static unsigned frames, steps;
  const double step = 1.0 / 60.0;
  double now = qpc_seconds();
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
    log_msg("frames=%u steps=%u (%.1f s)", frames, cfg_gate ? steps : frames, now - window_start);
    frames = steps = 0;
    window_start = now;
  }
}

/* ---------- setup -------------------------------------------------------- */

static void setup(void) {
  open_log();
  cfg_uncap = env_int("SC6VRR_UNCAP", 0);
  cfg_gate = env_int("SC6VRR_GATE", 0);
  cfg_max_fps = (float)env_int("SC6VRR_MAX_FPS", 120);
  log_msg("sc6vrr: uncap=%d gate=%d max_fps=%.0f", cfg_uncap, cfg_gate, cfg_max_fps);

  if (!exe_hash_matches()) {
    log_msg("sc6vrr: unknown exe build; no hooks installed");
    return;
  }
  if (MH_Initialize() != MH_OK) {
    log_msg("sc6vrr: MH_Initialize failed");
    return;
  }
  if (!bytes_match(ADDR(ADDR_GET_MAX_TICK_RATE), "40534883ec500f2974244048") ||
      !bytes_match(ADDR(ADDR_UPDATE_TIME), "40555357488d6c24b94881ec")) {
    log_msg("sc6vrr: engine function prologue mismatch; no hooks installed");
    return;
  }
  MH_CreateHook(ADDR(ADDR_GET_MAX_TICK_RATE), hook_get_max_tick_rate, (void **)&orig_get_max_tick_rate);
  MH_CreateHook(ADDR(ADDR_UPDATE_TIME), hook_update_time, (void **)&orig_update_time);
  if (cfg_gate && !install_gates()) {
    log_msg("sc6vrr: gating disabled");
    cfg_gate = 0;
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
  }
  return TRUE;
}
