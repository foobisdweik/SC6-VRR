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
 * Settings come from sc6vrr.ini next to the exe ([sc6vrr] section, same key
 * names), so every launch method (CrossOver GUI, dll/launch.sh, wine) uses one
 * file. An environment variable with the same name takes precedence, which
 * keeps the test scripts that pass `wine --env` reproducible:
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
#include <d3d11_3.h>
#include <stddef.h>
#include <stdint.h>
#include <wctype.h>
#include <intrin.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>
#include "MinHook.h"
#include "measurements.h"

/* ---------- configuration and logging ---------------------------------- */

static int cfg_uncap, cfg_gate, cfg_measure, cfg_frame_timers, cfg_thread_names, cfg_block_query_wait;
static float cfg_max_fps = 120.0f;
static FILE *log_file;

static char ini_path[MAX_PATH]; /* sc6vrr.ini next to the exe; empty if unknown */

static void find_ini(void) {
  DWORD n = GetModuleFileNameA(NULL, ini_path, sizeof(ini_path));
  char *slash = (n && n < sizeof(ini_path)) ? strrchr(ini_path, '\\') : NULL;
  if (slash && (size_t)(slash + 1 - ini_path) + sizeof("sc6vrr.ini") <= sizeof(ini_path))
    strcpy(slash + 1, "sc6vrr.ini");
  else
    ini_path[0] = 0;
}

/* Copies setting `name` into buf and returns its length; 0 means unset or
 * empty. The environment wins over sc6vrr.ini. Windows keeps an inline
 * "; comment" as part of an ini value, so it is cut off here. */
static DWORD cfg_str(const char *name, char *buf, DWORD size) {
  DWORD n = GetEnvironmentVariableA(name, buf, size);
  if (n && n < size)
    return n;
  buf[0] = 0;
  if (!ini_path[0])
    return 0;
  GetPrivateProfileStringA("sc6vrr", name, "", buf, size, ini_path);
  char *semi = strchr(buf, ';');
  if (semi)
    *semi = 0;
  char *start = buf + strspn(buf, " \t");
  size_t len = strlen(start);
  while (len && (start[len - 1] == ' ' || start[len - 1] == '\t'))
    len--;
  memmove(buf, start, len);
  buf[len] = 0;
  return (DWORD)len;
}

static int env_int(const char *name, int fallback) {
  char buf[64];
  return cfg_str(name, buf, sizeof(buf)) ? atoi(buf) : fallback;
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
  DWORD n = cfg_str("SC6VRR_LOG", path, sizeof(path));
  if (n && !_stricmp(path, "none"))
    return; /* no log file; log_msg() then does nothing */
  if (!n) {
    /* Default: sc6vrr.log next to the exe. */
    n = GetModuleFileNameA(NULL, path, sizeof(path));
    char *slash = strrchr(path, '\\');
    if (!slash)
      return;
    strcpy(slash + 1, "sc6vrr.log");
  }
  log_file = fopen(path, "w");
}

static void sample_note_thread(DWORD tid, const char *name); /* instruction-pointer sampler, below */

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
  sample_note_thread(target, name);
  return EXCEPTION_CONTINUE_SEARCH;
}

static void install_thread_name_observer(void) {
  set_thread_description = (set_thread_description_fn)GetProcAddress(
      GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription");
  thread_name_observer = AddVectoredExceptionHandler(1, observe_thread_name);
  log_msg("thread names: observer=%s native_naming=%s; original exception handling is preserved",
          thread_name_observer ? "enabled" : "failed", set_thread_description ? "available" : "unavailable");
}

/* ---------- instruction-pointer sampler ---------------------------------- */

/*
 * SC6VRR_SAMPLE=1 starts a helper thread that, about SC6VRR_SAMPLE_HZ times a
 * second, pauses each watched game thread for a moment, reads its instruction
 * pointer (RIP, the address of the code it is running) and resumes it. The
 * counts per address go to sc6vrr-samples.tsv, so a later script can name the
 * functions that use the most time. This replaces the macOS `sample` tool,
 * which gave invalid Windows stacks (notes/thread-identity.md).
 *
 * While a thread is paused the sampler must not take any lock the thread may
 * hold. So it copies RIP and a slice of the stack, resumes the thread at once,
 * and only then counts. It never allocates and never logs inside that window.
 *
 * Watched threads: the thread that loads this DLL (the game thread), every
 * "RenderThread" and "RHIThread", and with SC6VRR_SAMPLE_ALL=1 also the
 * TaskGraph workers and the async loading thread. The game names its threads
 * with the legacy exception that observe_thread_name() sees.
 */

enum { ROLE_GAME, ROLE_RENDER, ROLE_RHI, ROLE_TASK, ROLE_ASYNC, ROLE_COUNT };
static const char *const kRoleNames[ROLE_COUNT] = {"game", "render", "rhi", "taskgraph", "asyncload"};
/* KIND_RIP counts where the thread was. KIND_CALLER1..3 count the first three
 * return addresses (inside the exe) that the sampler finds on the stack. */
enum { KIND_RIP, KIND_CALLER1, KIND_CALLER2, KIND_CALLER3, KIND_COUNT };
static const char *const kKindNames[KIND_COUNT] = {"rip", "caller1", "caller2", "caller3"};

#define SAMPLE_THREADS 128
#define SAMPLE_SLOTS (1u << 17)
#define SAMPLE_STACK_QWORDS 128
#define SAMPLE_LINES_PER_GROUP 4000

typedef struct {
  volatile DWORD tid;
  int role;
  HANDLE handle;
  int dead;
  const NT_TIB *tib; /* the thread's TIB (start of its TEB); NULL = take RIP only */
} sample_thread;
typedef struct {
  uint64_t key; /* (role * KIND_COUNT + kind) << 56 | address; 0 = empty slot */
  uint32_t count;
} sample_slot;
typedef struct {
  uint64_t base, size;
  char name[40];
} sample_module;

static int cfg_sample, cfg_sample_hz = 250, cfg_sample_all, cfg_sample_dump_secs = 10;
static char sample_path[MAX_PATH];
static sample_thread sample_threads[SAMPLE_THREADS];
static volatile LONG sample_thread_count;
static sample_slot sample_table[SAMPLE_SLOTS];
static sample_slot sample_sorted[SAMPLE_SLOTS];
static size_t sample_used;
static uint64_t sample_overflow, sample_passes, sample_open_fail, sample_suspend_fail, sample_context_fail;
static uint64_t exe_code_lo, exe_code_hi, exe_base_now;
static int sample_exe_ok;
/* Set by code hooks (for example the GameInitialize timer) to clear the
 * counts or save a snapshot, the same as F9 and F8. */
static volatile LONG sample_req_reset, sample_req_snapshot;
static volatile LONG sample_stop;

static void find_exe_code(void) {
  const uint8_t *base = (const uint8_t *)GetModuleHandleW(NULL);
  exe_base_now = (uint64_t)(uintptr_t)base;
  const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return;
  const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE)
    return;
  const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++) {
    if (!(s[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
      continue;
    uint64_t lo = exe_base_now + s[i].VirtualAddress, hi = lo + s[i].Misc.VirtualSize;
    if (!exe_code_lo || lo < exe_code_lo)
      exe_code_lo = lo;
    if (hi > exe_code_hi)
      exe_code_hi = hi;
  }
}

/* A return address follows a call instruction. This checks the bytes before
 * the address for the common call forms, so stale numbers on the stack that
 * merely point into the code are mostly rejected. It is a heuristic. */
static int looks_like_return(uint64_t a) {
  if (a < exe_code_lo + 8 || a >= exe_code_hi)
    return 0;
  const uint8_t *p = (const uint8_t *)(uintptr_t)a;
  if (p[-5] == 0xE8) /* call rel32 */
    return 1;
  static const int lengths[] = {2, 3, 4, 6, 7}; /* FF /2 with a register, disp8, SIB or disp32 */
  for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++)
    if (p[-lengths[i]] == 0xFF && ((p[-lengths[i] + 1] >> 3) & 7) == 2)
      return 1;
  return 0;
}

static void sample_register(DWORD tid, int role) {
  LONG count = sample_thread_count;
  if (count > SAMPLE_THREADS)
    count = SAMPLE_THREADS;
  for (LONG i = 0; i < count; i++)
    if (sample_threads[i].tid == tid)
      return;
  LONG slot = InterlockedIncrement(&sample_thread_count) - 1;
  if (slot >= SAMPLE_THREADS)
    return;
  sample_threads[slot].role = role;
  MemoryBarrier();
  sample_threads[slot].tid = tid; /* the sampler skips a slot until its tid is set */
}

static void sample_note_thread(DWORD tid, const char *name) {
  if (!cfg_sample)
    return;
  int role = -1;
  if (!strncmp(name, "RenderThread", 12))
    role = ROLE_RENDER;
  else if (!strncmp(name, "RHIThread", 9))
    role = ROLE_RHI;
  else if (cfg_sample_all && !strncmp(name, "TaskGraph", 9))
    role = ROLE_TASK;
  else if (cfg_sample_all && !strncmp(name, "FAsyncLoadingThread", 19))
    role = ROLE_ASYNC;
  if (role >= 0)
    sample_register(tid, role);
}

static void sample_add(int role, int kind, uint64_t addr) {
  if (!addr)
    return;
  uint64_t key = ((uint64_t)(role * KIND_COUNT + kind) << 56) | (addr & 0x00FFFFFFFFFFFFFFull);
  size_t i = (size_t)((key * 0x9E3779B97F4A7C15ull) >> 47) & (SAMPLE_SLOTS - 1);
  for (size_t n = 0; n < SAMPLE_SLOTS; n++, i = (i + 1) & (SAMPLE_SLOTS - 1)) {
    if (sample_table[i].key == key) {
      sample_table[i].count++;
      return;
    }
    if (!sample_table[i].key) {
      if (sample_used >= SAMPLE_SLOTS / 4 * 3) {
        sample_overflow++;
        return;
      }
      sample_table[i].key = key;
      sample_table[i].count = 1;
      sample_used++;
      return;
    }
  }
}

static int sample_compare(const void *a, const void *b) {
  const sample_slot *x = a, *y = b;
  uint64_t gx = x->key >> 56, gy = y->key >> 56;
  if (gx != gy)
    return gx < gy ? -1 : 1;
  if (x->count != y->count)
    return x->count > y->count ? -1 : 1;
  return 0;
}

static sample_module sample_modules[256];
static int sample_module_count;

static void load_sample_modules(void) {
  sample_module_count = 0;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (snap == INVALID_HANDLE_VALUE)
    return;
  MODULEENTRY32W entry;
  entry.dwSize = sizeof(entry);
  for (BOOL ok = Module32FirstW(snap, &entry); ok && sample_module_count < 256; ok = Module32NextW(snap, &entry)) {
    sample_module *m = &sample_modules[sample_module_count++];
    m->base = (uint64_t)(uintptr_t)entry.modBaseAddr;
    m->size = entry.modBaseSize;
    WideCharToMultiByte(CP_UTF8, 0, entry.szModule, -1, m->name, sizeof(m->name) - 1, NULL, NULL);
    m->name[sizeof(m->name) - 1] = 0;
  }
  CloseHandle(snap);
}

static int find_sample_module(uint64_t addr) {
  for (int i = 0; i < sample_module_count; i++)
    if (addr >= sample_modules[i].base && addr < sample_modules[i].base + sample_modules[i].size)
      return i;
  return -1;
}

/* Writes the counts to a temporary file and renames it over the real one, so
 * a reader never sees half a file. Runs only on the sampler thread, while no
 * game thread is paused. */
static void sample_dump(DWORD started_ms, const char *path) {
  size_t n = 0;
  for (size_t i = 0; i < SAMPLE_SLOTS; i++)
    if (sample_table[i].key)
      sample_sorted[n++] = sample_table[i];
  qsort(sample_sorted, n, sizeof(sample_sorted[0]), sample_compare);
  load_sample_modules();
  static uint64_t role_total[ROLE_COUNT];
  static uint64_t module_sum[ROLE_COUNT][257]; /* the last column is "no module" */
  memset(role_total, 0, sizeof(role_total));
  memset(module_sum, 0, sizeof(module_sum));
  for (size_t i = 0; i < n; i++) {
    unsigned group = (unsigned)(sample_sorted[i].key >> 56);
    if (group % KIND_COUNT != KIND_RIP)
      continue;
    int role = (int)(group / KIND_COUNT);
    int m = find_sample_module(sample_sorted[i].key & 0x00FFFFFFFFFFFFFFull);
    role_total[role] += sample_sorted[i].count;
    module_sum[role][m < 0 ? 256 : m] += sample_sorted[i].count;
  }
  char tmp[MAX_PATH + 8];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE *f = fopen(tmp, "w");
  if (!f)
    return;
  fprintf(f, "# sc6vrr samples v1\n");
  fprintf(f, "# exe_hash_ok=%d exe_base=0x%llx ghidra_base=0x140000000 hz=%d all=%d\n", sample_exe_ok,
          (unsigned long long)exe_base_now, cfg_sample_hz, cfg_sample_all);
  fprintf(f, "# elapsed_ms=%lu passes=%llu open_fail=%llu suspend_fail=%llu context_fail=%llu table_overflow=%llu\n",
          (unsigned long)(GetTickCount() - started_ms), (unsigned long long)sample_passes,
          (unsigned long long)sample_open_fail, (unsigned long long)sample_suspend_fail,
          (unsigned long long)sample_context_fail, (unsigned long long)sample_overflow);
  for (int r = 0; r < ROLE_COUNT; r++) {
    if (!role_total[r])
      continue;
    fprintf(f, "# role_samples\t%s\t%llu\n", kRoleNames[r], (unsigned long long)role_total[r]);
    for (int m = 0; m <= sample_module_count && m <= 256; m++) {
      uint64_t c = module_sum[r][m < sample_module_count ? m : 256];
      if (m <= sample_module_count && c)
        fprintf(f, "# module_samples\t%s\t%s\t%llu\n", kRoleNames[r], m < sample_module_count ? sample_modules[m].name : "?",
                (unsigned long long)c);
    }
  }
  fprintf(f, "role\tkind\tmodule\taddr\tcount\n");
  unsigned long long emitted = 0;
  unsigned last_group = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    unsigned group = (unsigned)(sample_sorted[i].key >> 56);
    if (group != last_group) {
      last_group = group;
      emitted = 0;
    }
    if (emitted++ >= SAMPLE_LINES_PER_GROUP)
      continue;
    uint64_t addr = sample_sorted[i].key & 0x00FFFFFFFFFFFFFFull;
    int m = find_sample_module(addr);
    const char *name = m < 0 ? "?" : sample_modules[m].name;
    uint64_t shown = addr;
    if (m >= 0 && sample_modules[m].base == exe_base_now) {
      name = "exe";
      shown = addr - exe_base_now + 0x140000000ull; /* the address Ghidra shows */
    } else if (m >= 0) {
      shown = addr - sample_modules[m].base; /* offset inside the module */
    }
    fprintf(f, "%s\t%s\t%s\t0x%llx\t%u\n", kRoleNames[group / KIND_COUNT], kKindNames[group % KIND_COUNT], name,
            (unsigned long long)shown, sample_sorted[i].count);
  }
  fclose(f);
  MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
}

/* Returns the address of the thread's TIB, or NULL when ntdll cannot say. This
 * runs while the thread is not paused. */
