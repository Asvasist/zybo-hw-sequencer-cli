/*
 * xadcps.h - host mock of the XAdcPs driver API.
 *
 * Types, constants and prototypes match the Vitis 2025.2 SDT BSP. The calls
 * go to the board model in mock_board.c instead of the XADC interface.
 */
#ifndef XADCPS_H
#define XADCPS_H

#include "xil_types.h"
#include "xstatus.h"

/* Channel numbers (xadcps.h) */
#define XADCPS_CH_TEMP                          0x0U

/* Averaging (xadcps.h) */
#define XADCPS_AVG_16_SAMPLES                   1U

/* Sequencer modes (xadcps.h) */
#define XADCPS_SEQ_MODE_SAFE                    0U
#define XADCPS_SEQ_MODE_CONTINPASS              2U

/* Sequencer channel bits (xadcps_hw.h) */
#define XADCPS_SEQ_CH_CALIB                     0x00000001U
#define XADCPS_SEQ_CH_TEMP                      0x00000100U

/* Calibration enables (xadcps_hw.h) */
#define XADCPS_CFR1_CAL_ADC_GAIN_OFFSET_MASK    0x00000020U
#define XADCPS_CFR1_CAL_PS_GAIN_OFFSET_MASK     0x00000080U

typedef struct
{
    u16 DeviceId;
    u32 BaseAddress;
} XAdcPs_Config;

typedef struct
{
    XAdcPs_Config Config;
    u32           IsReady;
} XAdcPs;

XAdcPs_Config *XAdcPs_LookupConfig(u32 BaseAddress);
int  XAdcPs_CfgInitialize(XAdcPs *InstancePtr, const XAdcPs_Config *ConfigPtr, u32 EffectiveAddr);
int  XAdcPs_SelfTest(XAdcPs *InstancePtr);
void XAdcPs_SetSequencerMode(XAdcPs *InstancePtr, u8 SequencerMode);
void XAdcPs_SetCalibEnables(XAdcPs *InstancePtr, u16 Calibration);
void XAdcPs_SetAvg(XAdcPs *InstancePtr, u8 Average);
int  XAdcPs_SetSeqAvgEnables(XAdcPs *InstancePtr, u32 AvgEnableChMask);
int  XAdcPs_SetSeqChEnables(XAdcPs *InstancePtr, u32 ChEnableMask);
u16  XAdcPs_GetAdcData(XAdcPs *InstancePtr, u8 Channel);

#endif /* XADCPS_H */
