/* Run this Windows executable under Wine. It exercises the generated x64 code.
 * It neither loads the game nor calls setup(). */
#include <assert.h>
#include "../dll/src/sc6vrr.c"

static uint64_t call_clock(void *cave, void *object) {
  uint64_t flags, result;
  __asm__ volatile(
      "mov %[object], %%rbx\n\t"
      "xor %%eax, %%eax\n\t"
      "mov $0x12345678, %%rax\n\t"
      "stc\n\t"
      "call *%[cave]\n\t"
      "pushfq\n\tpop %[flags]"
      : [flags] "=r"(flags), "=&a"(result)
      : [object] "r"(object), [cave] "r"(cave)
      : "rbx", "rcx", "rdx", "r8", "r9", "r10", "r11", "cc", "memory");
  assert(result == 0x12345678);
  return flags;
}

static void fake_world(void *world, int tick_type, float delta) {
  assert(world == (void *)0x12345678);
  assert(tick_type == 3);
  assert(delta == 0.125f);
}

static unsigned fake_fence_depth;
static void fake_fence(void *self, uint8_t option) {
  assert(self == (void *)0x12345678 && option == 0xA5);
  if (!fake_fence_depth++)
    hook_fence_wait(self, option);
  fake_fence_depth--;
}

static void fake_sync(void *self, uint8_t option) {
  assert(self == (void *)0x12345678 && option == 1);
  hook_fence_wait(self, 0xA5);
}

static void fake_game(void *self, float delta, uint8_t idle) {
  assert(self == (void *)0x12345678 && delta == 0.125f && idle == 0xA5);
  hook_world_tick(self, 3, delta);
  hook_world_tick(self, 3, delta);
}

static void fake_loop(void *self) {
  assert(self == (void *)0x12345678);
  hook_game_tick(self, 0.125f, 0xA5);
  hook_frame_sync(self, 1);
}

static unsigned consumed_thread_names;
static LONG CALLBACK consume_test_thread_name(EXCEPTION_POINTERS *exception) {
  if (exception->ExceptionRecord->ExceptionCode != 0x406D1388)
    return EXCEPTION_CONTINUE_SEARCH;
  consumed_thread_names++;
  return EXCEPTION_CONTINUE_EXECUTION;
}

static void test_thread_names(void) {
  typedef HRESULT (WINAPI *get_description_fn)(HANDLE, PWSTR *);
  get_description_fn get_description = (get_description_fn)GetProcAddress(
      GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
  assert(get_description);
  PWSTR previous = NULL;
  assert(SUCCEEDED(get_description(GetCurrentThread(), &previous)));
  PVOID consumer = AddVectoredExceptionHandler(0, consume_test_thread_name);
  assert(consumer);
  install_thread_name_observer();
  assert(thread_name_observer && set_thread_description);
  ULONG_PTR info[3] = {0x1000, (ULONG_PTR)"SC6-name-test", (ULONG_PTR)(DWORD)-1};
  RaiseException(0x406D1388, 0, 3, info);
  assert(consumed_thread_names == 1); /* The observer did not consume the exception. */
  PWSTR name = NULL;
  assert(SUCCEEDED(get_description(GetCurrentThread(), &name)));
  assert(wcscmp(name, L"SC6-name-test") == 0);
  LocalFree(name);
  info[0] = 0x999;
  RaiseException(0x406D1388, 0, 3, info);
  assert(consumed_thread_names == 2);
  assert(SUCCEEDED(get_description(GetCurrentThread(), &name)));
  assert(wcscmp(name, L"SC6-name-test") == 0);
  LocalFree(name);
  RemoveVectoredExceptionHandler(thread_name_observer);
  thread_name_observer = NULL;
  RemoveVectoredExceptionHandler(consumer);
  set_thread_description(GetCurrentThread(), previous);
  LocalFree(previous);
  puts("Legacy thread names propagate; invalid records are ignored; original exception handling continues.");
}

int main(void) {
  test_thread_names();
  uint8_t object[0x3A8] = {0};
  uint32_t *clock = (uint32_t *)(object + 0x3A4);
  uint8_t *cave = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  assert(cave);
  for (unsigned enabled = 0; enabled < 2; enabled++) {
    cfg_measure = enabled;
    for (unsigned site = 0; site < 2; site++) {
      make_clock_cave(cave, site);
      FlushInstructionCache(GetCurrentProcess(), cave, 4096);
      g_clock_advances[0] = g_clock_advances[1] = 0;
      *clock = 0x7FFFFFFF;
      g_skip_battle = 0;
      uint64_t flags = call_clock(cave, object);
      assert(*clock == 0x80000000);
      assert((flags & 0x8D5) == 0x895); /* INC preserves the incoming carry flag. */
      assert(g_clock_advances[site] == enabled);
      assert(g_clock_advances[1 - site] == 0);
      g_skip_battle = 1;
      flags = call_clock(cave, object);
      assert((flags & 0x8D5) == 0x45); /* A skip preserves every incoming status flag. */
      assert(*clock == 0x80000000);
      assert(g_clock_advances[site] == enabled);
    }
  }
  VirtualFree(cave, 0, MEM_RELEASE);
  cfg_measure = 1;
  measurements.engine_thread = GetCurrentThreadId();
  orig_world_tick = fake_world;
  hook_world_tick((void *)0x12345678, 3, 0.125f);
  assert(measurements.world.count == 1);
  cfg_measure = 0;
  hook_world_tick((void *)0x12345678, 3, 0.125f);
  assert(measurements.world.count == 1);
  cfg_measure = cfg_frame_timers = 1;
  orig_frame_loop = fake_loop;
  orig_game_tick = fake_game;
  orig_frame_sync = fake_sync;
  orig_fence_wait = fake_fence;
  hook_frame_loop((void *)0x12345678);
  assert(frame_timers.loop.count == 1);
  assert(frame_timers.game.count == 1);
  assert(frame_timers.world.count == 2);
  assert(frame_timers.sync.count == 1);
  assert(frame_timers.fence.count == 1); /* Nested waits are not counted twice. */
  assert(frame_timers.sync_in_game == 0);
  assert(frame_timers.frame_depth == 0 && frame_timers.game_depth == 0);
  cfg_frame_timers = 0;
  hook_frame_loop((void *)0x12345678);
  assert(frame_timers.loop.count == 1 && frame_timers.game.count == 1);
  assert(frame_timers.fence.count == 1);
  puts("Frame timer arguments, nested waits, scope boundaries, and disabled mode passed.");
  puts("Clock counts, skipped increments, flags, registers, and world arguments passed.");
  return 0;
}