static const NT_TIB *sample_thread_tib(HANDLE thread) {
  typedef struct {
    LONG exit_status;
    PVOID teb;
    PVOID client_id[2];
    ULONG_PTR affinity;
    LONG priority, base_priority;
  } basic_info;
  typedef LONG(NTAPI * query_fn)(HANDLE, int, PVOID, ULONG, PULONG);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  query_fn query = ntdll ? (query_fn)GetProcAddress(ntdll, "NtQueryInformationThread") : NULL;
  basic_info info;
  if (!query || query(thread, 0 /* ThreadBasicInformation */, &info, sizeof(info), NULL) < 0)
    return NULL;
  return (const NT_TIB *)info.teb;
}

static DWORD WINAPI sampler_main(LPVOID unused) {
  (void)unused;
  uint32_t rng = GetTickCount() | 1;
  DWORD period = (DWORD)(1000 / cfg_sample_hz);
  if (!period)
    period = 1;
  DWORD started = GetTickCount(), next_dump = started + (DWORD)cfg_sample_dump_secs * 1000;
  int reset_was_down = 0, snapshot_was_down = 0;
  unsigned snapshot_count = 0;
  for (;;) {
    /* A random wait (mean = period) keeps the sampler from locking on to the
     * game's own frame rhythm. */
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    DWORD wait = period / 2 + rng % (period + 1);
    Sleep(wait ? wait : 1);
    if (sample_stop)
      break;
    int reset_down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if ((reset_down && !reset_was_down) || InterlockedExchange(&sample_req_reset, 0)) {
      memset(sample_table, 0, sizeof(sample_table));
      sample_used = sample_overflow = sample_passes = 0;
      started = GetTickCount();
      log_msg("sample: counts cleared by F9");
    }
    reset_was_down = reset_down;
    /* F8 saves the counts so far to sc6vrr-samples.<n>.tsv and keeps counting.
     * With F9 at the start of a round, that gives one file per round. */
    int snapshot_down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if ((snapshot_down && !snapshot_was_down) || InterlockedExchange(&sample_req_snapshot, 0)) {
      char numbered[MAX_PATH + 16];
      size_t stem = strlen(sample_path);
      if (stem > 4 && !_stricmp(sample_path + stem - 4, ".tsv"))
        stem -= 4;
      snprintf(numbered, sizeof(numbered), "%.*s.%u.tsv", (int)stem, sample_path, ++snapshot_count);
      sample_dump(started, numbered);
      log_msg("sample: snapshot saved to %s", numbered);
    }
    snapshot_was_down = snapshot_down;
    LONG count = sample_thread_count;
    if (count > SAMPLE_THREADS)
      count = SAMPLE_THREADS;
    for (LONG i = 0; i < count; i++) {
      sample_thread *t = &sample_threads[i];
      if (!t->tid || t->dead)
        continue;
      if (!t->handle) {
        t->handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, t->tid);
        if (!t->handle) {
          t->dead = 1;
          sample_open_fail++;
          continue;
        }
        t->tib = sample_thread_tib(t->handle);
      }
      CONTEXT ctx;
      ctx.ContextFlags = CONTEXT_CONTROL;
      uint64_t stack[SAMPLE_STACK_QWORDS];
      SIZE_T got = 0;
      /* Paused window. The paused thread may hold any lock inside Wine or the
       * C library, so only calls that take no lock are allowed here. That rules
       * out ReadProcessMemory (it probably takes Wine's virtual-memory lock), VirtualQuery,
       * allocation and logging. A boot hang on 2026-10-31 may have come from here; unproven.
       * The stack is read directly instead, but only between RSP and the stack
       * base from the thread's TIB, which is committed memory that cannot fault. */
      if (SuspendThread(t->handle) == (DWORD)-1) {
        sample_suspend_fail++;
        CloseHandle(t->handle);
        t->handle = NULL;
        t->dead = 1; /* the thread has probably exited */
        continue;
      }
      BOOL ok = GetThreadContext(t->handle, &ctx);
      if (ok && t->tib) {
        uint64_t sp = ctx.Rsp, lo = (uint64_t)(uintptr_t)t->tib->StackLimit, hi = (uint64_t)(uintptr_t)t->tib->StackBase;
        if (sp >= lo && sp < hi && !(sp & 7)) {
          got = hi - sp < sizeof(stack) ? (SIZE_T)(hi - sp) : sizeof(stack);
          const volatile uint64_t *src = (const volatile uint64_t *)(uintptr_t)sp;
          for (SIZE_T q = 0; q < got / 8; q++)
            stack[q] = src[q];
        }
      }
      ResumeThread(t->handle);
      /* Thread running again. */
      if (!ok) {
        sample_context_fail++;
        continue;
      }
      sample_add(t->role, KIND_RIP, ctx.Rip);
      int depth = KIND_CALLER1;
      for (SIZE_T q = 0; q < got / 8 && depth <= KIND_CALLER3; q++)
        if (looks_like_return(stack[q]))
          sample_add(t->role, depth++, stack[q]);
    }
    sample_passes++;
    if ((LONG)(GetTickCount() - next_dump) >= 0) {
      sample_dump(started, sample_path);
      next_dump = GetTickCount() + (DWORD)cfg_sample_dump_secs * 1000;
    }
  }
  sample_dump(started, sample_path);
  return 0;
}

