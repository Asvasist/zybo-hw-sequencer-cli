/*
 * xgpiops.h - host mock of the XGpioPs driver API.
 *
 * Types and prototypes match the Vitis 2025.2 SDT BSP. The calls go to the
 * board model in mock_board.c instead of the PS GPIO registers.
 */
#ifndef XGPIOPS_H
#define XGPIOPS_H

#include "xil_types.h"
#include "xstatus.h"

typedef struct
{
    char   *Name;
    UINTPTR BaseAddr;
    u16     IntrId;
    UINTPTR IntrParent;
} XGpioPs_Config;

typedef struct
{
    XGpioPs_Config GpioConfig;
    u32            IsReady;
    u32            MaxPinNum;
} XGpioPs;

XGpioPs_Config *XGpioPs_LookupConfig(u32 BaseAddress);
s32  XGpioPs_CfgInitialize(XGpioPs *InstancePtr, const XGpioPs_Config *ConfigPtr, u32 EffectiveAddr);
void XGpioPs_WritePin(const XGpioPs *InstancePtr, u32 Pin, u32 Data);
u32  XGpioPs_ReadPin(const XGpioPs *InstancePtr, u32 Pin);
void XGpioPs_SetDirectionPin(const XGpioPs *InstancePtr, u32 Pin, u32 Direction);
void XGpioPs_SetOutputEnablePin(const XGpioPs *InstancePtr, u32 Pin, u32 OpEnable);

#endif /* XGPIOPS_H */
