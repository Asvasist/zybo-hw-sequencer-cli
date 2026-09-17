/*
 * xadc_drv.c
 *
 * XADC driver on top of the Xilinx XAdcPs low-level driver. XAdcPs owns the
 * XADCIF command/read FIFO protocol; this layer owns the sequencer setup and
 * the unit conversion.
 *
 * Reference: UG480 (7 Series XADC) chapter 2 for the transfer function and
 * chapter 4 for the sequencer, UG585 chapter 30 for the PS-XADC interface.
 */
#include "xadc_drv.h"

#include <stddef.h>

#include "xadcps.h"
#include "xstatus.h"

#include "board_zybo.h"

#ifndef SDT
#error "xadc_drv.c targets the SDT BSP flow (Vitis 2023.2 or later); SDT is not defined"
#endif

/*
 * The result registers hold the 12-bit conversion MSB-justified in 16 bits.
 * The bottom 4 bits are extra resolution from averaging that the datasheet
 * accuracy figures do not cover, so they are dropped.
 */
#define XADC_RESULT_SHIFT           4U
#define XADC_CODE_FULL_SCALE        4096UL

/*
 * UG480: T[degC] = code * 503.975 / 4096 - 273.15, scaled to milli-degrees so
 * it stays in integers. Worst case 4095 * 503975 = 2.06e9, inside uint32_t.
 * No floating point anywhere, so printf never needs float support.
 */
#define XADC_TEMP_GAIN_MILLI        503975UL
#define XADC_TEMP_OFFSET_MILLI      273150L

/* Only what this stage needs. CALIB keeps the offset and gain coefficients fresh. */
#define XADC_SEQ_CHANNELS           (XADCPS_SEQ_CH_CALIB | XADCPS_SEQ_CH_TEMP)

/* Averaging applies to measurement channels; the calibration channel is excluded. */
#define XADC_SEQ_AVG_CHANNELS       (XADCPS_SEQ_CH_TEMP)

static XAdcPs s_xadc_inst;
static bool   s_xadc_ready;

xadc_drv_status_t xadc_drv_init(void)
{
    XAdcPs_Config *xadc_cfg;

    s_xadc_ready = false;

    xadc_cfg = XAdcPs_LookupConfig(BOARD_XADC_ID);
    if (xadc_cfg == NULL)
    {
        return XADC_DRV_ERR_LOOKUP;
    }

    /* Also enables the XADCIF and sets up its FIFO thresholds. */
    if (XAdcPs_CfgInitialize(&s_xadc_inst, xadc_cfg, xadc_cfg->BaseAddress) != XST_SUCCESS)
    {
        return XADC_DRV_ERR_INIT;
    }

    /*
     * Register write and read-back plus an XADC reset. If this fails, the
     * XADCIF link is broken and no reading afterwards could be trusted.
     */
    if (XAdcPs_SelfTest(&s_xadc_inst) != XST_SUCCESS)
    {
        return XADC_DRV_ERR_SELFTEST;
    }

    /*
     * The sequencer has to be parked in safe mode while the channel and
     * averaging selections change; the driver refuses them otherwise.
     */
    XAdcPs_SetSequencerMode(&s_xadc_inst, XADCPS_SEQ_MODE_SAFE);

    /* Factory offset and gain calibration for the ADC and the supply measurements. */
    XAdcPs_SetCalibEnables(&s_xadc_inst,
                           XADCPS_CFR1_CAL_PS_GAIN_OFFSET_MASK | XADCPS_CFR1_CAL_ADC_GAIN_OFFSET_MASK);

    XAdcPs_SetAvg(&s_xadc_inst, XADCPS_AVG_16_SAMPLES);

    if (XAdcPs_SetSeqAvgEnables(&s_xadc_inst, XADC_SEQ_AVG_CHANNELS) != XST_SUCCESS)
    {
        return XADC_DRV_ERR_SEQ_CONFIG;
    }

    if (XAdcPs_SetSeqChEnables(&s_xadc_inst, XADC_SEQ_CHANNELS) != XST_SUCCESS)
    {
        return XADC_DRV_ERR_SEQ_CONFIG;
    }

    /*
     * Alarm thresholds stay at their power-on defaults on purpose: the
     * over-temperature alarm drives the automatic thermal shutdown, and there
     * is no reason to touch it here.
     */

    XAdcPs_SetSequencerMode(&s_xadc_inst, XADCPS_SEQ_MODE_CONTINPASS);

    s_xadc_ready = true;
    return XADC_DRV_OK;
}

xadc_drv_status_t xadc_drv_read_temp_code(uint16_t *code_out)
{
    if (code_out == NULL)
    {
        return XADC_DRV_ERR_ARG;
    }
    if (!s_xadc_ready)
    {
        return XADC_DRV_ERR_NOT_READY;
    }

    *code_out = (uint16_t)(XAdcPs_GetAdcData(&s_xadc_inst, XADCPS_CH_TEMP) >> XADC_RESULT_SHIFT);

    return XADC_DRV_OK;
}

int32_t xadc_drv_code_to_milli_c(uint16_t code)
{
    /* Half the divisor is added for round-to-nearest instead of truncation. */
    const uint32_t scaled = (((uint32_t)code * XADC_TEMP_GAIN_MILLI) + (XADC_CODE_FULL_SCALE / 2UL))
                            / XADC_CODE_FULL_SCALE;

    return (int32_t)scaled - XADC_TEMP_OFFSET_MILLI;
}