static void start_sampler(int exe_ok) {
  sample_exe_ok = exe_ok;
  find_exe_code();
  if (!cfg_str("SC6VRR_SAMPLE_FILE", sample_path, sizeof(sample_path))) {
    DWORD n = GetModuleFileNameA(NULL, sample_path, sizeof(sample_path));
    char *slash = (n && n < sizeof(sample_path)) ? strrchr(sample_path, '\\') : NULL;
    if (!slash || (size_t)(slash + 1 - sample_path) + sizeof("sc6vrr-samples.tsv") > sizeof(sample_path)) {
      log_msg("sample: no output path; sampler off");
      return;
    }
    strcpy(slash + 1, "sc6vrr-samples.tsv");
  }
  if (!thread_name_observer)
    install_thread_name_observer();
  sample_register(GetCurrentThreadId(), ROLE_GAME);
  HANDLE h = CreateThread(NULL, 0, sampler_main, NULL, 0, NULL);
  if (!h) {
    log_msg("sample: CreateThread failed (%lu); sampler off", (unsigned long)GetLastError());
    return;
  }
  CloseHandle(h);
  log_msg("sample: on, %d Hz, all=%d, game thread %lu, output %s", cfg_sample_hz, cfg_sample_all,
          (unsigned long)GetCurrentThreadId(), sample_path);
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

static void title_tick(void); /* SC6VRR_SKIP_TITLE; defined with the title skip */
static volatile LONG64 g_frames; /* engine frames, counted in the UpdateTime hooks */

static void hook_update_time(void *self) {
  double timing_start = cfg_measure ? qpc_seconds() : 0.0;
  orig_update_time(self);
  g_frames++;
  title_tick();

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

static int cfg_skip_prepass; /* SC6VRR_SKIP_PREPASS; explained where the prepass is skipped */
typedef HRESULT(WINAPI *create_depth_state_fn)(void *self, const D3D11_DEPTH_STENCIL_DESC *desc, void **out);
typedef HRESULT(WINAPI *create_raster_state_fn)(void *self, const D3D11_RASTERIZER_DESC *desc, void **out);
typedef HRESULT(WINAPI *create_sampler_state_fn)(void *self, const D3D11_SAMPLER_DESC *desc, void **out);
static create_depth_state_fn orig_create_depth_state;
static create_raster_state_fn orig_create_raster_state;
static create_sampler_state_fn orig_create_sampler_state;

/* Twins for the prepass skip (explained at SC6VRR_SKIP_PREPASS below). For
 * every depth state that tests depth but does not write it, or tests "equal",
 * a second state that writes depth is made when the game creates the first.
 * The game's own states stay as they are, and the main pass binds a twin only
 * on frames where the prepass was dropped, so the skip can be toggled while the
 * game runs. The game uses reversed depth (near is 1.0, far is 0.0, the buffer
 * clears to 0), so "equal" becomes "greater or equal" in the twin. */
#define DEPTH_TWINS 256
static void *depth_twin_orig[DEPTH_TWINS], *depth_twin_new[DEPTH_TWINS];
static volatile LONG depth_twins;
static SRWLOCK depth_twin_lock = SRWLOCK_INIT;

static void *depth_twin(void *state) {
  LONG n = InterlockedCompareExchange(&depth_twins, 0, 0); /* a full barrier: entries below n are written */
  for (LONG i = 0; i < n; i++)
    if (depth_twin_orig[i] == state)
      return depth_twin_new[i];
  return NULL;
}

static HRESULT WINAPI hook_create_depth_state(void *self, const D3D11_DEPTH_STENCIL_DESC *desc, void **out) {
  HRESULT hr = orig_create_depth_state(self, desc, out);
  if (FAILED(hr) || !out || !*out || !desc)
    return hr;
  capture_depth_state(desc, *out);
  if (!shader_dir[0] || !desc->DepthEnable ||
      (desc->DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO && desc->DepthFunc != D3D11_COMPARISON_EQUAL))
    return hr;
  AcquireSRWLockExclusive(&depth_twin_lock);
  LONG n = depth_twins;
  if (!depth_twin(*out) && n < DEPTH_TWINS) {
    D3D11_DEPTH_STENCIL_DESC t = *desc;
    if (t.DepthFunc == D3D11_COMPARISON_EQUAL)
      t.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    t.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    void *twin = NULL;
    if (SUCCEEDED(orig_create_depth_state(self, &t, &twin)) && twin) {
      depth_twin_orig[n] = *out;
      depth_twin_new[n] = twin; /* kept for the whole run */
      InterlockedExchange(&depth_twins, n + 1);
      if (n < 24)
        log_msg("prepass: depth twin %ld: func %d write %d stencil %d mask 0x%02x -> func %d write 1", n + 1,
                (int)desc->DepthFunc, (int)desc->DepthWriteMask, (int)desc->StencilEnable,
                (unsigned)desc->StencilWriteMask, (int)t.DepthFunc);
    }
  }
  ReleaseSRWLockExclusive(&depth_twin_lock);
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

/* The per-pass GPU timer (SC6VRR_GPU_PASSES) is defined further down, after
 * the context hooks it needs. These hooks call into it. */
static int cfg_gpu_passes;
static void *gpu_device, *gpu_ctx;
static void gpu_pass_draw(void);
static void gpu_pass_targets(void *ctx, UINT count, void *const *rtvs, void *dsv);
static void gpu_pass_dispatch(void *ctx);
static void gpu_pass_present(void);
static void gpu_view_info(void *view, void **res, uint32_t *w, uint32_t *h, uint32_t *fmt);
static void gpu_short_id(uint32_t id, char out[13]);

/* Called on every draw. Only a combination not seen before costs more than
 * one hash probe; its IDs are written out as hashes. */
static void note_draw(void) {
  gpu_pass_draw();
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
  CTX_CS_SAMPLERS, CTX_GET_DATA, CTX_MAP, CTX_FLUSH, CTX_BEGIN, CTX_END, CTX_COUNT
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
    CONTEXT_SLOT(Flush), CONTEXT_SLOT(Begin), CONTEXT_SLOT(End)};
static const char *const kContextNames[CTX_COUNT] = {
    "VSSetShader", "GSSetShader", "PSSetShader", "IASetInputLayout", "IASetPrimitiveTopology",
    "IASetVertexBuffers", "OMSetRenderTargets", "OMSetRenderTargetsAndUnorderedAccessViews", "OMSetBlendState",
    "Draw", "DrawIndexed", "DrawIndexedInstanced", "DrawInstanced", "DrawAuto", "DrawIndexedInstancedIndirect",
    "DrawInstancedIndirect", "CSSetShader", "PSSetShaderResources", "CSSetShaderResources",
    "CSSetUnorderedAccessViews", "ClearRenderTargetView", "ClearDepthStencilView", "RSSetViewports", "Dispatch",
    "DispatchIndirect", "CopyResource", "CopySubresourceRegion", "ResolveSubresource", "OMSetDepthStencilState",
    "RSSetState", "VSSetShaderResources", "PSSetSamplers", "VSSetSamplers", "CSSetSamplers", "GetData", "Map",
    "Flush", "Begin", "End"};
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

/* With SC6VRR_SKIP_PREPASS=1 the depth prepass is not drawn. The depth
 * prepass is the pass that draws the opaque scene into the depth buffer only
 * (no pixel shader, no color target) before the main scene pass, so that the
 * main pass can skip hidden pixels. Apple GPUs remove hidden pixels in
 * hardware, so the prepass is mostly wasted work there.
 *
 * What counts, per frame and per depth buffer:
 * - A scene depth is a depth buffer that the game binds together with 4 or
 *   more color targets (the G-buffer: the textures that hold the scene's
 *   materials). The DLL learns them while the game runs, so other depth
 *   buffers (custom depth, shadow maps) are never touched.
 * - A depth-only pass on a scene depth before that buffer's main pass in the
 *   frame is the prepass. Its draws without a pixel shader are dropped.
 * - The main pass is the first G-buffer pass after a dropped prepass, and
 *   later binds of the same targets in that frame. While it is bound, each
 *   depth state is swapped for its twin that writes depth (see
 *   hook_create_depth_state), because nothing else writes depth now.
 * - Depth-only passes after the main pass are left alone, and so is every draw
 *   inside an occlusion query (a query that counts the visible pixels of a
 *   box, so the game can stop drawing hidden objects). The first versions
 *   dropped those draws too. The queries then reported "hidden", and the game
 *   stopped drawing whole objects (corrupt picture, 2026-10-31; probable cause,
 *   not proven).
 * F6 toggles the skip while the game runs, and so does a changed sc6vrr.ini.
 * A change applies from the next frame. */
typedef void(WINAPI *set_depth_state_fn)(void *ctx, void *state, UINT stencil_ref);
#define SCENE_DEPTHS 8
enum { DEPTH_FRESH, DEPTH_PREPASS, DEPTH_MAIN };
static void *scene_depth[SCENE_DEPTHS], *scene_main_rt0[SCENE_DEPTHS];
static UINT scene_main_count[SCENE_DEPTHS];
static uint8_t scene_depth_state[SCENE_DEPTHS]; /* reset every frame */
static unsigned scene_depths;
static int prepass_state;      /* 1 while a prepass is bound: drop its depth-only draws */
static int main_pass_active;   /* 1 while a main pass after a dropped prepass is bound */
static int skip_this_frame;    /* cfg_skip_prepass, latched at Present */
static void *prepass_ctx;      /* the context of the last targets call */
static void *game_depth_state; /* the depth state the game bound last */
#define OCCLUSION_MAX 16
static void *occlusion_open[OCCLUSION_MAX];
static int occlusion_active; /* occlusion queries begun and not yet ended */
static uint64_t prepass_skipped, twin_binds, occlusion_draws_kept;

static int scene_depth_find(void *res) {
  for (unsigned i = 0; i < scene_depths; i++)
    if (scene_depth[i] == res)
      return (int)i;
  return -1;
}

/* Runs before the game's targets call. Returns 1 when the main pass begins or
 * ends, because the depth state must then be bound again after the call. */
static int prepass_targets(void *ctx, UINT count, void *const *rtvs, void *dsv) {
  int was_main = main_pass_active;
  prepass_state = 0;
  main_pass_active = 0;
  if (!skip_this_frame)
    return was_main;
  prepass_ctx = ctx;
  void *rtv0 = count && rtvs ? rtvs[0] : NULL;
  void *res = NULL, *rt0 = NULL;
  uint32_t w, h, fmt;
  if (dsv)
    gpu_view_info(dsv, &res, &w, &h, &fmt);
  if (!res)
    return was_main;
  int i = scene_depth_find(res);
  if (rtv0 && count >= 4) {
    if (i < 0 && scene_depths < SCENE_DEPTHS) {
      i = (int)scene_depths++;
      scene_depth[i] = res;
      scene_depth_state[i] = DEPTH_FRESH;
      log_msg("prepass: scene depth %d is %ux%u", i + 1, w, h);
    }
    if (i >= 0) {
      gpu_view_info(rtv0, &rt0, &w, &h, &fmt);
      if (scene_depth_state[i] == DEPTH_PREPASS) {
        scene_depth_state[i] = DEPTH_MAIN;
        scene_main_rt0[i] = rt0;
        scene_main_count[i] = count;
        main_pass_active = 1;
      } else if (scene_depth_state[i] == DEPTH_MAIN && scene_main_rt0[i] == rt0 && scene_main_count[i] == count) {
        main_pass_active = 1;
      }
    }
  } else if (!rtv0 && i >= 0 && scene_depth_state[i] != DEPTH_MAIN) {
    scene_depth_state[i] = DEPTH_PREPASS;
    prepass_state = 1;
  }
  return was_main != main_pass_active;
}

static void prepass_rebind(void *ctx) {
  void *state = game_depth_state, *twin;
  if (main_pass_active && (twin = depth_twin(state)))
    state = twin;
  ((set_depth_state_fn)orig_context[CTX_DEPTH_STATE])(ctx, state, bound_stencil_ref);
}

/* Occlusion queries. Begin takes any ID3D11Asynchronous; a query and a
 * predicate both have ID3D11Query's GetDesc at the same place, and a counter's
 * GetDesc fills a struct of the same size whose first value never equals an
 * occlusion type, so no QueryInterface is needed. */
typedef void(WINAPI *async_fn)(void *ctx, void *async);
typedef void(WINAPI *query_desc_fn)(void *query, D3D11_QUERY_DESC *desc);
static void WINAPI hook_query_begin(void *ctx, void *async) {
  if (skip_this_frame && async && occlusion_active < OCCLUSION_MAX) {
    D3D11_QUERY_DESC d = {0};
    ((query_desc_fn)VTBL_METHOD(async, ID3D11Query, GetDesc))(async, &d);
    if (d.Query == D3D11_QUERY_OCCLUSION || d.Query == D3D11_QUERY_OCCLUSION_PREDICATE)
      for (int i = 0; i < OCCLUSION_MAX; i++)
        if (!occlusion_open[i]) {
          occlusion_open[i] = async;
          occlusion_active++;
          break;
        }
  }
  ((async_fn)orig_context[CTX_BEGIN])(ctx, async);
}
static void WINAPI hook_query_end(void *ctx, void *async) {
  if (occlusion_active)
    for (int i = 0; i < OCCLUSION_MAX; i++)
      if (occlusion_open[i] == async) {
        occlusion_open[i] = NULL;
        occlusion_active--;
        break;
      }
  ((async_fn)orig_context[CTX_END])(ctx, async);
}

/* Live settings: F6 toggles the prepass skip, and a changed sc6vrr.ini is read
 * again (checked once a second). Only the settings below change live. */
static void live_reload(void) {
  int skip = env_int("SC6VRR_SKIP_PREPASS", 0), gs = env_int("SC6VRR_SKIP_GS_DRAWS", 0);
  if (skip && !shader_dir[0])
    skip = 0;
  float fps = (float)env_int("SC6VRR_MAX_FPS", 120);
  log_msg("live: sc6vrr.ini changed: SC6VRR_SKIP_PREPASS %d -> %d, SC6VRR_SKIP_GS_DRAWS %d -> %d, "
          "SC6VRR_MAX_FPS %.0f -> %.0f",
          cfg_skip_prepass, skip, cfg_skip_gs_draws, gs, cfg_max_fps, fps);
  cfg_skip_prepass = skip;
  cfg_skip_gs_draws = gs;
  cfg_max_fps = fps; /* read on every engine tick, so a plain store is enough */
}

static void prepass_present(void) {
  static int f6_was_down;
  static ULONGLONG next_check;
  static FILETIME ini_time;
  static unsigned frames;
  int down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
  if (down && !f6_was_down) {
    if (shader_dir[0]) {
      cfg_skip_prepass = !cfg_skip_prepass;
      log_msg("live: F6: SC6VRR_SKIP_PREPASS=%d", cfg_skip_prepass);
    } else {
      log_msg("live: F6 ignored: the prepass skip needs SC6VRR_SHADER_DIR");
    }
  }
  f6_was_down = down;
  ULONGLONG now = GetTickCount64();
  if (ini_path[0] && now >= next_check) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    next_check = now + 1000;
    if (GetFileAttributesExA(ini_path, GetFileExInfoStandard, &a)) {
      if ((ini_time.dwLowDateTime || ini_time.dwHighDateTime) && CompareFileTime(&a.ftLastWriteTime, &ini_time))
        live_reload();
      ini_time = a.ftLastWriteTime;
    }
  }
  if (main_pass_active && prepass_ctx) {
    main_pass_active = 0;
    prepass_rebind(prepass_ctx);
  }
  prepass_state = 0;
  skip_this_frame = cfg_skip_prepass;
  for (unsigned i = 0; i < scene_depths; i++)
    scene_depth_state[i] = DEPTH_FRESH;
  memset(occlusion_open, 0, sizeof(occlusion_open));
  occlusion_active = 0;
  if (skip_this_frame && ++frames % 600 == 0)
    log_msg("prepass: so far %llu draws dropped, %llu twin binds, %llu draws kept inside occlusion queries",
            (unsigned long long)prepass_skipped, (unsigned long long)twin_binds,
            (unsigned long long)occlusion_draws_kept);
}

/* Draws are timed when the GPU timer is on; a draw that takes long on the
 * calling thread (for example one that waits for a pipeline to compile) is
 * logged with its shaders. */
#define SLOW_DRAW_MS 2.0
static unsigned slow_draws_logged;
static double slow_draw_begin(void);
static void slow_draw_end(double t0, const char *what);

static int skip_gs_draw(void) {
  if (prepass_state && bound.ps == ID_NONE) {
    if (occlusion_active) {
      occlusion_draws_kept++;
    } else {
      if (++prepass_skipped == 1)
        log_msg("prepass: dropping depth-only draws of the prepass (SC6VRR_SKIP_PREPASS=1)");
      return 1;
    }
  }
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
  gpu_pass_targets(ctx, count, rtvs, dsv);
  int rebind = prepass_targets(ctx, count, rtvs, dsv);
  note_targets(count, rtvs, dsv);
  if (FRAME_LOGGING_NOW)
    log_targets("OMSetRenderTargets", count, rtvs, dsv);
  ((set_targets_fn)orig_context[CTX_TARGETS])(ctx, count, rtvs, dsv);
  if (rebind)
    prepass_rebind(ctx);
}
static void WINAPI hook_targets_uavs_set(void *ctx, UINT count, void *const *rtvs, void *dsv, UINT uav_start,
                                         UINT uav_count, void *const *uavs, const UINT *initial_counts) {
  int rebind = 0;
  if (count != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
    gpu_pass_targets(ctx, count, rtvs, dsv);
    rebind = prepass_targets(ctx, count, rtvs, dsv);
    note_targets(count, rtvs, dsv);
    if (FRAME_LOGGING_NOW)
      log_targets("OMSetRenderTargetsAndUnorderedAccessViews", count, rtvs, dsv);
  }
  if (FRAME_LOGGING_NOW && uav_count != D3D11_KEEP_UNORDERED_ACCESS_VIEWS)
    describe_views("OMSetUnorderedAccessViews", uav_start, uav_count, uavs);
  ((set_targets_uavs_fn)orig_context[CTX_TARGETS_UAVS])(ctx, count, rtvs, dsv, uav_start, uav_count, uavs,
                                                        initial_counts);
  if (rebind)
    prepass_rebind(ctx);
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
  double t0_ = slow_draw_begin();
  ((draw_fn)orig_context[CTX_DRAW])(ctx, count, start);
  slow_draw_end(t0_, "DRAW");
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
  double t0_ = slow_draw_begin();
  ((draw_indexed_fn)orig_context[CTX_DRAW_INDEXED])(ctx, count, start, base);
  slow_draw_end(t0_, "DRAW_INDEXED");
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
  double t0_ = slow_draw_begin();
  ((draw_indexed_instanced_fn)orig_context[CTX_DRAW_INDEXED_INSTANCED])(ctx, count, instances, start, base,
                                                                        start_instance);
  slow_draw_end(t0_, "DRAW_INDEXED_INSTANCED");
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
  double t0_ = slow_draw_begin();
  ((draw_instanced_fn)orig_context[CTX_DRAW_INSTANCED])(ctx, count, instances, start, start_instance);
  slow_draw_end(t0_, "DRAW_INSTANCED");
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
  double t0_ = slow_draw_begin();
  ((draw_auto_fn)orig_context[CTX_DRAW_AUTO])(ctx);
  slow_draw_end(t0_, "DRAW_AUTO");
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
  double t0_ = slow_draw_begin();
  ((draw_indirect_fn)orig_context[CTX_DRAW_INDEXED_INDIRECT])(ctx, args, offset);
  slow_draw_end(t0_, "DRAW_INDEXED_INDIRECT");
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
  double t0_ = slow_draw_begin();
  ((draw_indirect_fn)orig_context[CTX_DRAW_INSTANCED_INDIRECT])(ctx, args, offset);
  slow_draw_end(t0_, "DRAW_INSTANCED_INDIRECT");
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
  gpu_pass_dispatch(ctx);
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

typedef void(WINAPI *set_raster_state_fn)(void *ctx, void *state);
typedef void(WINAPI *set_samplers_fn)(void *ctx, UINT start, UINT count, void *const *samplers);

static void WINAPI hook_depth_state_set(void *ctx, void *state, UINT stencil_ref) {
  bound_depth_state = object_lookup(state, NULL);
  bound_stencil_ref = stencil_ref;
  game_depth_state = state;
  void *twin;
  if (main_pass_active && (twin = depth_twin(state))) {
    state = twin;
    twin_binds++;
  }
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

/* ---------- per-pass GPU timer (SC6VRR_GPU_PASSES) ------------------------ */

/* A "pass" here is a run of calls between two changes of the render target.
 * Draws are one kind of pass. A run of compute dispatches is another. At each
 * boundary the timer asks the GPU for a timestamp (an ID3D11Query of type
 * D3D11_QUERY_TIMESTAMP). The cost of a pass is the difference between its two
 * timestamps. The results arrive a few frames later, so the timer keeps a ring
 * of frames and reads each frame's queries once the GPU has finished them.
 *
 * Needs the DXMT build whose timestamps come from a GPU counter sample buffer
 * (TimestampReadbackSBuf). The stock build gives every timestamp in a Metal
 * command buffer the same value. Each timestamp also splits the Metal pass and
 * forces a flush, so the timer slows the game and moves some cost around. Use
 * it to rank passes, not to predict the frame rate.
 *
 * Work that sits between two boundaries (clears, copies) is charged to the pass
 * before it. The immediate context is used from one thread, so no lock is
 * needed. Needs SC6VRR_SHADER_DIR, so that shader IDs exist. */
#define GPU_MAX_PASSES 300
#define GPU_RING 8
#define GPU_MIN_AGE 3  /* frames to wait before the first read */
#define GPU_MAX_AGE 12 /* frames after which an unfinished frame is dropped */
#define GPU_AGG_SLOTS 1024
#define GPU_DUMP_SECONDS 10.0

typedef HRESULT(WINAPI *create_query_fn)(void *device, const D3D11_QUERY_DESC *desc, void **query);
typedef void(WINAPI *get_device_fn)(void *child, void **device);
typedef void(WINAPI *end_fn)(void *ctx, void *async);

enum { GPU_PASS_DRAW, GPU_PASS_CS };
typedef struct {
  void *rtv0, *dsv_view;     /* the views last seen, to skip repeated sets */
  void *rt0_res, *dsv_res;   /* the textures behind them: the pass identity */
  uint32_t type, rt_count, draws, ps, vs, cs;
  uint32_t w, h, fmt, dw, dh;
} gpu_pass;
typedef struct {
  gpu_pass pass[GPU_MAX_PASSES];
  void *query[GPU_MAX_PASSES + 1];
  uint64_t ts[GPU_MAX_PASSES + 1];
  uint8_t got[GPU_MAX_PASSES + 1];
  uint32_t npass;   /* closed passes; pass[npass] is the open one if `open` */
  uint32_t nquery;  /* timestamps issued; a frame with n passes has n + 1 */
  uint32_t nread;
  int open, done, age;
  double wall_start, wall;
} gpu_frame;
static gpu_frame gpu_frames[GPU_RING];
static int gpu_cur, gpu_started;
static char gpu_path[MAX_PATH];

typedef struct {
  uint64_t key, n, draws;
  double ticks;
  uint32_t type, rt_count, ps, vs, cs, w, h, fmt, dw, dh, first_order;
} gpu_agg;
static gpu_agg gpu_aggs[GPU_AGG_SLOTS];
static size_t gpu_agg_count;
static double gpu_ticks_total, gpu_wall_total, gpu_next_dump;
static uint64_t gpu_frames_read, gpu_frames_dropped, gpu_frames_total;
static int gpu_reset_was_down;

/* The slowest frames, each with its full pass list, so that rare heavy frames
 * (for example a super move) are not lost in the averages. Frames over
 * GPU_WORST_MAX_MS are skipped: those are load hitches, not heavy scenes. */
#define GPU_WORST 6
#define GPU_WORST_MAX_MS 32.0
#define GPU_HITCH_MS 22.0 /* frames slower than this are logged one by one */
typedef struct {
  double total;             /* raw timestamp units; 0 = empty slot */
  uint32_t n;
  gpu_pass pass[GPU_MAX_PASSES];
  double d[GPU_MAX_PASSES]; /* raw units per pass */
} gpu_worst_frame;
static gpu_worst_frame gpu_worst[GPU_WORST];
static char gpu_worst_path[MAX_PATH];

static void gpu_view_info(void *view, void **res, uint32_t *w, uint32_t *h, uint32_t *fmt) {
  *res = NULL;
  *w = *h = *fmt = 0;
  if (!view)
    return;
  void *resource = NULL;
  ((get_resource_fn)VTBL_METHOD(view, ID3D11View, GetResource))(view, &resource);
  if (!resource)
    return;
  D3D11_RESOURCE_DIMENSION type = D3D11_RESOURCE_DIMENSION_UNKNOWN;
  ((get_type_fn)VTBL_METHOD(resource, ID3D11Resource, GetType))(resource, &type);
  if (type == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
    D3D11_TEXTURE2D_DESC d;
    ((texture2d_desc_fn)VTBL_METHOD(resource, ID3D11Texture2D, GetDesc))(resource, &d);
    *w = d.Width;
    *h = d.Height;
    *fmt = (uint32_t)d.Format;
  }
  *res = resource; /* only compared, never used again */
  ((com_release_fn)VTBL_METHOD(resource, ID3D11Resource, Release))(resource);
}

/* Issues one timestamp for the frame; returns 0 if it could not. */
static int gpu_stamp(void *ctx, gpu_frame *fr) {
  if (fr->nquery >= GPU_MAX_PASSES + 1)
    return 0;
  uint32_t i = fr->nquery;
  if (!fr->query[i]) {
    D3D11_QUERY_DESC desc = {D3D11_QUERY_TIMESTAMP, 0};
    void *query = NULL;
    if (FAILED(((create_query_fn)VTBL_METHOD(gpu_device, ID3D11Device, CreateQuery))(gpu_device, &desc, &query)) ||
        !query)
      return 0;
    fr->query[i] = query;
  }
  ((end_fn)VTBL_METHOD(ctx, ID3D11DeviceContext, End))(ctx, fr->query[i]);
  fr->nquery++;
  return 1;
}

static void gpu_close_pass(void *ctx, gpu_frame *fr) {
  if (!fr->open)
    return;
  if (gpu_stamp(ctx, fr))
    fr->npass++;
  fr->open = 0;
}

static gpu_pass *gpu_open_pass(void *ctx, gpu_frame *fr, uint32_t type) {
  /* A full frame keeps its last pass open, which then absorbs the rest. */
  if (fr->npass >= GPU_MAX_PASSES - 1)
    return fr->open ? &fr->pass[fr->npass] : NULL;
  gpu_close_pass(ctx, fr);
  gpu_pass *p = &fr->pass[fr->npass];
  memset(p, 0, sizeof(*p));
  p->type = type;
  fr->open = 1;
  return p;
}

static void gpu_frame_begin(void *ctx, gpu_frame *fr) {
  fr->npass = fr->nquery = fr->nread = 0;
  fr->open = fr->done = fr->age = 0;
  memset(fr->got, 0, sizeof(fr->got));
  fr->wall_start = qpc_seconds();
  gpu_stamp(ctx, fr); /* timestamp 0 starts the first pass */
}

static void gpu_pass_draw(void) {
  if (!cfg_gpu_passes || !gpu_started)
    return;
  gpu_frame *fr = &gpu_frames[gpu_cur];
  if (!fr->open || fr->pass[fr->npass].type != GPU_PASS_DRAW)
    return;
  gpu_pass *p = &fr->pass[fr->npass];
  if (!p->draws++) {
    p->ps = bound.ps;
    p->vs = bound.vs;
  }
}

/* The game may create a short-lived device before the real one, so the
 * context and device come from live hook calls, never from device creation. */
static void gpu_pass_targets(void *ctx, UINT count, void *const *rtvs, void *dsv) {
  if (!cfg_gpu_passes)
    return;
  gpu_ctx = ctx;
  if (!gpu_started)
    return;
  void *rtv0 = count && rtvs ? rtvs[0] : NULL;
  if (!rtv0 && !dsv)
    return; /* unbinding everything is not a new pass */
  gpu_frame *fr = &gpu_frames[gpu_cur];
  gpu_pass *cur = fr->open ? &fr->pass[fr->npass] : NULL;
  if (cur && cur->type == GPU_PASS_DRAW && cur->rtv0 == rtv0 && cur->dsv_view == dsv)
    return;
  void *rt_res, *ds_res;
  uint32_t w, h, fmt, dw, dh, dfmt;
  gpu_view_info(rtv0, &rt_res, &w, &h, &fmt);
  gpu_view_info(dsv, &ds_res, &dw, &dh, &dfmt);
  int same = cur && cur->type == GPU_PASS_DRAW &&
             (cfg_gpu_passes >= 2 ? (cur->w == w && cur->h == h && cur->fmt == fmt && cur->dw == dw &&
                                     cur->dh == dh && cur->rt_count == count)
                                  : (cur->rt0_res == rt_res && cur->dsv_res == ds_res));
  if (same) {
    cur->rtv0 = rtv0; /* another view of the same textures: the same pass */
    cur->dsv_view = dsv;
    return;
  }
  gpu_pass *p = gpu_open_pass(ctx, fr, GPU_PASS_DRAW);
  if (!p || p == cur)
    return;
  p->rtv0 = rtv0;
  p->dsv_view = dsv;
  p->rt0_res = rt_res;
  p->dsv_res = ds_res;
  p->rt_count = count;
  p->w = w;
  p->h = h;
  p->fmt = fmt;
  p->dw = dw;
  p->dh = dh;
}

static void gpu_pass_dispatch(void *ctx) {
  if (!cfg_gpu_passes)
    return;
  gpu_ctx = ctx;
  if (!gpu_started)
    return;
  gpu_frame *fr = &gpu_frames[gpu_cur];
  gpu_pass *cur = fr->open ? &fr->pass[fr->npass] : NULL;
  if (cur && cur->type == GPU_PASS_CS) {
    cur->draws++;
    return;
  }
  gpu_pass *p = gpu_open_pass(ctx, fr, GPU_PASS_CS);
  if (p && p != cur) {
    p->cs = bound_cs;
    p->draws = 1;
  }
}

static uint64_t gpu_key(const gpu_pass *p) {
  uint32_t k[12] = {p->type, p->rt_count, p->ps, p->vs, p->cs, p->w, p->h, p->fmt, p->dw, p->dh, 0, 0};
  return fnv1a(k, sizeof(k));
}

static void gpu_short_id(uint32_t id, char out[13]);

static void gpu_accumulate(gpu_frame *fr) {
  double total = 0;
  static double dd[GPU_MAX_PASSES];
  for (uint32_t i = 0; i < fr->npass; i++) {
    const gpu_pass *p = &fr->pass[i];
    uint64_t t0 = fr->ts[i], t1 = fr->ts[i + 1];
    double d = t1 > t0 ? (double)(t1 - t0) : 0.0; /* timestamps must not go backward */
    dd[i] = d;
    total += d;
    uint64_t key = gpu_key(p);
    gpu_agg *a = NULL;
    for (size_t j = key & (GPU_AGG_SLOTS - 1), tries = 0; tries < GPU_AGG_SLOTS; j = (j + 1) & (GPU_AGG_SLOTS - 1), tries++) {
      if (gpu_aggs[j].n && gpu_aggs[j].key == key) {
        a = &gpu_aggs[j];
        break;
      }
      if (!gpu_aggs[j].n && gpu_agg_count < GPU_AGG_SLOTS / 4 * 3) {
        a = &gpu_aggs[j];
        memset(a, 0, sizeof(*a));
        a->key = key;
        a->type = p->type;
        a->rt_count = p->rt_count;
        a->ps = p->ps;
        a->vs = p->vs;
        a->cs = p->cs;
        a->w = p->w;
        a->h = p->h;
        a->fmt = p->fmt;
        a->dw = p->dw;
        a->dh = p->dh;
        a->first_order = i;
        gpu_agg_count++;
        break;
      }
    }
    if (!a)
      continue;
    a->n++;
    a->draws += p->draws;
    a->ticks += d;
  }
  gpu_ticks_total += total;
  gpu_wall_total += fr->wall;
  gpu_frames_read++;
  /* One log line per slow frame, so hitches can be lined up with what happened
   * in the game. The longest pass tells whether the GPU worked or sat idle:
   * a pass with no draws that takes many milliseconds is a gap. */
  static unsigned hitches_logged;
  if (fr->wall * 1000.0 > GPU_HITCH_MS && hitches_logged < 300) {
    uint32_t longest = 0;
    for (uint32_t i = 1; i < fr->npass; i++)
      if (dd[i] > dd[longest])
        longest = i;
    char ps[13] = "none", vs[13] = "none";
    if (fr->npass) {
      gpu_short_id(fr->pass[longest].ps, ps);
      gpu_short_id(fr->pass[longest].vs, vs);
    }
    log_msg("gpu hitch %u: frame %.1f ms, GPU %.1f ms, %u passes; longest pass #%u %.1f ms (%u draws, %ux%u, ps %s, vs %s)",
            ++hitches_logged, fr->wall * 1000.0, total * 1e-6, fr->npass, longest, fr->npass ? dd[longest] * 1e-6 : 0.0,
            fr->npass ? fr->pass[longest].draws : 0, fr->npass ? fr->pass[longest].w : 0,
            fr->npass ? fr->pass[longest].h : 0, ps, vs);
  }
  /* Compare in wall time: the units are close to nanoseconds, but the wall
   * time does not depend on that. */
  if (fr->wall * 1000.0 <= GPU_WORST_MAX_MS) {
    gpu_worst_frame *slot = &gpu_worst[0];
    for (int i = 1; i < GPU_WORST; i++)
      if (gpu_worst[i].total < slot->total)
        slot = &gpu_worst[i];
    if (total > slot->total) {
      slot->total = total;
      slot->n = fr->npass;
      memcpy(slot->pass, fr->pass, fr->npass * sizeof(gpu_pass));
      memcpy(slot->d, dd, fr->npass * sizeof(double));
    }
  }
}

/* Reads the queries of every finished frame. A frame is used only if all of its
 * timestamps are ready; one that is not ready after GPU_MAX_AGE frames is dropped. */
static void gpu_harvest(void *ctx) {
  for (int k = 0; k < GPU_RING; k++) {
    gpu_frame *fr = &gpu_frames[k];
    if (!fr->done || k == gpu_cur)
      continue;
    if (++fr->age < GPU_MIN_AGE)
      continue;
    for (uint32_t i = 0; i < fr->nquery; i++) {
      if (fr->got[i])
        continue;
      uint64_t value = 0;
      HRESULT hr = ((get_data_fn)orig_context[CTX_GET_DATA])(ctx, fr->query[i], &value, sizeof(value),
                                                            D3D11_ASYNC_GETDATA_DONOTFLUSH);
      if (hr != S_OK)
        break; /* the later timestamps cannot be ready before this one */
      fr->ts[i] = value;
      fr->got[i] = 1;
      fr->nread++;
    }
    if (fr->nread == fr->nquery) {
      if (fr->npass)
        gpu_accumulate(fr);
      fr->done = 0;
    } else if (fr->age > GPU_MAX_AGE) {
      gpu_frames_dropped++;
      fr->done = 0;
    }
  }
}

static void gpu_short_id(uint32_t id, char out[13]) {
  char hex[65];
  id_hex(id, hex);
  memcpy(out, hex, 12);
  out[12] = 0;
  if (strlen(hex) < 12)
    snprintf(out, 13, "%s", hex);
}

static double slow_draw_begin(void) { return cfg_gpu_passes ? qpc_seconds() : 0.0; }
static void slow_draw_end(double t0, const char *what) {
  if (!t0)
    return;
  double ms = (qpc_seconds() - t0) * 1000.0;
  if (ms < SLOW_DRAW_MS || slow_draws_logged >= 300)
    return;
  char ps[13], vs[13];
  gpu_short_id(bound.ps, ps);
  gpu_short_id(bound.vs, vs);
  log_msg("slow draw %u: %s took %.1f ms on the calling thread (ps %s, vs %s, %u render targets)", ++slow_draws_logged,
          what, ms, ps, vs, bound.rt_count);
}

static int gpu_agg_cmp(const void *a, const void *b) {
  double x = ((const gpu_agg *)a)->ticks, y = ((const gpu_agg *)b)->ticks;
  return x < y ? 1 : x > y ? -1 : 0;
}

static void gpu_dump(void) {
  if (!gpu_frames_read || !gpu_path[0])
    return;
  static gpu_agg sorted[GPU_AGG_SLOTS];
  size_t n = 0;
  for (size_t i = 0; i < GPU_AGG_SLOTS; i++)
    if (gpu_aggs[i].n)
      sorted[n++] = gpu_aggs[i];
  qsort(sorted, n, sizeof(sorted[0]), gpu_agg_cmp);
  /* Seconds per timestamp unit, assuming the GPU is busy for the whole frame
   * (true when the frame is GPU-bound, which was measured for battle). */
  double frames = (double)gpu_frames_read;
  double k = gpu_ticks_total > 0 ? gpu_wall_total / gpu_ticks_total : 0;
  FILE *f = fopen(gpu_path, "w");
  if (!f)
    return;
  fprintf(f, "# GPU time per pass, averaged over %llu frames (%llu dropped, %llu frames seen)\n",
          (unsigned long long)gpu_frames_read, (unsigned long long)gpu_frames_dropped,
          (unsigned long long)gpu_frames_total);
  fprintf(f, "# wall_ms_per_frame=%.3f raw_units_per_frame=%.1f seconds_per_raw_unit=%.6g (ms = raw * this; "
             "raw is nanoseconds if this is near 1e-9)\n",
          gpu_wall_total / frames * 1000.0, gpu_ticks_total / frames, k);
  fprintf(f, "# The timer splits every pass with a timestamp, so frames run slower than normal and "
             "cost moves between passes. Rank with it; do not add the milliseconds up as a frame time.\n");
  /* The timer's own cost: a pass with at most 2 draws that happens in most
   * frames does almost no GPU work, so its time is mostly the split itself. */
  double floors[GPU_AGG_SLOTS];
  size_t nf = 0;
  double passes_per_frame = 0;
  for (size_t i = 0; i < n; i++) {
    passes_per_frame += (double)sorted[i].n / frames;
    if ((double)sorted[i].n / frames >= 0.5 && (double)sorted[i].draws / (double)sorted[i].n <= 2.0)
      floors[nf++] = sorted[i].ticks * k / (double)sorted[i].n * 1000.0;
  }
  double floor_ms = 0;
  if (nf) {
    for (size_t i = 1; i < nf; i++)
      for (size_t j = i; j > 0 && floors[j - 1] > floors[j]; j--) {
        double t = floors[j]; floors[j] = floors[j - 1]; floors[j - 1] = t;
      }
    floor_ms = floors[nf / 2];
  }
  fprintf(f, "# passes_per_frame=%.1f; small passes cost about %.3f ms each, so roughly %.1f ms of every frame "
             "is the timer itself\n", passes_per_frame, floor_ms, passes_per_frame * floor_ms);
  fprintf(f, "rank\tshare_pct\tms_per_frame\tpasses_per_frame\tdraws_per_pass\ttype\trt0\tdepth\tps\tvs\tcs\tfirst_order\n");
  for (size_t i = 0; i < n; i++) {
    const gpu_agg *a = &sorted[i];
    char ps[13], vs[13], cs[13], rt[48], ds[32];
    gpu_short_id(a->ps, ps);
    gpu_short_id(a->vs, vs);
    gpu_short_id(a->cs, cs);
    snprintf(rt, sizeof(rt), "%ux%u:f%u:x%u", a->w, a->h, a->fmt, a->rt_count);
    snprintf(ds, sizeof(ds), "%ux%u", a->dw, a->dh);
    fprintf(f, "%zu\t%.2f\t%.3f\t%.2f\t%.1f\t%s\t%s\t%s\t%s\t%s\t%s\t%u\n", i + 1,
            gpu_ticks_total > 0 ? 100.0 * a->ticks / gpu_ticks_total : 0.0, a->ticks * k / frames * 1000.0,
            (double)a->n / frames, (double)a->draws / (double)a->n, a->type == GPU_PASS_CS ? "cs" : "draw", rt, ds,
            ps, vs, cs, a->first_order);
  }
  fclose(f);
  if (gpu_worst_path[0]) {
    FILE *w = fopen(gpu_worst_path, "w");
    if (w) {
      fprintf(w, "# The %d slowest frames since the last F9 (frames over %.0f ms are skipped). Passes in frame order.\n",
              GPU_WORST, GPU_WORST_MAX_MS);
      fprintf(w, "frame\ttotal_ms\torder\tms\ttype\trt0\tdepth\tps\tvs\tcs\tdraws\n");
      int order[GPU_WORST];
      for (int i = 0; i < GPU_WORST; i++)
        order[i] = i;
      for (int i = 1; i < GPU_WORST; i++)
        for (int j = i; j > 0 && gpu_worst[order[j - 1]].total < gpu_worst[order[j]].total; j--) {
          int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
      for (int r = 0; r < GPU_WORST; r++) {
        const gpu_worst_frame *wf = &gpu_worst[order[r]];
        if (!wf->n)
          continue;
        for (uint32_t i = 0; i < wf->n; i++) {
          const gpu_pass *p = &wf->pass[i];
          char ps[13], vs[13], cs[13], rt[48], ds[32];
          gpu_short_id(p->ps, ps);
          gpu_short_id(p->vs, vs);
          gpu_short_id(p->cs, cs);
          snprintf(rt, sizeof(rt), "%ux%u:f%u:x%u", p->w, p->h, p->fmt, p->rt_count);
          snprintf(ds, sizeof(ds), "%ux%u", p->dw, p->dh);
          fprintf(w, "%d\t%.3f\t%u\t%.3f\t%s\t%s\t%s\t%s\t%s\t%s\t%u\n", r + 1, wf->total * k * 1000.0, i,
                  wf->d[i] * k * 1000.0, p->type == GPU_PASS_CS ? "cs" : "draw", rt, ds, ps, vs, cs, p->draws);
        }
      }
      fclose(w);
    }
  }
  log_msg("gpu passes: %llu frames, wall %.2f ms/frame, raw %.0f units/frame (k=%.4g), %llu dropped, written to %s",
          (unsigned long long)gpu_frames_read, gpu_wall_total / frames * 1000.0, gpu_ticks_total / frames, k,
          (unsigned long long)gpu_frames_dropped, gpu_path);
}

/* Called at every Present, before the real one. */
static void gpu_pass_present(void) {
  if (!cfg_gpu_passes || !gpu_ctx)
    return;
  void *ctx = gpu_ctx;
  if (!gpu_device) {
    ((get_device_fn)VTBL_METHOD(ctx, ID3D11DeviceChild, GetDevice))(ctx, &gpu_device);
    if (!gpu_device)
      return;
    /* GetDevice added a reference; keep it, because the timer uses the device
     * for the rest of the run. */
  }
  double now = qpc_seconds();
  if (gpu_started) {
    gpu_frame *fr = &gpu_frames[gpu_cur];
    gpu_close_pass(ctx, fr);
    fr->wall = now - fr->wall_start;
    fr->done = 1;
    gpu_frames_total++;
  }
  int reset_down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
  if (reset_down && !gpu_reset_was_down) {
    memset(gpu_aggs, 0, sizeof(gpu_aggs));
    memset(gpu_worst, 0, sizeof(gpu_worst));
    gpu_agg_count = 0;
    gpu_ticks_total = gpu_wall_total = 0;
    gpu_frames_read = gpu_frames_dropped = gpu_frames_total = 0;
    log_msg("gpu passes: counts cleared (F9)");
  }
  gpu_reset_was_down = reset_down;
  gpu_harvest(ctx);
  gpu_cur = (gpu_cur + 1) % GPU_RING;
  gpu_frame *fr = &gpu_frames[gpu_cur];
  if (fr->done)
    gpu_frames_dropped++; /* the ring wrapped before this frame was read */
  gpu_frame_begin(ctx, fr);
  gpu_started = 1;
  if (now >= gpu_next_dump) {
    if (gpu_next_dump)
      gpu_dump();
    gpu_next_dump = now + GPU_DUMP_SECONDS;
  }
}

static void gpu_pass_setup(void) {
  cfg_gpu_passes = env_int("SC6VRR_GPU_PASSES", 0);
  if (!cfg_gpu_passes)
    return;
  if (!shader_dir[0]) {
    log_msg("gpu passes: needs SC6VRR_SHADER_DIR (the hooks that give shaders their IDs); timer off");
    cfg_gpu_passes = 0;
    return;
  }
  if (!cfg_str("SC6VRR_GPU_FILE", gpu_path, sizeof(gpu_path))) {
    DWORD n = GetModuleFileNameA(NULL, gpu_path, sizeof(gpu_path));
    char *slash = (n && n < sizeof(gpu_path)) ? strrchr(gpu_path, '\\') : NULL;
    if (!slash || (size_t)(slash + 1 - gpu_path) + sizeof("sc6vrr-gpu-passes.tsv") > sizeof(gpu_path)) {
      log_msg("gpu passes: no output path; timer off");
      cfg_gpu_passes = 0;
      return;
    }
    strcpy(slash + 1, "sc6vrr-gpu-passes.tsv");
  }
  size_t len = strlen(gpu_path);
  if (len > 4 && len + 8 < sizeof(gpu_worst_path) && !strcmp(gpu_path + len - 4, ".tsv"))
    snprintf(gpu_worst_path, sizeof(gpu_worst_path), "%.*s-worst.tsv", (int)(len - 4), gpu_path);
  log_msg("gpu passes: on (mode %d), output %s, slowest frames in %s, F9 clears the counts", cfg_gpu_passes,
          gpu_path, gpu_worst_path[0] ? gpu_worst_path : "(none)");
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
  gpu_pass_present();
  prepass_present();
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
      hook_vs_samplers_set, hook_cs_samplers_set, hook_get_data, hook_map, hook_flush,
      hook_query_begin, hook_query_end};
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

/* ---------- blocking wait for the GPU event query ----------------------- */

/* FUN_1412111a0 is UE4's FD3D11EventQuery::WaitForCompletion. After it issues
 * an event query (a marker that completes when the GPU has finished all work
 * submitted so far), it calls ID3D11DeviceContext::GetData in a loop until the
 * query completes, so the render thread uses a full CPU core while it waits.
 * D3D11 has no blocking wait for a query, but DXMT's Flush1(type, event)
 * (ID3D11DeviceContext3) sets a Win32 event when the GPU reaches the current
 * command, so this hook waits on that event instead of polling.
 * The hook always ends in the original function, which polls and then finds
 * the query done at once, so a wrong or late event never changes the result. */
#define ADDR_QUERY_WAIT 0x1412111a0ull
#define ADDR_QUERY_WAIT_STAT_TIME  0x14435185cull /* the game's own wait-time counters */
#define ADDR_QUERY_WAIT_STAT_COUNT 0x14435186cull
#define QUERY_WAIT_STEP_MS 1 /* the wait is cut into steps of this length */

/* b4e3c01d-e79e-4637-91b2-510e9f4c9b8f; defined here so the build needs no dxguid library. */
static const GUID kIidDeviceContext3 = {0xb4e3c01d, 0xe79e, 0x4637, {0x91, 0xb2, 0x51, 0x0e, 0x9f, 0x4c, 0x9b, 0x8f}};
static void (*orig_query_wait)(void *);
static HANDLE query_wait_event;
static ID3D11DeviceContext *query_wait_ctx; /* the context that query_wait_ctx3 belongs to */
static ID3D11DeviceContext3 *query_wait_ctx3;
static volatile LONG query_wait_disabled;
static unsigned query_wait_count, query_wait_by_poll, query_wait_poll_run, query_wait_slow_logged;
static unsigned long long query_wait_us_total;

static void hook_query_wait(void *self) {
  char *rhi = *(char **)((char *)self + 0x30); /* the RHI object that holds the device context */
  ID3D11DeviceContext *ctx = rhi ? *(ID3D11DeviceContext **)(rhi + 0x70) : NULL;
  ID3D11Asynchronous *query = *(ID3D11Asynchronous **)((char *)self + 0x38);
  if (query_wait_disabled || !ctx || !query || !query_wait_event) {
    orig_query_wait(self);
    return;
  }
  BOOL done = 0;
  HRESULT hr = ctx->lpVtbl->GetData(ctx, (ID3D11Asynchronous *)query, &done, sizeof(done), 0);
  if (hr != S_FALSE) { /* already done, or an error that the original reports */
    if (hr == S_OK && done)
      return;
    orig_query_wait(self);
    return;
  }
  if (ctx != query_wait_ctx) {
    if (query_wait_ctx3)
      query_wait_ctx3->lpVtbl->Release(query_wait_ctx3);
    query_wait_ctx3 = NULL;
    query_wait_ctx = ctx;
    if (FAILED(ctx->lpVtbl->QueryInterface(ctx, &kIidDeviceContext3, (void **)&query_wait_ctx3)))
      query_wait_ctx3 = NULL;
  }
  if (!query_wait_ctx3) {
    log_msg("query wait: context has no ID3D11DeviceContext3; using the game's polling loop");
    InterlockedExchange(&query_wait_disabled, 1);
    orig_query_wait(self);
    return;
  }
  LARGE_INTEGER t0, t1, freq;
  QueryPerformanceCounter(&t0);
  ResetEvent(query_wait_event);
  query_wait_ctx3->lpVtbl->Flush1(query_wait_ctx3, D3D11_CONTEXT_TYPE_ALL, query_wait_event);
  /* Wait for the event in 1 ms steps, up to the original's 0.5 s limit. After
   * each step, ask the query itself. DXMT delivers the event through a run
   * loop thread, and in the 2026-10-31 run it often arrived more than 20 ms
   * late. The 1 ms step keeps such a late event from stalling the render
   * thread: the query check ends the wait as soon as the GPU is done. */
  DWORD w = WAIT_TIMEOUT;
  DWORD waited = 0;
  int by_poll = 0;
  while (w == WAIT_TIMEOUT && waited < 500) {
    w = WaitForSingleObject(query_wait_event, QUERY_WAIT_STEP_MS);
    waited += QUERY_WAIT_STEP_MS;
    if (w == WAIT_TIMEOUT) {
      BOOL again = 0;
      if (ctx->lpVtbl->GetData(ctx, (ID3D11Asynchronous *)query, &again, sizeof(again), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && again) {
        by_poll = 1;
        break;
      }
    }
  }
  QueryPerformanceCounter(&t1);
  QueryPerformanceFrequency(&freq);
  unsigned long long us = (unsigned long long)(t1.QuadPart - t0.QuadPart) * 1000000ull / (unsigned long long)freq.QuadPart;
  query_wait_us_total += us;
  query_wait_count++;
  if (by_poll)
    query_wait_by_poll++;
  if (us > 50000 && query_wait_slow_logged++ < 10)
    log_msg("query wait: slow wait, %llu us (%s)", us, by_poll ? "found done by the query check" : "event");
  /* The event never arrived in 300 waits in a row: this DXMT build probably
   * does not send it, so the 1 ms steps would only add delay. Stop using them. */
  query_wait_poll_run = by_poll ? query_wait_poll_run + 1 : 0;
  if (query_wait_poll_run >= 300) {
    log_msg("query wait: event missing in %u waits in a row; back to the game's polling loop", query_wait_poll_run);
    InterlockedExchange(&query_wait_disabled, 1);
  }
  if (query_wait_count % 3600 == 0)
    log_msg("query wait: %u waits, %u ended by the event, %.2f ms on average", query_wait_count,
            query_wait_count - query_wait_by_poll, query_wait_us_total / 1000.0 / query_wait_count);
  /* The original polls once more and returns at once when the query is done.
   * Its own time counters stay unchanged for waits that this hook handled, so
   * add this wait to them the way the original does. */
  *(DWORD *)ADDR(ADDR_QUERY_WAIT_STAT_TIME) += (DWORD)t1.LowPart - (DWORD)t0.LowPart;
  *(DWORD *)ADDR(ADDR_QUERY_WAIT_STAT_COUNT) += 1;
  orig_query_wait(self);
}

static void install_query_wait(void) {
  if (!bytes_match(ADDR(ADDR_QUERY_WAIT), "488bc4535657415641574883ec700f2970b84533ff0f2978a84c8bf1")) {
    log_msg("query wait: function prologue mismatch; blocking wait disabled");
    return;
  }
  query_wait_event = CreateEventW(NULL, FALSE, FALSE, NULL);
  MH_STATUS st = query_wait_event ? MH_CreateHook(ADDR(ADDR_QUERY_WAIT), hook_query_wait, (void **)&orig_query_wait)
                                  : MH_ERROR_MEMORY_ALLOC;
  if (st == MH_OK)
    st = MH_EnableHook(ADDR(ADDR_QUERY_WAIT));
  log_msg("query wait: blocking wait hook %s (%d)", st == MH_OK ? "installed" : "failed", st);
}

/* ---------- setup -------------------------------------------------------- */

/* ---------- offline login ------------------------------------------------ */
/* SC6VRR_OFFLINE_LOGIN=1 skips the online login that the title screen starts
 * after Start, which waits on Bandai Namco's servers. UE4 calls the native
 * code of a reflected (Blueprint-callable) function through the pointer at
 * UFunction+0xb0. A watcher thread swaps that pointer for RequestLogin to
 * offline_request_login. This is a data write, not a code patch, so FEX's
 * cache of translated code does not matter. Static analysis and first test:
 * Codex, 2026-10-31 (/tmp/sc6-offline-fbeng0ir/offline-findings.md). */
#define ADDR_REQUEST_LOGIN_UFUNC  0x144157f70ull /* UFunction* RequestLogin; nonzero once the class is built */
#define ADDR_REQUEST_LOGIN_NATIVE 0x140bdafa0ull /* original native wrapper (reads 2 bools, calls LOGIN_IMPL) */
#define ADDR_FRAME_STEP           0x140f73570ull /* FFrame::Step(frame, context, result) */
#define ADDR_FRAME_STEP_EXPLICIT  0x140f735a0ull /* FFrame::StepExplicitProperty(frame, result, property) */
#define ADDR_LOGIN_IMPL           0x14051d420ull /* native login task: (self, IsInterraptFlow, IsStartUp) */
#define ADDR_SESSION_CLASS        0x140a51bf0ull /* returns the UClass of the session object */
#define ADDR_GET_SESSION          0x14050bab0ull /* returns the session object of that class */
#define ADDR_LOGIN_NOTIFY         0x140506130ull /* sends (success, error) to the waiting Blueprint */
#define UFUNC_NATIVE_OFFSET 0xb0

static volatile LONG offline_login_calls, offline_login_fallbacks;

/* Reads one bool argument the way the game's wrapper does (P_GET_UBOOL).
 * FFrame+0x20 is the bytecode pointer, +0x18 the context object, +0x80 the
 * property chain for calls from native code. */
static int frame_read_bool(uint8_t *frame) {
  int value = 0;
  if (*(uintptr_t *)(frame + 0x20)) {
    ((void (*)(void *, uintptr_t, int *))ADDR(ADDR_FRAME_STEP))(frame, *(uintptr_t *)(frame + 0x18), &value);
  } else {
    uintptr_t property = *(uintptr_t *)(frame + 0x80);
    *(uintptr_t *)(frame + 0x80) = *(uintptr_t *)(property + 0x28); /* UField::Next */
    ((void (*)(void *, int *, uintptr_t))ADDR(ADDR_FRAME_STEP_EXPLICIT))(frame, &value, property);
  }
  return value != 0;
}

/* Replaces RequestLogin's wrapper: it consumes the same two arguments, marks
 * the session offline (session+0x128 = 0 makes IsOfflineMode true), and
 * reports success (1, error 0) to the waiting Blueprint without contacting a
 * server. If the session cannot be validated, it runs the original login. */
static volatile LONG title_login_seen; /* read by the title skip */
static int cfg_offline_login;

static void offline_request_login(uint8_t *self, uint8_t *frame, void *result) {
  InterlockedExchange(&title_login_seen, 1);
  if (!cfg_offline_login) { /* installed only for SC6VRR_SKIP_TITLE */
    ((void (*)(void *, void *, void *))ADDR(ADDR_REQUEST_LOGIN_NATIVE))(self, frame, result);
    return;
  }
  int interrupt_flow = frame_read_bool(frame);
  int startup = frame_read_bool(frame);
  uintptr_t *code = (uintptr_t *)(frame + 0x20);
  *code += *code != 0; /* step past the end-of-arguments byte */
  self[0x98] = (uint8_t)startup;

  uintptr_t (*session_class_fn)(void) = (uintptr_t (*)(void))ADDR(ADDR_SESSION_CLASS);
  uintptr_t session = ((uintptr_t (*)(uintptr_t))ADDR(ADDR_GET_SESSION))(session_class_fn());
  if (session) {
    /* Same IsA check as the game's login code at 0x14051d473. */
    uintptr_t expected = session_class_fn();
    uintptr_t cls = *(uintptr_t *)(session + 0x10);
    int32_t depth = *(int32_t *)(expected + 0x90);
    if (depth >= 0 && depth <= *(int32_t *)(cls + 0x90) &&
        ((uintptr_t *)*(uintptr_t *)(cls + 0x88))[depth] == expected + 0x88) {
      *(uint8_t *)(session + 0x128) = 0;
      ((uint64_t (*)(void *, uint8_t, uint8_t))ADDR(ADDR_LOGIN_NOTIFY))(self + 0x48, 1, 0);
      log_msg("offline login: RequestLogin answered offline (call %ld, startup=%d)",
              InterlockedIncrement(&offline_login_calls), startup);
      return;
    }
  }
  log_msg("offline login: session not found (%p); running the original online login (fallback %ld)",
          (void *)session, InterlockedIncrement(&offline_login_fallbacks));
  ((void (*)(void *, uint8_t, uint8_t))ADDR(ADDR_LOGIN_IMPL))(self, (uint8_t)interrupt_flow, (uint8_t)startup);
}

/* ---------- fast imports (SC6VRR_FAST_IMPORTS) ---------------------------- */
/* In the FEX bottle, Wine's system DLLs are native ARM64 code. Every call
 * from the game's x86 code into them leaves the emulator and comes back, which
 * costs about 30 ns (measured 2026-10-31: TlsGetValue 29 ns in FEX, 8.4 ns in
 * Rosetta, 2 ns for a direct TEB read). UE4's memory allocator calls
 * TlsGetValue on every allocation and free, and string code calls towupper
 * per character. This option points the exe's import table entries for these
 * functions at x86 versions in this DLL, which FEX runs without leaving the
 * emulator. Each version handles only the common case and calls the original
 * for anything else. */
#define TEB_LAST_ERROR     0x68   /* TEB.LastErrorValue */
#define TEB_TLS_SLOTS      0x1480 /* TEB.TlsSlots[64] */
#define TEB_TLS_EXPANSION  0x1780 /* TEB.TlsExpansionSlots (pointer to 1024 slots) */
static FARPROC orig_tls_get_value;
static wint_t (__cdecl *orig_towupper)(wint_t);
static wint_t (__cdecl *orig_towlower)(wint_t);

static LPVOID WINAPI fast_tls_get_value(DWORD index) {
  void *value;
  if (index < 64) {
    value = (void *)__readgsqword(TEB_TLS_SLOTS + index * 8);
  } else {
    void **expansion = (void **)__readgsqword(TEB_TLS_EXPANSION);
    if (index >= 64 + 1024 || !expansion)
      return ((LPVOID(WINAPI *)(DWORD))orig_tls_get_value)(index); /* sets the error code itself */
    value = expansion[index - 64];
  }
  __writegsdword(TEB_LAST_ERROR, ERROR_SUCCESS); /* the real function does this too */
  return value;
}

static wint_t __cdecl fast_towupper(wint_t c) {
  if (c < 0x80)
    return c >= 'a' && c <= 'z' ? c - 0x20 : c;
  return orig_towupper(c);
}

static wint_t __cdecl fast_towlower(wint_t c) {
  if (c < 0x80)
    return c >= 'A' && c <= 'Z' ? c + 0x20 : c;
  return orig_towlower(c);
}

/* SC6VRR_FAST_LOCKS: the same idea for EnterCriticalSection,
 * LeaveCriticalSection and TryEnterCriticalSection (60 ns per enter+leave in
 * FEX, 13.6 ns with these; stress-tested with 6 threads mixing these and the
 * originals in both bottles, 2026-10-31). */
/* x86 fast paths for Win32 critical sections, compatible with Wine's
 * RTL_CRITICAL_SECTION rules: LockCount is -1 when free and counts holders
 * plus waiters from 0; the owner's recursion is in RecursionCount. Contended
 * cases go to the original functions, which spin, wait and wake. */
#define TEB_THREAD_ID 0x48 /* TEB.ClientId.UniqueThread */
static void (WINAPI *orig_enter_cs)(CRITICAL_SECTION *);
static void (WINAPI *orig_leave_cs)(CRITICAL_SECTION *);
static BOOL (WINAPI *orig_try_enter_cs)(CRITICAL_SECTION *);

static void WINAPI fast_enter_cs(CRITICAL_SECTION *cs) {
  HANDLE self = (HANDLE)(ULONG_PTR)__readgsdword(TEB_THREAD_ID);
  if (InterlockedCompareExchange(&cs->LockCount, 0, -1) == -1) {
    cs->OwningThread = self;
    cs->RecursionCount = 1;
    return;
  }
  if (cs->OwningThread == self) {
    InterlockedIncrement(&cs->LockCount);
    cs->RecursionCount++;
    return;
  }
  orig_enter_cs(cs);
}

static void WINAPI fast_leave_cs(CRITICAL_SECTION *cs) {
  if (cs->RecursionCount > 1) {
    cs->RecursionCount--;
    InterlockedDecrement(&cs->LockCount);
    return;
  }
  if (cs->RecursionCount != 1) {
    orig_leave_cs(cs); /* not held: let the original report it */
    return;
  }
  HANDLE self = cs->OwningThread;
  cs->OwningThread = 0;
  cs->RecursionCount = 0;
  if (InterlockedCompareExchange(&cs->LockCount, -1, 0) == 0)
    return; /* nobody waits */
  /* Someone waits: restore the held state and let the original release and wake. */
  cs->OwningThread = self;
  cs->RecursionCount = 1;
  orig_leave_cs(cs);
}

static BOOL WINAPI fast_try_enter_cs(CRITICAL_SECTION *cs) {
  HANDLE self = (HANDLE)(ULONG_PTR)__readgsdword(TEB_THREAD_ID);
  if (InterlockedCompareExchange(&cs->LockCount, 0, -1) == -1) {
    cs->OwningThread = self;
    cs->RecursionCount = 1;
    return TRUE;
  }
  if (cs->OwningThread == self) {
    InterlockedIncrement(&cs->LockCount);
    cs->RecursionCount++;
    return TRUE;
  }
  return FALSE;
}

/* Replaces the main exe's import of dll!name (dll matched without case and
 * by prefix, so "api-ms-win-crt-string" matches its versioned name). Returns
 * the old target, or NULL if the import was not found. */
static void *patch_exe_import(const char *dll, const char *name, void *replacement) {
  uint8_t *base = (uint8_t *)GetModuleHandleW(NULL);
  IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
  IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
    if (_strnicmp((char *)(base + d->Name), dll, strlen(dll)))
      continue;
    IMAGE_THUNK_DATA64 *names = (IMAGE_THUNK_DATA64 *)(base + d->OriginalFirstThunk);
    IMAGE_THUNK_DATA64 *slots = (IMAGE_THUNK_DATA64 *)(base + d->FirstThunk);
    for (; names->u1.AddressOfData; names++, slots++) {
      if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
        continue;
      if (strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, name))
        continue;
      DWORD old;
      void *previous = (void *)slots->u1.Function;
      if (!VirtualProtect(&slots->u1.Function, 8, PAGE_READWRITE, &old))
        return NULL;
      slots->u1.Function = (ULONGLONG)replacement;
      VirtualProtect(&slots->u1.Function, 8, old, &old);
      return previous;
    }
  }
  return NULL;
}

/* SC6VRR_FAST_CRT: x86 versions of the next most-called imports (counts
 * from SC6VRR_COUNT_IMPORTS for one boot to the main menu, 2026-10-31):
 * memcpy 43.7 M, GetCurrentThreadId 19.4 M, toupper 17.8 M, memset 5.8 M,
 * _stricmp 4.1 M, _wcsnicmp 3.5 M, memmove 1.4 M. Copies and fills of more
 * than FAST_MEM_MAX bytes still go to the native code, which is faster for
 * big blocks. The string functions handle ASCII only (the C locale's rules)
 * and give the whole call to the original when they meet any other
 * character. */
#define FAST_MEM_MAX 64
static void *(__cdecl *orig_memcpy)(void *, const void *, size_t);
static void *(__cdecl *orig_memmove)(void *, const void *, size_t);
static void *(__cdecl *orig_memset)(void *, int, size_t);
static int (__cdecl *orig_toupper)(int);
static int (__cdecl *orig_stricmp)(const char *, const char *);
static int (__cdecl *orig_wcsnicmp)(const wchar_t *, const wchar_t *, size_t);

/* Small copy through registers and a stack buffer, so overlapping source and
 * destination are safe (MSVC's memcpy is a memmove, and code relies on it).
 * The constant-size __builtin_memcpy calls compile to plain moves. */
static inline void small_move(uint8_t *d, const uint8_t *s, size_t n) {
  uint8_t tmp[FAST_MEM_MAX];
  size_t i = 0;
  for (; i + 8 <= n; i += 8)
    __builtin_memcpy(tmp + i, s + i, 8);
  for (; i < n; i++)
    tmp[i] = s[i];
  for (i = 0; i + 8 <= n; i += 8)
    __builtin_memcpy(d + i, tmp + i, 8);
  for (; i < n; i++)
    d[i] = tmp[i];
}

__attribute__((optimize("no-tree-loop-distribute-patterns")))
static void *__cdecl fast_memcpy(void *dst, const void *src, size_t n) {
  if (n > FAST_MEM_MAX)
    return orig_memcpy(dst, src, n);
  small_move(dst, src, n);
  return dst;
}

__attribute__((optimize("no-tree-loop-distribute-patterns")))
static void *__cdecl fast_memmove(void *dst, const void *src, size_t n) {
  if (n > FAST_MEM_MAX)
    return orig_memmove(dst, src, n);
  small_move(dst, src, n);
  return dst;
}

__attribute__((optimize("no-tree-loop-distribute-patterns")))
static void *__cdecl fast_memset(void *dst, int c, size_t n) {
  if (n > FAST_MEM_MAX)
    return orig_memset(dst, c, n);
  uint8_t *d = dst;
  uint64_t v = 0x0101010101010101ull * (uint8_t)c;
  size_t i = 0;
  for (; i + 8 <= n; i += 8)
    __builtin_memcpy(d + i, &v, 8);
  for (; i < n; i++)
    d[i] = (uint8_t)c;
  return dst;
}

static DWORD WINAPI fast_get_current_thread_id(void) { return __readgsdword(TEB_THREAD_ID); }

static int __cdecl fast_toupper(int c) {
  if (c >= 0 && c < 0x80)
    return c >= 'a' && c <= 'z' ? c - 0x20 : c;
  return orig_toupper(c);
}

static int __cdecl fast_stricmp(const char *a, const char *b) {
  for (size_t i = 0;; i++) {
    unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
    if ((x | y) & 0x80)
      return orig_stricmp(a, b);
    if (x >= 'A' && x <= 'Z') x += 0x20;
    if (y >= 'A' && y <= 'Z') y += 0x20;
    if (x != y || !x)
      return (int)x - (int)y;
  }
}

static int __cdecl fast_wcsnicmp(const wchar_t *a, const wchar_t *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    wchar_t x = a[i], y = b[i];
    if (x >= 0x80 || y >= 0x80)
      return orig_wcsnicmp(a, b, n);
    if (x >= 'A' && x <= 'Z') x += 0x20;
    if (y >= 'A' && y <= 'Z') y += 0x20;
    if (x != y || !x)
      return (int)x - (int)y;
  }
  return 0;
}

/* Battle counts (Training mode, 2026-10-31): sqrtf 2252 calls per frame,
 * _finite 192, _isnan 88. sqrtss gives exactly the CRT's result (IEEE square
 * root is correctly rounded); negative and NaN inputs go to the original,
 * which also sets errno. _finite and _isnan are bit tests. powf, sinf, cosf,
 * expf, atan2f and acosf are left alone: another implementation can differ in
 * the last bit, and the battle logic may depend on exact float results. */
static float (__cdecl *orig_sqrtf)(float);
static float __cdecl fast_sqrtf(float x) {
  if (!(x >= 0.0f)) /* negative or NaN */
    return orig_sqrtf(x);
  float r;
  __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
  return r;
}
static int __cdecl fast_finite(double x) {
  uint64_t b;
  __builtin_memcpy(&b, &x, 8);
  return ((b >> 52) & 0x7ff) != 0x7ff;
}
static int __cdecl fast_isnan(double x) {
  uint64_t b;
  __builtin_memcpy(&b, &x, 8);
  return ((b >> 52) & 0x7ff) == 0x7ff && (b & 0xfffffffffffffull) != 0;
}

static void install_fast_crt(void) {
  struct { const char *dll, *name; void *fast; void **orig; } list[] = {
    {"VCRUNTIME140.dll", "memcpy", (void *)fast_memcpy, (void **)&orig_memcpy},
    {"VCRUNTIME140.dll", "memmove", (void *)fast_memmove, (void **)&orig_memmove},
    {"VCRUNTIME140.dll", "memset", (void *)fast_memset, (void **)&orig_memset},
    {"KERNEL32.dll", "GetCurrentThreadId", (void *)fast_get_current_thread_id, NULL},
    {"api-ms-win-crt-string", "toupper", (void *)fast_toupper, (void **)&orig_toupper},
    {"api-ms-win-crt-string", "_stricmp", (void *)fast_stricmp, (void **)&orig_stricmp},
    {"api-ms-win-crt-string", "_wcsnicmp", (void *)fast_wcsnicmp, (void **)&orig_wcsnicmp},
    {"api-ms-win-crt-math", "sqrtf", (void *)fast_sqrtf, (void **)&orig_sqrtf},
    {"api-ms-win-crt-math", "_finite", (void *)fast_finite, NULL},
    {"api-ms-win-crt-math", "_isnan", (void *)fast_isnan, NULL},
  };
  int done = 0;
  for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); i++) {
    void *previous = patch_exe_import(list[i].dll, list[i].name, list[i].fast);
    if (list[i].orig)
      *list[i].orig = previous;
    if (previous)
      done++;
    else
      log_msg("fast crt: %s!%s not imported; left alone", list[i].dll, list[i].name);
  }
  log_msg("fast crt: %d of %d imports now run as x86 code in this DLL", done, (int)(sizeof(list) / sizeof(list[0])));
}

/* SC6VRR_COUNT_IMPORTS=1 (diagnostics): every code import of the exe goes
 * through a 14-byte stub, "lock inc qword [counter]; jmp [target]", so the
 * log can list the most-called imports. A jump (not a call) leaves the stack
 * and return address untouched. Imports whose target is not executable
 * memory (data imports) are left alone. The counts are logged at the scene
 * change to "mainmenu" (import_counts_dump). */
#define COUNT_MAX 2048
typedef struct { uint64_t count; void *target; const char *dll, *name; } import_count;
static import_count *import_counts;
static int import_count_n;

static double count_window_start;
static DWORD WINAPI import_count_keys(LPVOID unused);

static void install_import_counters(void) {
  uint8_t *base = (uint8_t *)GetModuleHandleW(NULL);
  IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
  IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  uint8_t *code = VirtualAlloc(NULL, COUNT_MAX * 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  import_counts = VirtualAlloc(NULL, COUNT_MAX * sizeof(import_count), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!code || !import_counts) {
    log_msg("count imports: allocation failed");
    return;
  }
  int skipped = 0;
  for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
    IMAGE_THUNK_DATA64 *names = (IMAGE_THUNK_DATA64 *)(base + d->OriginalFirstThunk);
    IMAGE_THUNK_DATA64 *slots = (IMAGE_THUNK_DATA64 *)(base + d->FirstThunk);
    for (; names->u1.AddressOfData && import_count_n < COUNT_MAX; names++, slots++) {
      MEMORY_BASIC_INFORMATION mbi;
      void *target = (void *)slots->u1.Function;
      if (!VirtualQuery(target, &mbi, sizeof(mbi)) ||
          !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
        skipped++;
        continue;
      }
      import_count *c = &import_counts[import_count_n];
      c->target = target;
      c->dll = (char *)(base + d->Name);
      c->name = IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)
                    ? "(ordinal)"
                    : (char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name;
      uint8_t *st = code + import_count_n * 16;
      int32_t to_count = (int32_t)((uint8_t *)&c->count - (st + 8));
      int32_t to_target = (int32_t)((uint8_t *)&c->target - (st + 14));
      if ((int64_t)((uint8_t *)&c->count - (st + 8)) != to_count) {
        log_msg("count imports: counters out of rel32 reach; stopped");
        return;
      }
      st[0] = 0xF0; st[1] = 0x48; st[2] = 0xFF; st[3] = 0x05; /* lock inc qword [rip+disp32] */
      memcpy(st + 4, &to_count, 4);
      st[8] = 0xFF; st[9] = 0x25; /* jmp qword [rip+disp32] */
      memcpy(st + 10, &to_target, 4);
      DWORD old;
      VirtualProtect(&slots->u1.Function, 8, PAGE_READWRITE, &old);
      slots->u1.Function = (ULONGLONG)st;
      VirtualProtect(&slots->u1.Function, 8, old, &old);
      import_count_n++;
    }
  }
  FlushInstructionCache(GetCurrentProcess(), code, COUNT_MAX * 16);
  count_window_start = qpc_seconds();
  HANDLE h = CreateThread(NULL, 0, import_count_keys, NULL, 0, NULL);
  if (h)
    CloseHandle(h);
  log_msg("count imports: %d imports counted, %d data imports skipped; F9 clears, F8 logs the top 40", import_count_n,
          skipped);
}

static int import_count_cmp(const void *a, const void *b) {
  uint64_t x = ((const import_count *)a)->count, y = ((const import_count *)b)->count;
  return x < y ? 1 : x > y ? -1 : 0;
}

static LONG64 count_window_frames;

static void import_counts_dump(const char *why) {
  if (!import_count_n)
    return;
  static import_count sorted[COUNT_MAX];
  memcpy(sorted, import_counts, import_count_n * sizeof(import_count));
  qsort(sorted, import_count_n, sizeof(import_count), import_count_cmp);
  double secs = qpc_seconds() - count_window_start;
  LONG64 frames = g_frames - count_window_frames;
  log_msg("count imports: top imports over %.1f s and %lld frames (%s)", secs, (long long)frames, why);
  for (int i = 0; i < 40 && i < import_count_n && sorted[i].count; i++)
    log_msg("count imports: %10llu calls %10.0f/s %8.1f/frame %s!%s", (unsigned long long)sorted[i].count,
            sorted[i].count / (secs > 0 ? secs : 1), frames > 0 ? (double)sorted[i].count / frames : 0.0,
            sorted[i].dll, sorted[i].name);
}

static void import_counts_reset(void) {
  for (int i = 0; i < import_count_n; i++)
    InterlockedExchange64((volatile LONG64 *)&import_counts[i].count, 0);
  count_window_start = qpc_seconds();
  count_window_frames = g_frames;
  log_msg("count imports: counts cleared by F9");
}

/* F9 clears the counts and F8 logs the top imports since then, the same keys
 * as the sampler. Polled every 50 ms by a helper thread. */
static DWORD WINAPI import_count_keys(LPVOID unused) {
  (void)unused;
  int f9_was = 0, f8_was = 0;
  for (;;) {
    Sleep(50);
    int f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0, f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f9 && !f9_was)
      import_counts_reset();
    if (f8 && !f8_was)
      import_counts_dump("F8");
    f9_was = f9;
    f8_was = f8;
  }
  return 0;
}

