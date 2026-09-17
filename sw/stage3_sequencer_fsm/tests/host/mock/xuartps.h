/*
 * xuartps.h - host mock of the XUartPs driver API and register map.
 *
 * Types, register offsets and bit masks match xuartps.h / xuartps_hw.h from
 * the Vitis 2025.2 SDT BSP. Register access goes to the register model in
 * mock_uart.c instead of memory-mapped I/O.
 */
#ifndef XUARTPS_H
#define XUARTPS_H

#include "xil_types.h"
#include "xstatus.h"

/* ---- register offsets (xuartps_hw.h) ---- */
#define XUARTPS_CR_OFFSET           0x0000U
#define XUARTPS_MR_OFFSET           0x0004U
#define XUARTPS_IER_OFFSET          0x0008U
#define XUARTPS_IDR_OFFSET          0x000CU
#define XUARTPS_IMR_OFFSET          0x0010U
#define XUARTPS_ISR_OFFSET          0x0014U
#define XUARTPS_RXTOUT_OFFSET       0x001CU
#define XUARTPS_RXWM_OFFSET         0x0020U
#define XUARTPS_SR_OFFSET           0x002CU
#define XUARTPS_FIFO_OFFSET         0x0030U

/* ---- interrupt bits: IER / IDR / IMR / ISR ---- */
#define XUARTPS_IXR_RBRK            0x00002000U
#define XUARTPS_IXR_TOVR            0x00001000U
#define XUARTPS_IXR_TNFUL           0x00000800U
#define XUARTPS_IXR_TTRIG           0x00000400U
#define XUARTPS_IXR_DMS             0x00000200U
#define XUARTPS_IXR_TOUT            0x00000100U
#define XUARTPS_IXR_PARITY          0x00000080U
#define XUARTPS_IXR_FRAMING         0x00000040U
#define XUARTPS_IXR_OVER            0x00000020U
#define XUARTPS_IXR_TXFULL          0x00000010U
#define XUARTPS_IXR_TXEMPTY         0x00000008U
#define XUARTPS_IXR_RXFULL          0x00000004U
#define XUARTPS_IXR_RXEMPTY         0x00000002U
#define XUARTPS_IXR_RXOVR           0x00000001U
#define XUARTPS_IXR_MASK            0x00003FFFU

/* ---- channel status register bits ---- */
#define XUARTPS_SR_TACTIVE          0x00000800U
#define XUARTPS_SR_RACTIVE          0x00000400U
#define XUARTPS_SR_TXFULL           0x00000010U
#define XUARTPS_SR_TXEMPTY          0x00000008U
#define XUARTPS_SR_RXFULL           0x00000004U
#define XUARTPS_SR_RXEMPTY          0x00000002U
#define XUARTPS_SR_RXOVR            0x00000001U

/* ---- driver constants (xuartps.h) ---- */
#define XUARTPS_OPER_MODE_NORMAL    (u8)0x00U

#define XUARTPS_FORMAT_8_BITS       0U
#define XUARTPS_FORMAT_NO_PARITY    4U
#define XUARTPS_FORMAT_1_STOP_BIT   0U

/* ---- types, SDT variant ---- */
typedef struct
{
    char   *Name;
    u32     BaseAddress;
    u32     InputClockHz;
    s32     ModemPinsConnected;
    u32     RefClk;
    u32     IntrId;
    UINTPTR IntrParent;
} XUartPs_Config;

typedef struct
{
    u32 BaudRate;
    u32 DataBits;
    u32 Parity;
    u8  StopBits;
} XUartPsFormat;

typedef struct
{
    XUartPs_Config Config;
    u32            IsReady;
    u32            BaudRate;
} XUartPs;

/* ---- register access, routed to the model ---- */
u32  mock_uart_read_reg(u32 base_address, u32 offset);
void mock_uart_write_reg(u32 base_address, u32 offset, u32 value);

#define XUartPs_ReadReg(BaseAddress, RegOffset) \
    mock_uart_read_reg((u32)(BaseAddress), (u32)(RegOffset))
#define XUartPs_WriteReg(BaseAddress, RegOffset, RegisterValue) \
    mock_uart_write_reg((u32)(BaseAddress), (u32)(RegOffset), (u32)(RegisterValue))

/* ---- driver functions used by uart_driver.c ---- */
XUartPs_Config *XUartPs_LookupConfig(u32 BaseAddress);
s32  XUartPs_CfgInitialize(XUartPs *InstancePtr, XUartPs_Config *Config, u32 EffectiveAddr);
s32  XUartPs_SetDataFormat(XUartPs *InstancePtr, XUartPsFormat *FormatPtr);
void XUartPs_SetOperMode(XUartPs *InstancePtr, u8 OperationMode);
void XUartPs_SetFifoThreshold(XUartPs *InstancePtr, u8 TriggerLevel);
void XUartPs_SetRecvTimeout(XUartPs *InstancePtr, u8 RecvTimeout);
void XUartPs_SetInterruptMask(XUartPs *InstancePtr, u32 Mask);
void XUartPs_SendByte(u32 BaseAddress, u8 Data);

#endif /* XUARTPS_H */
