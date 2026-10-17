#include <assert.h>
#include <stdio.h>
#include "../dll/src/fixed_step.h"

static void test_rate(unsigned fps) {
  sc6_fixed_clock clock = {0};
  const uint64_t frequency = 10000003; /* Does not divide evenly by 60. */
  unsigned steps = 0;
  for (unsigned frame = 0; frame < fps * 60; frame++) {
    steps += sc6_fixed_step(&clock, (uint64_t)frame * frequency / fps, frequency);
    /* The initial update starts the world. Later updates follow elapsed time. */
    uint64_t expected = 1 + ((uint64_t)frame * frequency / fps) * 60 / frequency;
    assert(steps == expected || steps == expected + 1);
  }
  assert(steps >= 3599 && steps <= 3600);
}

int main(void) {
  const unsigned rates[] = {60, 72, 90, 120, 144, 165, 240, 1000};
  for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
    test_rate(rates[i]);
  sc6_fixed_clock clock = {0};
  assert(!sc6_fixed_step(&clock, 0, 0));
  assert(sc6_fixed_step(&clock, 0, 60000));
  assert(!sc6_fixed_step(&clock, 0, 60000));
  assert(!sc6_fixed_step(&clock, 998, 60000));
  assert(sc6_fixed_step(&clock, 1000, 60000));
  /* A 100 ms stall releases one update and discards missed whole updates. */
  assert(sc6_fixed_step(&clock, 7000, 60000));
  assert(!sc6_fixed_step(&clock, 7500, 60000));
  assert(sc6_fixed_step(&clock, 8000, 60000));
  /* A long pause starts a fresh interval, so there is no recovery burst. */
  assert(sc6_fixed_step(&clock, 600000, 60000));
  assert(!sc6_fixed_step(&clock, 600500, 60000));
  assert(sc6_fixed_step(&clock, 601000, 60000));
  /* A changed timer origin resets the schedule. */
  assert(sc6_fixed_step(&clock, 100, 60000));
  assert(!sc6_fixed_step(&clock, 600, 60000));
  clock = (sc6_fixed_clock){0};
  for (unsigned frame = 0; frame < 450; frame++)
    assert(sc6_fixed_step(&clock, frame * 4000, 180000));
  puts("60 Hz scheduling passed at eight render rates, with stalls, pauses, and 45 fps.");
  return 0;
}