static void install_fast_locks(void) {
  orig_enter_cs = (void(WINAPI *)(CRITICAL_SECTION *))patch_exe_import("KERNEL32.dll", "EnterCriticalSection", (void *)fast_enter_cs);
  orig_leave_cs = (void(WINAPI *)(CRITICAL_SECTION *))patch_exe_import("KERNEL32.dll", "LeaveCriticalSection", (void *)fast_leave_cs);
  orig_try_enter_cs = (BOOL(WINAPI *)(CRITICAL_SECTION *))patch_exe_import("KERNEL32.dll", "TryEnterCriticalSection", (void *)fast_try_enter_cs);
  if (!orig_enter_cs || !orig_leave_cs || !orig_try_enter_cs) {
    /* All three or none: a lock taken by one version must be released by a
     * compatible one, and a missing original would crash the fallback. */
    if (orig_enter_cs) patch_exe_import("KERNEL32.dll", "EnterCriticalSection", (void *)orig_enter_cs);
    if (orig_leave_cs) patch_exe_import("KERNEL32.dll", "LeaveCriticalSection", (void *)orig_leave_cs);
    if (orig_try_enter_cs) patch_exe_import("KERNEL32.dll", "TryEnterCriticalSection", (void *)orig_try_enter_cs);
    log_msg("fast locks: an import was missing; all three restored");
    return;
  }
  log_msg("fast locks: Enter/Leave/TryEnterCriticalSection now run as x86 code in this DLL");
}

