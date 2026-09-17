/*
 * mock_time.h - test-side controls of the host clock model.
 *
 * mock_time.c stands in for the Cortex-A9 global timer, so uptime_drv.c
 * compiles and runs unchanged on a PC. Time only moves when a test moves it
 * (or when the code under test sleeps), which makes a ten-minute sequence a
 * matter of microseconds and the 49.7-day wrap of the millisecond stamp an
 * ordinary test case.
 */
#ifndef MOCK_TIME_H
#define MOCK_TIME_H

#include <stdbool.h>

#include "xiltimer.h"

/* Back to zero, counting, no auto-advance. */
void mock_time_reset(void);

void mock_time_set_us(u64 microseconds);
void mock_time_advance_us(u64 microseconds);
void mock_time_advance_ms(u64 milliseconds);

/* A frozen timer: the counter never moves, as when the global timer never started. */
void mock_time_set_frozen(bool frozen);

/* Microseconds added on every read. Lets a free-running loop see time pass. */
void mock_time_set_drift_per_read_us(u64 microseconds);

u64 mock_time_us(void);

#endif /* MOCK_TIME_H */
