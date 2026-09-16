/*
 * mock_board.c - host model of the PS MIO GPIO pins and the XADC.
 * See mock_board.h.
 */
#include "mock_board.h"

#include <string.h>

#include "xparameters.h"

#define MOCK_XADC_BASEADDR          0xf8007100U     /* devcfg/XADCIF on the Zynq */
#define MOCK_COMPONENT_IS_READY     0x11111111U
#define MOCK_XADC_RESULT_SHIFT      4U

mock_board_state_t mock_board;

static XGpioPs_Config        s_gpio_config;
static XAdcPs_Config         s_xadc_config;
static u16                   s_temp_raw;        /* MSB-justified, as the register holds it */
static mock_board_pin_hook_t s_pin_hook;

static bool pin_valid(u32 pin)
{
    if (pin >= MOCK_MIO_PIN_COUNT)
    {
        mock_board.gpio_bad_pin_accesses++;
        return false;
    }

    return true;
}

void mock_board_reset(void)
{
    memset(&mock_board, 0, sizeof(mock_board));

    s_gpio_config.Name       = "gpio@e000a000";
    s_gpio_config.BaseAddr   = XPAR_XGPIOPS_0_BASEADDR;
    s_gpio_config.IntrId     = 0U;
    s_gpio_config.IntrParent = 0U;

    s_xadc_config.DeviceId    = 0U;
    s_xadc_config.BaseAddress = MOCK_XADC_BASEADDR;

    /*
     * A warm restart finds the sequencer wherever the previous run left it,
     * which is running - not in safe mode. The driver has to park it itself.
     */
    mock_board.seq_mode                  = XADCPS_SEQ_MODE_CONTINPASS;
    mock_board.seq_mode_at_channel_setup = MOCK_SEQ_MODE_UNSET;
    mock_board.seq_mode_at_average_setup = MOCK_SEQ_MODE_UNSET;

    s_temp_raw = 0U;
    s_pin_hook = NULL;
}

void mock_board_set_temp_code(u16 code)
{
    s_temp_raw = (u16)(code << MOCK_XADC_RESULT_SHIFT);
}

void mock_board_set_pin_hook(mock_board_pin_hook_t hook)
{
    s_pin_hook = hook;
}

/* ---- XGpioPs ---- */

XGpioPs_Config *XGpioPs_LookupConfig(u32 BaseAddress)
{
    if (mock_board.gpio_fail_lookup || (BaseAddress != s_gpio_config.BaseAddr))
    {
        return NULL;
    }

    return &s_gpio_config;
}

s32 XGpioPs_CfgInitialize(XGpioPs *InstancePtr, const XGpioPs_Config *ConfigPtr, u32 EffectiveAddr)
{
    if (mock_board.gpio_fail_init)
    {
        return XST_FAILURE;
    }

    InstancePtr->GpioConfig          = *ConfigPtr;
    InstancePtr->GpioConfig.BaseAddr = EffectiveAddr;
    InstancePtr->IsReady             = MOCK_COMPONENT_IS_READY;
    InstancePtr->MaxPinNum           = MOCK_MIO_PIN_COUNT;

    mock_board.gpio_initialised = true;
    return XST_SUCCESS;
}

void XGpioPs_WritePin(const XGpioPs *InstancePtr, u32 Pin, u32 Data)
{
    (void)InstancePtr;

    if (!mock_board.gpio_initialised)
    {
        mock_board.gpio_use_before_init++;
        return;
    }
    if (!pin_valid(Pin))
    {
        return;
    }

    if (mock_board.pin[Pin].writes == 0U)
    {
        mock_board.pin[Pin].first_write_had_oe = (mock_board.pin[Pin].output_enable != 0U);
    }
    mock_board.pin[Pin].writes++;
    mock_board.pin[Pin].level = (Data != 0U) ? 1U : 0U;

    if (s_pin_hook != NULL)
    {
        s_pin_hook(Pin, mock_board.pin[Pin].level);
    }
}

u32 XGpioPs_ReadPin(const XGpioPs *InstancePtr, u32 Pin)
{
    (void)InstancePtr;

    if (!mock_board.gpio_initialised)
    {
        mock_board.gpio_use_before_init++;
        return 0U;
    }
    if (!pin_valid(Pin))
    {
        return 0U;
    }

    /*
     * An output pin reads back what it drives, an input pin reads 0 in this
     * model. Good enough: the stage 2 driver only reads back its own outputs.
     */
    return mock_board.pin[Pin].level;
}