static void install_fast_imports(void) {
  orig_tls_get_value = (FARPROC)patch_exe_import("KERNEL32.dll", "TlsGetValue", (void *)fast_tls_get_value);
  orig_towupper = (wint_t(__cdecl *)(wint_t))patch_exe_import("api-ms-win-crt-string", "towupper", (void *)fast_towupper);
  orig_towlower = (wint_t(__cdecl *)(wint_t))patch_exe_import("api-ms-win-crt-string", "towlower", (void *)fast_towlower);
  /* A missing original would make the fallbacks crash, so undo that entry. */
  if (orig_towupper == NULL || orig_towlower == NULL || orig_tls_get_value == NULL)
    log_msg("fast imports: some imports not found (TlsGetValue %p, towupper %p, towlower %p)",
            (void *)orig_tls_get_value, (void *)orig_towupper, (void *)orig_towlower);
  else
    log_msg("fast imports: TlsGetValue, towupper and towlower now run as x86 code in this DLL");
}

/* ---------- title-screen skip --------------------------------------------- */
/* SC6VRR_SKIP_TITLE=1 presses Start for the player on the "press any button"
 * title screen. The game has a Blueprint-callable test helper,
 * EmulateTitleDecide (wrapper 0x140abcef0), that queues one emulated press
 * of the gamepad Start key (UE4 key Gamepad_Special_Right) for player 0 in
 * the game's input manager. The DLL calls that helper's code on the game
 * thread, from the UpdateTime hook that runs once per frame, because the
 * input manager has no lock. Presses start after the title Blueprint first
 * calls IsEnableTitleMovie (a sign that the title screen exists) and stop as
 * soon as the title calls RequestLogin (the step that follows Start). */
