/*
 * uptime_drv.c
 *
 * Cortex-A9 global timer through the BSP's XTime interface.
 * Reference: UG585 chapter 4.2.2 (global timer).
 */
#include "uptime_drv.h"

#include "xiltimer.h"
#include "sleep.h"

#ifndef SDT
#error "uptime_drv.c targets the SDT BSP flow (Vitis 2023.2 or later); SDT is not defined"
#endif

/*
 * COUNTS_PER_SECOND is generated as a bare division ("FREQ/2"), so it has to
 * be parenthesised before being divided again, or the result is nonsense.
 */
#define UPTIME_TICKS_PER_SECOND     ((uint64_t)(COUNTS_PER_SECOND))

/* Long enough that the check is unambiguous, short enough to not delay boot. */
#define UPTIME_SELFTEST_US          100U

/*
 * Whole seconds first, the remainder second. That is exact - no divisor is
 * rounded off, so a millisecond stamp and a microsecond stamp taken from the
 * same tick always agree - and neither product can overflow 64 bits within
 * any uptime this board will ever see.
 */
static uint64_t uptime_scale(uint64_t ticks, uint64_t units_per_second)
{
    return ((ticks / UPTIME_TICKS_PER_SECOND) * units_per_second)
           + (((ticks % UPTIME_TICKS_PER_SECOND) * units_per_second) / UPTIME_TICKS_PER_SECOND);
}

uptime_drv_status_t uptime_drv_init(void)
{
    XTime before;
    XTime after;

    /*
     * The first sleep is what starts (and zeroes) the global timer under the
     * SDT xiltimer library. It is a plain busy-wait, safe this early.
     */
    usleep(1U);

    XTime_GetTime(&before);
    usleep(UPTIME_SELFTEST_US);
    XTime_GetTime(&after);

    return (after > before) ? UPTIME_DRV_OK : UPTIME_DRV_ERR_NOT_COUNTING;
}

uint32_t uptime_drv_ms(void)
{
    XTime now;

    XTime_GetTime(&now);

    return (uint32_t)uptime_scale((uint64_t)now, 1000ULL);
}

uint64_t uptime_drv_us(void)
{
    XTime now;

    XTime_GetTime(&now);

    return uptime_scale((uint64_t)now, 1000000ULL);
}
