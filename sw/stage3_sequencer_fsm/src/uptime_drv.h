/*
 * uptime_drv.h
 *
 * Monotonic time since boot, from the Cortex-A9 global timer.
 *
 * Low-level layer: it knows which counter the Zynq has and how fast it ticks.
 * The application asks the HAL, never this file.
 *
 * The global timer is 64 bits wide and counts at half the CPU clock, so it
 * never wraps in any practical sense. The millisecond value is cut to 32 bits
 * for convenience, which does wrap - after 49.7 days. Everything that compares
 * millisecond stamps must therefore use the difference, never "<" or ">"; the
 * sequencer does exactly that and is tested across the wrap.
 *
 * This is a soft time base: reading it costs a couple of register reads and it
 * is accurate to a few microseconds, which is what a blink needs. The
 * command-to-hardware latency measurement in stage 4 uses the AXI Timer.
 */
#ifndef UPTIME_DRV_H
#define UPTIME_DRV_H

#include <stdint.h>

typedef enum
{
    UPTIME_DRV_OK = 0,
    UPTIME_DRV_ERR_NOT_COUNTING     /* the global timer never started */
} uptime_drv_status_t;

/*
 * Starts the time base and proves it is running. Under the SDT BSP the
 * xiltimer library only starts the global timer on the first sleep call, so a
 * firmware that never sleeps would read zero forever; this does that sleep
 * once, at boot, and then checks the counter actually advances.
 */
uptime_drv_status_t uptime_drv_init(void);

uint32_t uptime_drv_ms(void);
uint64_t uptime_drv_us(void);

#endif /* UPTIME_DRV_H */