#define ADDR_TITLE_MOVIE_UFUNC    0x14415b258ull /* UFunction* IsEnableTitleMovie */
#define ADDR_SHARED_BOOL_NATIVE   0x1409e1f20ull /* its wrapper, shared with IsCeBankEnable and LoadFromSaveData */
#define ADDR_EMULATE_TITLE_DECIDE 0x1404b9e10ull /* queues a Start press for player 0 */
#define TITLE_FIRST_PRESS_S 0.3  /* delay after the title signal before the first press */
#define TITLE_PRESS_EVERY_S 0.5  /* repeat in case the title was not yet accepting input */
#define TITLE_MAX_PRESSES   40

static int cfg_skip_title;
static volatile LONG title_seen;

static void title_movie_passthrough(void *self, void *frame, void *result) {
  ((void (*)(void *, void *, void *))ADDR(ADDR_SHARED_BOOL_NATIVE))(self, frame, result);
  if (!InterlockedExchange(&title_seen, 1))
    log_msg("skip title: IsEnableTitleMovie called (returned %d); title screen is up",
            *(uint8_t *)result);
}

/* Runs on the game thread once per frame. */
static void title_tick(void) {
  static double seen_at, last_press;
  static int presses, done;
  if (cfg_skip_title != 1 || done || !title_seen)
    return;
  double now = qpc_seconds();
  if (title_login_seen || presses >= TITLE_MAX_PRESSES) {
    log_msg("skip title: stopped after %d emulated Start presses (%s)", presses,
            title_login_seen ? "RequestLogin was called" : "press limit reached");
    done = 1;
    return;
  }
  if (seen_at == 0.0)
    seen_at = now;
  if (now - seen_at < TITLE_FIRST_PRESS_S || (presses && now - last_press < TITLE_PRESS_EVERY_S))
    return;
  ((void (*)(void))ADDR(ADDR_EMULATE_TITLE_DECIDE))();
  last_press = now;
  log_msg("skip title: emulated Start press %d (%.2f s after the title signal)", ++presses, now - seen_at);
}

