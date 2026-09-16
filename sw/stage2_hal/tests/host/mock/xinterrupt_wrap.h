/*
 * xinterrupt_wrap.h - host mock of the SDT interrupt wrapper.
 *
 * XSetupInterruptSystem() doesn't touch a GIC here; mock_uart.c records the
 * handler so a test can raise the UART interrupt by calling it.
 */
#ifndef XINTERRUPT_WRAP_H
#define XINTERRUPT_WRAP_H

#include "xil_types.h"
#include "xstatus.h"

#define XINTERRUPT_DEFAULT_PRIORITY     0xA0U

int XSetupInterruptSystem(void *DriverInstance, void *IntrHandler, u32 IntrId, UINTPTR IntcParent, u16 Priority);

#endif /* XINTERRUPT_WRAP_H */
