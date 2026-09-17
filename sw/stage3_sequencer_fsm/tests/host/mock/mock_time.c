/*
 * mock_time.c - host model of the Cortex-A9 global timer. See mock_time.h.
 */
#include "mock_time.h"

#include <stddef.h>

#include "sleep.h"

#define MOCK_TICKS_PER_SECOND   ((u64)(COUNTS_PER_SECOND))

/* Exact both ways, so the model and the driver never disagree by a tick. */
static u64 us_to_ticks(u64 microseconds)
{
    return ((microseconds / 1000000ULL) * MOCK_TICKS_PER_SECOND)
           + (((microseconds % 1000000ULL) * MOCK_TICKS_PER_SECOND) / 1000000ULL);
}

static u64 ticks_to_us(u64 ticks)
{
    return ((ticks / MOCK_TICKS_PER_SECOND) * 1000000ULL)
           + (((ticks % MOCK_TICKS_PER_SECOND) * 1000000ULL) / MOCK_TICKS_PER_SECOND);
}

static u64  s_ticks;
static bool s_frozen;
static u64  s_drift_per_read_us;

void mock_time_reset(void)
{
    s_ticks             = 0U;
    s_frozen            = false;
    s_drift_per_read_us = 0U;
}

void mock_time_set_us(u64 microseconds)
{
    s_ticks = us_to_ticks(microseconds);
}

void mock_time_advance_us(u64 microseconds)
{
    s_ticks += us_to_ticks(microseconds);
}

void mock_time_advance_ms(u64 milliseconds)
{
    mock_time_advance_us(milliseconds * 1000U);
}

void mock_time_set_frozen(bool frozen)
{
    s_frozen = frozen;
}

void mock_time_set_drift_per_read_us(u64 microseconds)
{
    s_drift_per_read_us = microseconds;
}

u64 mock_time_us(void)
{
    return ticks_to_us(s_ticks);
}

/* ---- the BSP interface the driver uses ---- */

void XTime_GetTime(XTime *time_out)
{
    if (time_out == NULL)
    {
        return;
    }

    *time_out = s_ticks;

    if (!s_frozen)
    {
        s_ticks += us_to_ticks(s_drift_per_read_us);
    }
}

int usleep(unsigned long useconds)
{
    if (!s_frozen)
    {
        mock_time_advance_us((u64)useconds);
    }

    return 0;
}

int sleep(unsigned int seconds)
{
    return usleep((unsigned long)seconds * 1000000UL);
}
