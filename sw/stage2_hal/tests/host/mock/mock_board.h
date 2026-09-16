/*
 * mock_board.h - test-side controls of the host board model.
 *
 * mock_board.c models the parts of the Zynq that the stage 2 drivers use
 * besides the UART: the PS MIO GPIO pins (level, direction, output enable)
 * and the XADC (sequencer settings and the temperature result). gpio_drv.c
 * and xadc_drv.c are compiled against it unchanged - no test hooks in the
 * production code.
 */
#ifndef MOCK_BOARD_H
#define MOCK_BOARD_H

#include <stdbool.h>

#include "xadcps.h"
#include "xgpiops.h"

#define MOCK_MIO_PIN_COUNT      54U     /* the Zynq has MIO0..MIO53 */

/* Value of the "mode while ... was set" fields when that call never happened. */
#define MOCK_SEQ_MODE_UNSET     0xFFU

typedef struct
{
    u32      level;
    u32      direction;             /* 1 = output */
    u32      output_enable;         /* 1 = driver enabled */
    unsigned writes;
    bool     first_write_had_oe;    /* was the output already enabled on the first write? */
} mock_pin_t;

typedef struct
{
    /* --- failure injection, set after mock_board_reset() --- */
    bool gpio_fail_lookup;
    bool gpio_fail_init;
    bool xadc_fail_lookup;
    bool xadc_fail_init;
    bool xadc_fail_selftest;
    bool xadc_fail_seq_channels;
    bool xadc_fail_seq_average;

    /* --- GPIO --- */
    bool       gpio_initialised;
    mock_pin_t pin[MOCK_MIO_PIN_COUNT];
    unsigned   gpio_bad_pin_accesses;      /* a pin outside MIO0..MIO53 */
    unsigned   gpio_use_before_init;

    /* --- XADC --- */
    bool     xadc_initialised;
    unsigned selftest_calls;
    u8       seq_mode;                     /* last mode set                         */
    u8       seq_mode_at_channel_setup;    /* mode while the channels were selected */
    u8       seq_mode_at_average_setup;
    u8       average;
    u16      calibration_mask;
    u32      seq_channels;
    u32      seq_average_channels;
    unsigned xadc_use_before_init;
    unsigned xadc_bad_channel_reads;       /* a channel this board doesn't convert  */
} mock_board_state_t;

extern mock_board_state_t mock_board;

/* Power-on state: pins high-impedance, no XADC configuration, no failures. */
void mock_board_reset(void);

/* Sets what the XADC reports for the temperature channel, as a 12-bit code. */
void mock_board_set_temp_code(u16 code);

/* Called on every pin write; the simulator uses it to show the board reacting. */
typedef void (*mock_board_pin_hook_t)(u32 pin, u32 level);
void mock_board_set_pin_hook(mock_board_pin_hook_t hook);

#endif /* MOCK_BOARD_H */
