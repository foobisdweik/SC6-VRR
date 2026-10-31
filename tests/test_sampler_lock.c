/* Run this Windows executable under Wine. It checks that the sampler cannot
 * deadlock with a thread that it pauses. The game thread allocates and frees
 * virtual memory without pause, so it often holds Wine's virtual-memory lock
 * when the sampler pauses it. If the sampler took that lock while the thread
 * was paused, both would stop, and the watchdog below would report it. */
#include <assert.h>
#include "../dll/src/sc6vrr.c"

static volatile LONG running = 1;
static volatile LONG alloc_loops;

static DWORD WINAPI watchdog_main(LPVOID unused) {
  (void)unused;
  LONG last_loops = -1, last_passes = -1;
  for (int tick = 0; tick < 12 && running; tick++) {
    Sleep(500);
    LONG loops = alloc_loops, passes = (LONG)sample_passes;
    if (tick > 2 && loops == last_loops && passes == last_passes) {
      printf("DEADLOCK: no progress for 0.5 s (alloc loops %ld, sample passes %ld)\n", (long)loops, (long)passes);
      fflush(stdout);
      ExitProcess(2);
    }
    last_loops = loops;
    last_passes = passes;
  }
  return 0;
}

int main(void) {
  cfg_sample = 1;
  cfg_sample_hz = 1000;
  cfg_sample_dump_secs = 60;
  char path[MAX_PATH];
  assert(GetEnvironmentVariableA("SC6VRR_SAMPLE_FILE", path, sizeof(path)));
  start_sampler(0);
  HANDLE dog = CreateThread(NULL, 0, watchdog_main, NULL, 0, NULL);
  DWORD end = GetTickCount() + 5000;
  while ((LONG)(GetTickCount() - end) < 0) {
    void *p = VirtualAlloc(NULL, 1 << 16, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(p);
    ((volatile char *)p)[0] = 1;
    VirtualFree(p, 0, MEM_RELEASE);
    InterlockedIncrement(&alloc_loops);
  }
  running = 0;
  sample_stop = 1;
  WaitForSingleObject(dog, 2000);
  printf("no deadlock: %ld alloc loops, %ld sample passes\n", (long)alloc_loops, (long)sample_passes);
  return 0;
}
