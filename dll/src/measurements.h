#ifndef SC6VRR_MEASUREMENTS_H
#define SC6VRR_MEASUREMENTS_H

#include <stdint.h>

typedef struct {
  uint64_t count;
  double total_ms, max_ms;
} sc6_duration;

static inline void sc6_duration_add(sc6_duration *duration, double milliseconds) {
  duration->count++;
  duration->total_ms += milliseconds;
  if (milliseconds > duration->max_ms)
    duration->max_ms = milliseconds;
}

/* The battle counter can restart between scenes. Accept unsigned wraparound,
 * but report jumps above 600 steps between engine frames as discontinuities.
 * This changes measurement only. It does not change the game's counter. */
static inline uint32_t sc6_counter_delta(uint32_t previous, uint32_t current,
                                         uint64_t *discontinuities) {
  uint32_t delta = current - previous;
  if (delta > 600) {
    (*discontinuities)++;
    return 0;
  }
  return delta;
}

#endif
