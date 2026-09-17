/*
 * board_zybo.h
 *
 * Board resource map for the Digilent Zybo / Zybo Z7-10 / Zybo Z7-20.
 *
 * The low-level drivers take their instance addresses and pin numbers from
 * here rather than from xparameters.h directly, so moving to another board or
 * another BSP flow touches this one header. Nothing above the driver layer
 * includes it: the HAL hides the board from the application entirely.
 *
 * Only resources the PS can reach without any PL logic are listed. The four
 * PL LEDs LD0..LD3 sit on PL pins and need an AXI GPIO, so they arrive with
 * the hardware design in a later stage.
 *
 * The console baud rate is not here: it is an application setting, and the
 * application cannot include this header without pulling xparameters.h in
 * with it. main.c owns it.
 *
 * SDT BSP flow (Vitis 2023.2 and later): LookupConfig() takes a base address.
 * The Zybo preset enables only UART1, so it is the first XUartPs instance.
 * There is one XADC; the SDT lookup uses base address 0, which XAdcPs reads
 * as "first instance" - what the Xilinx SDT examples do.
 */
#ifndef BOARD_ZYBO_H
#define BOARD_ZYBO_H

#include "xparameters.h"

#define BOARD_CONSOLE_UART_BASEADDR     XPAR_XUARTPS_0_BASEADDR

#define BOARD_PS_GPIO_BASEADDR          XPAR_XGPIOPS_0_BASEADDR
#define BOARD_XADC_ID                   0U

/*
 * LD4 is on MIO7, active high. MIO7 is one of the two output-only MIO pins,
 * so it needs both a direction and an output enable to drive anything.
 * The pin mux itself comes from ps7_init (the board preset).
 */
#define BOARD_MIO_LED4                  7U

#endif /* BOARD_ZYBO_H */