void XGpioPs_SetDirectionPin(const XGpioPs *InstancePtr, u32 Pin, u32 Direction)
{
    (void)InstancePtr;

    if (!mock_board.gpio_initialised)
    {
        mock_board.gpio_use_before_init++;
        return;
    }
    if (pin_valid(Pin))
    {
        mock_board.pin[Pin].direction = (Direction != 0U) ? 1U : 0U;
    }
}

void XGpioPs_SetOutputEnablePin(const XGpioPs *InstancePtr, u32 Pin, u32 OpEnable)
{
    (void)InstancePtr;

    if (!mock_board.gpio_initialised)
    {
        mock_board.gpio_use_before_init++;
        return;
    }
    if (pin_valid(Pin))
    {
        mock_board.pin[Pin].output_enable = (OpEnable != 0U) ? 1U : 0U;
    }
}

/* ---- XAdcPs ---- */

XAdcPs_Config *XAdcPs_LookupConfig(u32 BaseAddress)
{
    /*
     * The SDT driver treats base address 0 as "the first instance", which is
     * how board_zybo.h asks for the one XADC on the chip.
     */
    if (mock_board.xadc_fail_lookup || ((BaseAddress != 0U) && (BaseAddress != s_xadc_config.BaseAddress)))
    {
        return NULL;
    }

    return &s_xadc_config;
}

int XAdcPs_CfgInitialize(XAdcPs *InstancePtr, const XAdcPs_Config *ConfigPtr, u32 EffectiveAddr)
{
    if (mock_board.xadc_fail_init)
    {
        return XST_FAILURE;
    }

    InstancePtr->Config             = *ConfigPtr;
    InstancePtr->Config.BaseAddress = EffectiveAddr;
    InstancePtr->IsReady            = MOCK_COMPONENT_IS_READY;

    mock_board.xadc_initialised = true;
    return XST_SUCCESS;
}

int XAdcPs_SelfTest(XAdcPs *InstancePtr)
{
    (void)InstancePtr;

    if (!mock_board.xadc_initialised)
    {
        mock_board.xadc_use_before_init++;
    }

    mock_board.selftest_calls++;
    return mock_board.xadc_fail_selftest ? XST_FAILURE : XST_SUCCESS;
}

void XAdcPs_SetSequencerMode(XAdcPs *InstancePtr, u8 SequencerMode)
{
    (void)InstancePtr;

    if (!mock_board.xadc_initialised)
    {
        mock_board.xadc_use_before_init++;
        return;
    }

    mock_board.seq_mode = SequencerMode;
}

void XAdcPs_SetCalibEnables(XAdcPs *InstancePtr, u16 Calibration)
{
    (void)InstancePtr;
    mock_board.calibration_mask = Calibration;
}

void XAdcPs_SetAvg(XAdcPs *InstancePtr, u8 Average)
{
    (void)InstancePtr;
    mock_board.average = Average;
}

int XAdcPs_SetSeqAvgEnables(XAdcPs *InstancePtr, u32 AvgEnableChMask)
{
    (void)InstancePtr;

    /* The real driver refuses this unless the sequencer sits in safe mode. */
    mock_board.seq_mode_at_average_setup = mock_board.seq_mode;
    if (mock_board.seq_mode != XADCPS_SEQ_MODE_SAFE)
    {
        return XST_FAILURE;
    }

    mock_board.seq_average_channels = AvgEnableChMask;

    return mock_board.xadc_fail_seq_average ? XST_FAILURE : XST_SUCCESS;
}

int XAdcPs_SetSeqChEnables(XAdcPs *InstancePtr, u32 ChEnableMask)
{
    (void)InstancePtr;

    mock_board.seq_mode_at_channel_setup = mock_board.seq_mode;
    if (mock_board.seq_mode != XADCPS_SEQ_MODE_SAFE)
    {
        return XST_FAILURE;
    }

    mock_board.seq_channels = ChEnableMask;

    return mock_board.xadc_fail_seq_channels ? XST_FAILURE : XST_SUCCESS;
}

u16 XAdcPs_GetAdcData(XAdcPs *InstancePtr, u8 Channel)
{
    (void)InstancePtr;

    if (!mock_board.xadc_initialised)
    {
        mock_board.xadc_use_before_init++;
        return 0U;
    }

    if (Channel != XADCPS_CH_TEMP)
    {
        mock_board.xadc_bad_channel_reads++;
        return 0U;
    }

    return s_temp_raw;
}
