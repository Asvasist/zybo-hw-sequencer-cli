/*
 * xiltimer.h - host mock of the BSP time interface.
 *
 * The real one gives XTime_GetTime() and COUNTS_PER_SECOND from the Cortex-A9
 * global timer. Here they come from the clock model in mock_time.c, which the
 * tests wind forward by hand.
 */
#ifndef XILTIMER_H
#define XILTIMER_H

#include "xil_types.h"

typedef u64 XTime;

/*
 * The real BSP generates this as a bare division, which is exactly why the
 * driver parenthesises it before dividing again. Kept that way here so the
 * host build exercises the same expression.
 */
#define COUNTS_PER_SECOND 666666687 / 2

void XTime_GetTime(XTime *time_out);

#endif /* XILTIMER_H */
