#include <assert.h>
#include <stdio.h>
#include "../dll/src/measurements.h"

int main(void) {
  sc6_duration duration = {0};
  sc6_duration_add(&duration, 2.0);
  sc6_duration_add(&duration, 7.0);
  sc6_duration_add(&duration, 0.0);
  assert(duration.count == 3);
  assert(duration.total_ms == 9.0);
  assert(duration.max_ms == 7.0);
  uint64_t resets = 0;
  assert(sc6_counter_delta(123, 125, &resets) == 2);
  assert(sc6_counter_delta(UINT32_MAX, 0, &resets) == 1);
  assert(sc6_counter_delta(120, 0, &resets) == 0);
  assert(resets == 1);
  assert(sc6_counter_delta(0, 601, &resets) == 0);
  assert(resets == 2);
  assert(sc6_counter_delta(0, 1, &resets) == 1);
  puts("Duration totals, counter wraparound, and counter discontinuities passed.");
  return 0;
}
