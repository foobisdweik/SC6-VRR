#ifndef SC6_FIXED_STEP_H
#define SC6_FIXED_STEP_H

#include <stdint.h>

#define SC6_LOGIC_HZ 60
#define SC6_LOGIC_DELTA (1.0f / SC6_LOGIC_HZ)

/* phase counts timer units multiplied by 60. Integer arithmetic keeps the
 * schedule stable when the render rate does not divide evenly by 60. */
typedef struct {
  uint64_t last;
  int64_t phase;
  int started;
} sc6_fixed_clock;

/* Release at most one world update per engine frame. Discard missed whole
 * steps so a slow frame cannot cause a burst of accelerated battle updates.
 * Below 60 rendered frames per second, logic slows as it does in stock SC6. */
static inline int sc6_fixed_step(sc6_fixed_clock *clock, uint64_t now, uint64_t frequency) {
  if (!frequency)
    return 0;
  if (!clock->started || now < clock->last) {
    *clock = (sc6_fixed_clock){.last = now, .started = 1};
    return 1;
  }
  uint64_t elapsed = now - clock->last;
  clock->last = now;
  if (elapsed > frequency / 4) {
    clock->phase = 0;
    return 1;
  }
  clock->phase += (int64_t)(elapsed * SC6_LOGIC_HZ);
  /* Allow one timer count of rounding at an exact 60 Hz boundary. Retain
   * that small negative remainder so the tolerance cannot accumulate drift. */
  if (clock->phase < (int64_t)frequency - SC6_LOGIC_HZ)
    return 0;
  clock->phase -= (int64_t)frequency;
  if (clock->phase >= (int64_t)frequency)
    clock->phase %= (int64_t)frequency;
  return 1;
}

#endif
