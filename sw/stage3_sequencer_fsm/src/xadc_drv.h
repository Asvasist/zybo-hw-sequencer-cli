/*
 * xadc_drv.h
 *
 * Die temperature from the XADC, through the PS-XADC interface (DevC XADCIF).
 *
 * No PL logic is involved: the PS reaches the XADC hard macro over its own
 * serial link, so this works on a PS-only design with no bitstream.
 *
 * The sequencer runs continuously over the temperature and calibration
 * channels with 16x averaging, so a read picks up the latest averaged result
 * and never waits for a conversion.
 *
 * Not reentrant: every read is a command/response exchange over the XADCIF
 * FIFOs, and in this stage only the main loop reads it. The conversion helper
 * is pure and can be called from anywhere, host tests included.
 */
#ifndef XADC_DRV_H
#define XADC_DRV_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    XADC_DRV_OK = 0,
    XADC_DRV_ERR_LOOKUP,        /* no XADC in the hardware design        */
    XADC_DRV_ERR_INIT,          /* XAdcPs_CfgInitialize() failed         */
    XADC_DRV_ERR_SELFTEST,      /* the XADCIF link itself is broken      */
    XADC_DRV_ERR_SEQ_CONFIG,    /* sequencer rejected the channel setup  */
    XADC_DRV_ERR_NOT_READY,     /* xadc_drv_init() has not succeeded     */
    XADC_DRV_ERR_ARG            /* NULL output pointer                   */
} xadc_drv_status_t;

xadc_drv_status_t xadc_drv_init(void);

/* Latest averaged temperature as a raw 12-bit ADC code (0..4095). */
xadc_drv_status_t xadc_drv_read_temp_code(uint16_t *code_out);

/* UG480 transfer function, in milli-degrees Celsius. Pure, no hardware access. */
int32_t xadc_drv_code_to_milli_c(uint16_t code);

#endif /* XADC_DRV_H */
