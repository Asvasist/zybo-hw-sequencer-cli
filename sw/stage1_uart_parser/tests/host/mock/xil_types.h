/*
 * xil_types.h - host mock of the Xilinx BSP basic types.
 *
 * Part of the hardware mock that lets uart_driver.c build and run on a PC.
 * Only what uart_driver.c needs is provided.
 */
#ifndef XIL_TYPES_H
#define XIL_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef int32_t   s32;
typedef uintptr_t UINTPTR;

#endif /* XIL_TYPES_H */