/* The UpdateTime hook for SC6VRR_FRAME_MODS=0: only the title tick. */
static void hook_update_time_tick(void *self) {
  orig_update_time(self);
  g_frames++;
  title_tick();
}

/* ---------- title native-call trace (SC6VRR_TRACE_TITLE) ------------------ */
/* Every call from script into native code passes through UFunction::Invoke
 * (0x140e76180: this=UFunction, object, FFrame, result). Between the scene
 * changes to "Title" and "mainmenu", this records each distinct native
 * function once, in the order of first use, so the title's setup steps can
 * be named offline from the exe's native tables. Diagnostics only. */
#define ADDR_UFUNCTION_INVOKE 0x140e76180ull
#define TRACE_MAX 768
typedef void (*ufunc_invoke_fn)(void *, void *, void *, void *);
static ufunc_invoke_fn orig_ufunc_invoke;
static volatile LONG trace_title_active;
static double trace_title_start;
static SRWLOCK trace_lock = SRWLOCK_INIT;
static struct { void *native, *ufunc, *obj, *obj_class; double t; unsigned count; } trace_rows[TRACE_MAX];
static int trace_count;

static void hook_ufunc_invoke(void *fn, void *obj, void *stack, void *result) {
  if (trace_title_active && fn) {
    void *native = *(void **)((uint8_t *)fn + UFUNC_NATIVE_OFFSET);
    AcquireSRWLockExclusive(&trace_lock);
    int i = 0;
    while (i < trace_count && trace_rows[i].native != native)
      i++;
    if (i < trace_count)
      trace_rows[i].count++;
    else if (trace_count < TRACE_MAX)
      trace_rows[trace_count++] = (typeof(trace_rows[0])){native, fn, obj, obj ? *(void **)((uint8_t *)obj + 0x10) : NULL,
                                                          qpc_seconds() - trace_title_start, 1};
    ReleaseSRWLockExclusive(&trace_lock);
  }
  orig_ufunc_invoke(fn, obj, stack, result);
}

static void trace_title_dump(const char *why) {
  InterlockedExchange(&trace_title_active, 0);
  AcquireSRWLockExclusive(&trace_lock);
  uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
  log_msg("trace: %d distinct native functions during the title (%s); addresses use Ghidra's numbering", trace_count,
          why);
  for (int i = 0; i < trace_count; i++) {
    uintptr_t n = (uintptr_t)trace_rows[i].native;
    log_msg("trace: %3d t=%7.3f n=%5u native=0x%llx ufunc=%p obj=%p class=%p", i, trace_rows[i].t,
            trace_rows[i].count, (unsigned long long)(n - base + 0x140000000ull), trace_rows[i].ufunc,
            trace_rows[i].obj, trace_rows[i].obj_class);
  }
  trace_count = 0;
  ReleaseSRWLockExclusive(&trace_lock);
}

/* With SC6VRR_TRACE_TITLE, GameInitialize's implementation (the native
 * wrapper 0x140cb3800 jumps to 0x140636c10) is timed. With SC6VRR_SAMPLE=1
 * too, the sampler clears its counts when the function starts and saves a
 * numbered snapshot when it returns, so that snapshot covers only it. */
#define ADDR_GAME_INITIALIZE_IMPL 0x140636c10ull
typedef void (*game_initialize_fn)(void *);
static game_initialize_fn orig_game_initialize;

static void hook_game_initialize(void *self) {
  if (cfg_sample) {
    InterlockedExchange(&sample_req_reset, 1);
    Sleep(20); /* let the sampler clear its counts before the work starts */
  }
  double t0 = qpc_seconds();
  orig_game_initialize(self);
  double ms = (qpc_seconds() - t0) * 1000.0;
  if (cfg_sample)
    InterlockedExchange(&sample_req_snapshot, 1);
  log_msg("trace: GameInitialize took %.1f ms on thread %lu%s", ms, (unsigned long)GetCurrentThreadId(),
          cfg_sample ? "; sampler snapshot requested" : "");
}

/* The five steps that GameInitialize's implementation runs, timed one by
 * one (with SC6VRR_TRACE_TITLE). Each takes one pointer argument. */
#define GI_STEP(n, addr, prologue)                                                     \
  static void (*orig_gi_step##n)(void *);                                              \
  static void hook_gi_step##n(void *arg) {                                             \
    double t0 = qpc_seconds();                                                         \
    orig_gi_step##n(arg);                                                              \
    log_msg("trace: GameInitialize step %d (0x%llx) took %.1f ms", n,                  \
            (unsigned long long)(addr), (qpc_seconds() - t0) * 1000.0);                \
  }
