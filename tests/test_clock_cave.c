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

static unsigned fixed_world_calls;
static void fake_fixed_world(void *world, int tick_type, float delta) {
  assert(world == (void *)0x12345678 || world == (void *)0x87654321);
  assert(tick_type == 2);
  assert(delta == SC6_LOGIC_DELTA);
  fixed_world_calls++;
}

static void test_fixed_world(void) {
  SetEnvironmentVariableA("SC6VRR_UNCAP", "1");
  SetEnvironmentVariableA("SC6VRR_GATE", "0");
  SetEnvironmentVariableA("SC6VRR_MAX_FPS", NULL);
  read_frame_config();
  assert(cfg_uncap && cfg_gate && cfg_max_fps == 0.0f);
  SetEnvironmentVariableA("SC6VRR_MAX_FPS", "120");
  read_frame_config();
  assert(cfg_gate && cfg_max_fps == 120.0f);
  SetEnvironmentVariableA("SC6VRR_MAX_FPS", "-1");
  read_frame_config();
  assert(cfg_max_fps == 0.0f);
  cfg_measure = 0;
  orig_world_tick = fake_fixed_world;
  sc6_fixed_clock clock = {0};
  /* Two worlds share each decision. A skipped frame must advance neither. */
  for (unsigned frame = 0; frame < 2400; frame++) {
    g_skip_battle = !sc6_fixed_step(&clock, frame * 250, 60000);
    hook_world_tick((void *)0x12345678, 2, 1.0f / 240);
    hook_world_tick((void *)0x87654321, 2, 1.0f / 240);
  }
  assert(fixed_world_calls == 1200);
  cfg_gate = 0;
  g_skip_battle = 1;
  orig_world_tick = fake_world;
  hook_world_tick((void *)0x12345678, 3, 0.125f);
  g_skip_battle = 0;
  SetEnvironmentVariableA("SC6VRR_UNCAP", NULL);
  SetEnvironmentVariableA("SC6VRR_GATE", NULL);
  SetEnvironmentVariableA("SC6VRR_MAX_FPS", NULL);
  puts("Uncap requires the world gate; two worlds advance at 60 Hz; disabled mode preserves arguments.");
}

typedef int (*test_value_fn)(void);
static test_value_fn original_value;
static int hooked_value(void) { return original_value() + 10; }

static void test_hook_installation(void) {
  assert(MH_Initialize() == MH_OK);
  uint8_t *code = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  assert(code);
  memset(code, 0x90, 4096);
  const uint8_t value[] = {0xb8, 7, 0, 0, 0, 0xc3}; /* mov eax,7; ret */
  memcpy(code + 32, value, sizeof(value));
  test_value_fn function = (test_value_fn)(code + 32);
  FlushInstructionCache(GetCurrentProcess(), code, 4096);
  engine_hook_site sites[] = {
    {code + 32, "b807000000c3", hooked_value, (void **)&original_value},
    {code + 32, "b807000000c3", hooked_value, (void **)&original_value},
  };
  /* A mismatch fails before any code changes. */
  sites[1].bytes = "cc";
  assert(!install_required_hooks(sites, 2));
  assert(function() == 7 && memcmp(code + 32, value, sizeof(value)) == 0);
  /* Creating the same hook twice fails after the first creation. It must undo it. */
  sites[1].bytes = sites[0].bytes;
  assert(!install_required_hooks(sites, 2));
  assert(function() == 7 && MH_RemoveHook(code + 32) == MH_ERROR_NOT_CREATED);
  assert(install_required_hooks(sites, 1));
  assert(function() == 17);
  assert(MH_DisableHook(code + 32) == MH_OK);
  assert(MH_RemoveHook(code + 32) == MH_OK);
  assert(function() == 7 && memcmp(code + 32, value, sizeof(value)) == 0);
  VirtualFree(code, 0, MEM_RELEASE);
  assert(MH_Uninitialize() == MH_OK);
  puts("Hook activation works; byte mismatches and partial creation failures preserve the original code.");
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
  test_hook_installation();
  test_fixed_world();
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
