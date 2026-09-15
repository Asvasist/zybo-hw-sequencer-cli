/*
 * xparameters.h - host mock of the generated hardware parameters.
 *
 * Values copied from the Zybo Z7 SDT BSP (Vitis 2025.2): PS UART1 is the only
 * XUartPs instance, SPI 50 (GIC ID 82), level-sensitive, parent is the GIC.
 */
#ifndef XPARAMETERS_H
#define XPARAMETERS_H

#define XPAR_XUARTPS_0_BASEADDR             0xe0001000
#define XPAR_XUARTPS_0_INTERRUPTS           0x4032
#define XPAR_XUARTPS_0_INTERRUPT_PARENT     0xf8f01000

#endif /* XPARAMETERS_H */