GI_STEP(1, 0x1406458a0ull, "48895c2408574883ec60")
GI_STEP(2, 0x140645dd0ull, "4055574155488d6c24b9")
GI_STEP(3, 0x140502ff0ull, "4055564154488d6c24b9")
GI_STEP(4, 0x1404fe3d0ull, "488bc45553488da868ff")
GI_STEP(5, 0x1404fc7e0ull, "488bc44889582055488b")
GI_STEP(11, 0x1406485f0ull, "488bc4554156488da848")
GI_STEP(12, 0x140647e90ull, "488bc45556488da8f8fe")
static const struct { uint64_t addr; const char *prologue; void *hook; void **orig; } gi_steps[] = {
  {0x1406458a0ull, "48895c2408574883ec60", (void *)hook_gi_step1, (void **)&orig_gi_step1},
  {0x140645dd0ull, "4055574155488d6c24b9", (void *)hook_gi_step2, (void **)&orig_gi_step2},
  {0x140502ff0ull, "4055564154488d6c24b9", (void *)hook_gi_step3, (void **)&orig_gi_step3},
  {0x1404fe3d0ull, "488bc45553488da868ff", (void *)hook_gi_step4, (void **)&orig_gi_step4},
  {0x1404fc7e0ull, "488bc44889582055488b", (void *)hook_gi_step5, (void **)&orig_gi_step5},
  /* Steps 11 and 12 are the two helpers that step 1 calls first. */
  {0x1406485f0ull, "488bc4554156488da848", (void *)hook_gi_step11, (void **)&orig_gi_step11},
  {0x140647e90ull, "488bc45556488da8f8fe", (void *)hook_gi_step12, (void **)&orig_gi_step12},
};

static void install_title_trace(void) {
  if (!bytes_match(ADDR(ADDR_UFUNCTION_INVOKE), "48895c240848896c2410488974241848897c2420")) {
    log_msg("trace: UFunction::Invoke prologue mismatch; trace off");
    return;
  }
  MH_STATUS st = MH_CreateHook(ADDR(ADDR_UFUNCTION_INVOKE), hook_ufunc_invoke, (void **)&orig_ufunc_invoke);
  if (st == MH_OK)
    st = MH_EnableHook(ADDR(ADDR_UFUNCTION_INVOKE));
  log_msg("trace: UFunction::Invoke hook %s (%d)", st == MH_OK ? "on" : "failed", st);
  if (!bytes_match(ADDR(ADDR_GAME_INITIALIZE_IMPL), "40534883ec20488bd9488b8968020000")) {
    log_msg("trace: GameInitialize prologue mismatch; not timed");
    return;
  }
  st = MH_CreateHook(ADDR(ADDR_GAME_INITIALIZE_IMPL), hook_game_initialize, (void **)&orig_game_initialize);
  if (st == MH_OK)
    st = MH_EnableHook(ADDR(ADDR_GAME_INITIALIZE_IMPL));
  log_msg("trace: GameInitialize timer %s (%d)", st == MH_OK ? "on" : "failed", st);
  for (size_t i = 0; i < sizeof(gi_steps) / sizeof(gi_steps[0]); i++) {
    if (!bytes_match(ADDR(gi_steps[i].addr), gi_steps[i].prologue)) {
      log_msg("trace: GameInitialize step %d prologue mismatch; not timed", (int)i + 1);
      continue;
    }
    st = MH_CreateHook(ADDR(gi_steps[i].addr), gi_steps[i].hook, gi_steps[i].orig);
    if (st == MH_OK)
      st = MH_EnableHook(ADDR(gi_steps[i].addr));
    if (st != MH_OK)
      log_msg("trace: GameInitialize step %d hook failed (%d)", (int)i + 1, st);
  }
}

/* ---------- scene-change log (SC6VRR_LOG_SCENES) --------------------------- */
/* GameFlowManager::ChangeScene (native wrapper 0x142f7a890) parses an FString
 * scene name and a data struct, then calls this implementation. The scene
 * names come from the game's data files, so the log shows which names the
 * boot uses. Diagnostics only: the hook changes nothing. */
#define ADDR_FLOW_CHANGE_SCENE 0x142ef6e30ull /* (manager, FString *name, data *, ?) */
typedef struct { wchar_t *data; int32_t num, max; } ue_fstring;
typedef void (*flow_change_scene_fn)(void *, ue_fstring *, void *, void *);
static flow_change_scene_fn orig_flow_change_scene;

static void hook_flow_change_scene(void *mgr, ue_fstring *name, void *data, void *extra) {
  char text[256] = "?";
  if (name && name->data && name->num > 0)
    WideCharToMultiByte(CP_UTF8, 0, name->data, -1, text, sizeof(text), NULL, NULL);
  log_msg("scene: ChangeScene \"%s\" (manager %p, title_seen=%ld, login_seen=%ld)", text, mgr, title_seen,
          title_login_seen);
  if (name && name->data && !_wcsicmp(name->data, L"mainmenu"))
    import_counts_dump("scene change to mainmenu");
  if (orig_ufunc_invoke) {
    if (!_wcsicmp(name && name->data ? name->data : L"", L"Title")) {
      trace_title_start = qpc_seconds();
      InterlockedExchange(&trace_title_active, 1);
    } else if (trace_title_active) {
      trace_title_dump(text);
    }
  }
  /* SC6VRR_SKIP_TITLE=2 (EXPERIMENTAL, BROKEN): ask for the main menu
   * instead of the title screen, once per run. Tested 2026-10-31: the menu
   * then fades in and out forever, because the title does setup that the
   * menu needs (RequestDecideMainUser, StartSaveDataCheckFlow,
   * GameInitialize, RequestStartUpTask; see SC6VRR_TRACE_TITLE). The implementation only reads and copies the name (it
   * copies at 0x142ef6ee8, and the wrapper frees its own string afterwards),
   * so a string on this stack is safe. FString.num counts the final NUL. */
  static int redirected;
  if (cfg_skip_title == 2 && !redirected && name && name->data && !_wcsicmp(name->data, L"Title")) {
    static wchar_t main_menu[] = L"mainmenu";
    ue_fstring replacement = {main_menu, (int32_t)(sizeof(main_menu) / sizeof(wchar_t)),
                              (int32_t)(sizeof(main_menu) / sizeof(wchar_t))};
    redirected = 1;
    log_msg("skip title: ChangeScene \"Title\" replaced by \"mainmenu\"");
    orig_flow_change_scene(mgr, &replacement, data, extra);
    return;
  }
  orig_flow_change_scene(mgr, name, data, extra);
}

static void install_scene_log(void) {
  if (!bytes_match(ADDR(ADDR_FLOW_CHANGE_SCENE), "488bc44c8948204c89401855564156")) {
    log_msg("scene: ChangeScene prologue mismatch; scene log off");
    return;
  }
  MH_STATUS st = MH_CreateHook(ADDR(ADDR_FLOW_CHANGE_SCENE), hook_flow_change_scene, (void **)&orig_flow_change_scene);
  if (st == MH_OK)
    st = MH_EnableHook(ADDR(ADDR_FLOW_CHANGE_SCENE));
  log_msg("scene: ChangeScene log %s (%d)", st == MH_OK ? "on" : "failed", st);
}

/* ---------- UFunction pointer swaps --------------------------------------- */
/* Waits until the engine builds each UFunction, then swaps its native
 * pointer, and then exits. It must not keep running: at shutdown UE4 frees
 * the UFunctions while the static pointers to them remain, so a later check
 * would touch freed memory (this crashed the game on quit, 2026-10-31). It
 * never overwrites a pointer it does not recognize. */
typedef struct {
  const char *name;
  uint64_t ufunc_slot, original;
  void *replacement;
  int swaps, warned;
} ufunc_swap;

static ufunc_swap ufunc_swaps[2];
static int ufunc_swap_count;
static volatile LONG ufunc_watch_stop; /* set at process detach */

static DWORD WINAPI ufunc_watch(LPVOID unused) {
  (void)unused;
  for (DWORD start = GetTickCount(); !ufunc_watch_stop;) {
    int pending = 0;
    for (int i = 0; i < ufunc_swap_count; i++) {
      ufunc_swap *w = &ufunc_swaps[i];
      uintptr_t ufunc = *(volatile uintptr_t *)ADDR(w->ufunc_slot);
      pending += !w->swaps;
      if (!ufunc)
        continue;
      void *original = ADDR(w->original);
      void *seen = InterlockedCompareExchangePointer((void *volatile *)(ufunc + UFUNC_NATIVE_OFFSET),
                                                     w->replacement, original);
      if (seen == original)
        log_msg("ufunc swap: %s native pointer replaced (UFunction %p, swap %d, %lu ms after start)", w->name,
                (void *)ufunc, ++w->swaps, (unsigned long)(GetTickCount() - start));
      else if (seen != w->replacement && !w->warned++)
        log_msg("ufunc swap: %s has unexpected native pointer %p (expected %p); left unchanged", w->name, seen,
                original);
    }
    if (!pending) {
      log_msg("ufunc swap: all %d swaps done; watcher stopped", ufunc_swap_count);
      break;
    }
    Sleep(5);
  }
  return 0;
}

static void start_ufunc_swaps(void) {
  if (cfg_offline_login || cfg_skip_title)
    ufunc_swaps[ufunc_swap_count++] = (ufunc_swap){"RequestLogin", ADDR_REQUEST_LOGIN_UFUNC,
                                                   ADDR_REQUEST_LOGIN_NATIVE, (void *)offline_request_login};
  if (cfg_skip_title)
    ufunc_swaps[ufunc_swap_count++] = (ufunc_swap){"IsEnableTitleMovie", ADDR_TITLE_MOVIE_UFUNC,
                                                   ADDR_SHARED_BOOL_NATIVE, (void *)title_movie_passthrough};
  if (!ufunc_swap_count)
    return;
  HANDLE h = CreateThread(NULL, 0, ufunc_watch, NULL, 0, NULL);
  if (h)
    CloseHandle(h);
  else
    log_msg("ufunc swap: CreateThread failed (%lu); login and title unchanged", (unsigned long)GetLastError());
}

static void setup(void) {
  find_ini();
  open_log();
  log_msg("sc6vrr: settings file %s (%s); environment variables take precedence", ini_path,
          GetFileAttributesA(ini_path) == INVALID_FILE_ATTRIBUTES ? "missing" : "found");
  cfg_uncap = env_int("SC6VRR_UNCAP", 0);
  cfg_gate = env_int("SC6VRR_GATE", 0);
  cfg_measure = env_int("SC6VRR_MEASURE", 0);
  cfg_frame_timers = env_int("SC6VRR_FRAME_TIMERS", 0);
  cfg_thread_names = env_int("SC6VRR_THREAD_NAMES", 0);
  cfg_max_fps = (float)env_int("SC6VRR_MAX_FPS", 120);
  log_msg("sc6vrr: uncap=%d gate=%d max_fps=%.0f measure=%d", cfg_uncap, cfg_gate, cfg_max_fps, cfg_measure);

  int exe_ok = exe_hash_matches();
  /* The sampler only reads, so it also runs on an unknown exe build (the
   * tests use that). It must start on this thread: the thread that loads the
   * DLL is the game thread. */
  cfg_sample = env_int("SC6VRR_SAMPLE", 0);
  if (cfg_sample) {
    cfg_sample_hz = env_int("SC6VRR_SAMPLE_HZ", 250);
    cfg_sample_hz = cfg_sample_hz < 10 ? 10 : cfg_sample_hz > 1000 ? 1000 : cfg_sample_hz;
    cfg_sample_all = env_int("SC6VRR_SAMPLE_ALL", 0);
    cfg_sample_dump_secs = env_int("SC6VRR_SAMPLE_DUMP_SECS", 10);
    if (cfg_sample_dump_secs < 1)
      cfg_sample_dump_secs = 1;
    start_sampler(exe_ok);
  }
  if (!exe_ok) {
    log_msg("sc6vrr: unknown exe build; no hooks installed");
    return;
  }
  cfg_offline_login = env_int("SC6VRR_OFFLINE_LOGIN", 0);
  cfg_skip_title = env_int("SC6VRR_SKIP_TITLE", 0);
  log_msg("sc6vrr: offline_login=%d skip_title=%d", cfg_offline_login, cfg_skip_title);
  start_ufunc_swaps();
  if (env_int("SC6VRR_FAST_IMPORTS", 0))
    install_fast_imports();
  if (env_int("SC6VRR_FAST_LOCKS", 0))
    install_fast_locks();
  if (env_int("SC6VRR_FAST_CRT", 0))
    install_fast_crt();
  if (env_int("SC6VRR_COUNT_IMPORTS", 0))
    install_import_counters();
  if (cfg_thread_names && !thread_name_observer)
    install_thread_name_observer();
  if (MH_Initialize() != MH_OK) {
    log_msg("sc6vrr: MH_Initialize failed");
    return;
  }
  if (env_int("SC6VRR_TRACE_TITLE", 0))
    install_title_trace();
  if (env_int("SC6VRR_LOG_SCENES", 0) || env_int("SC6VRR_TRACE_TITLE", 0) || env_int("SC6VRR_COUNT_IMPORTS", 0) ||
      cfg_skip_title == 2)
    install_scene_log();
  cfg_block_query_wait = env_int("SC6VRR_BLOCK_QUERY_WAIT", 0);
  if (cfg_block_query_wait)
    install_query_wait();
  /* Shader capture hooks only d3d11.dll exports, so it does not depend on the
   * engine prologue checks below and enables its own hooks. */
  char dir[MAX_PATH];
  DWORD dir_len = cfg_str("SC6VRR_SHADER_DIR", dir, sizeof(dir));
  cfg_skip_gs_draws = env_int("SC6VRR_SKIP_GS_DRAWS", 0);
  if (dir_len) {
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
  gpu_pass_setup();
  cfg_skip_prepass = env_int("SC6VRR_SKIP_PREPASS", 0);
  if (cfg_skip_prepass && !shader_dir[0]) {
    log_msg("prepass: needs SC6VRR_SHADER_DIR (the hooks that track bindings); skip off");
    cfg_skip_prepass = 0;
  }
  if (shader_dir[0])
    log_msg("live: SC6VRR_SKIP_PREPASS=%d; F6 toggles it, and edits to sc6vrr.ini apply while the game runs",
            cfg_skip_prepass);
  /* Frame-rate changes are switched off for now (2026-10-16). The engine hooks
   * below go in only with SC6VRR_FRAME_MODS=1, whatever the other settings say. */
  int frame_mods = env_int("SC6VRR_FRAME_MODS", 0);
  if (!bytes_match(ADDR(ADDR_GET_MAX_TICK_RATE), "40534883ec500f2974244048") ||
      !bytes_match(ADDR(ADDR_UPDATE_TIME), "40555357488d6c24b94881ec")) {
    log_msg("sc6vrr: engine function prologue mismatch; no hooks installed");
    cfg_skip_title = 0; /* the title tick needs the UpdateTime hook */
    return;
  }
  if (!frame_mods) {
    log_msg("sc6vrr: frame-rate modifications disabled; set SC6VRR_FRAME_MODS=1 to allow them");
    if (cfg_skip_title) {
      MH_CreateHook(ADDR(ADDR_UPDATE_TIME), hook_update_time_tick, (void **)&orig_update_time);
      log_msg("sc6vrr: UpdateTime hook for the title skip enabled (%d)", MH_EnableHook(ADDR(ADDR_UPDATE_TIME)));
    }
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
  } else if (reason == DLL_PROCESS_DETACH) {
    sample_stop = 1; /* the sampler writes its last file when it sees this */
    ufunc_watch_stop = 1;
    if (thread_name_observer) {
      RemoveVectoredExceptionHandler(thread_name_observer);
      thread_name_observer = NULL;
    }
  }
  return TRUE;
}
