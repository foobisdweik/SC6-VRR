/* Run this Windows executable under Wine. It starts the instruction-pointer
 * sampler on itself: one busy "game" thread and one busy "render" thread each
 * run a known function, and the samples file must name that function.
 * It neither loads the game nor calls setup().
 * The samples file path comes from SC6VRR_SAMPLE_FILE (a Windows path), because
 * Wine's wrapper does not pass command-line arguments through. */
#include <assert.h>
#include "../dll/src/sc6vrr.c"

static volatile double sink;
static volatile LONG running = 1;

static __attribute__((noinline)) void render_hot(void) {
  double x = 1.0;
  for (int i = 0; i < 20000; i++)
    x = x * 1.0000001 + 0.5;
  sink = x;
}

static __attribute__((noinline)) void game_hot(void) {
  double x = 2.0;
  for (int i = 0; i < 20000; i++)
    x = x * 0.9999999 + 0.25;
  sink = x;
}

static DWORD WINAPI render_main(LPVOID unused) {
  (void)unused;
  while (running)
    render_hot();
  return 0;
}

/* Finds the first line "<role>\t<kind>\t..." and returns its address and module. */
static int first_entry(const char *text, const char *role, const char *kind, char module[40], unsigned long long *addr) {
  char prefix[64];
  snprintf(prefix, sizeof(prefix), "\n%s\t%s\t", role, kind);
  const char *line = strstr(text, prefix);
  if (!line)
    return 0;
  line += strlen(prefix);
  unsigned count;
  return sscanf(line, "%39[^\t]\t%llx\t%u", module, addr, &count) == 3;
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  char path[MAX_PATH];
  assert(GetEnvironmentVariableA("SC6VRR_SAMPLE_FILE", path, sizeof(path)));
  cfg_sample = 1;
  cfg_sample_hz = 500;
  cfg_sample_dump_secs = 1;
  start_sampler(0);

  HANDLE render = CreateThread(NULL, 0, render_main, NULL, 0, NULL);
  assert(render);
  sample_note_thread(GetThreadId(render), "RenderThread 9");

  DWORD end = GetTickCount() + 3000;
  while ((LONG)(GetTickCount() - end) < 0)
    game_hot();
  running = 0;
  WaitForSingleObject(render, 5000);
  sample_stop = 1;
  Sleep(1500); /* the sampler writes its last file */

  FILE *f = fopen(path, "rb");
  assert(f);
  static char text[1 << 20];
  size_t n = fread(text, 1, sizeof(text) - 1, f);
  text[n] = 0;
  fclose(f);

  unsigned long long base = (unsigned long long)(uintptr_t)GetModuleHandleW(NULL);
  unsigned long long want_render = (unsigned long long)(uintptr_t)render_hot - base + 0x140000000ull;
  unsigned long long want_game = (unsigned long long)(uintptr_t)game_hot - base + 0x140000000ull;
  char module[40];
  unsigned long long addr;

  /* Under FEX (its emulator DLL xtajit64.dll is loaded in the process) the
   * thread context holds the last return target, not the instruction being
   * run, so the exact-function check holds only under Rosetta 2. */
  int exact = !GetModuleHandleW(L"xtajit64.dll");
  printf("instruction pointer is %s\n", exact ? "exact (Rosetta 2)" : "stale (FEX): checking the module only");

  assert(first_entry(text, "render", "rip", module, &addr));
  printf("render top rip: %s 0x%llx (render_hot at 0x%llx)\n", module, addr, want_render);
  assert(!strcmp(module, "exe") && (!exact || (addr >= want_render && addr < want_render + 0x100)));

  assert(first_entry(text, "game", "rip", module, &addr));
  printf("game top rip: %s 0x%llx (game_hot at 0x%llx)\n", module, addr, want_game);
  assert(!strcmp(module, "exe") && (!exact || (addr >= want_game && addr < want_game + 0x100)));

  /* The loops run inside the hot functions, so the stack holds the callers' return addresses. */
  /* Under FEX the saved context can already be in the thread's start function,
   * so no caller inside the exe may be on the stack. */
  if (exact) {
    assert(first_entry(text, "render", "caller1", module, &addr));
    printf("render top caller1: %s 0x%llx\n", module, addr);
    assert(!strcmp(module, "exe"));
  }

  unsigned long long samples = 0;
  const char *p = strstr(text, "# role_samples\trender\t");
  assert(p && sscanf(p, "# role_samples\trender\t%llu", &samples) == 1);
  printf("render samples: %llu (3 s at 500 Hz is about 1500)\n", samples);
  assert(samples > 300);
  printf("sampler test passed\n");
  return 0;
}
