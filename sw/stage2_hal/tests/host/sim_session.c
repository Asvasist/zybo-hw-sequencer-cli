/*
 * sim_session.c
 *
 * Runs the real stage 2 firmware - main.c, hal.c, gpio_drv.c, xadc_drv.c,
 * parser.c and uart_driver.c, all unchanged - on a PC against the UART and
 * board models in mock/. A scripted "user" types into the modelled RX pin
 * whenever the firmware shows its prompt, the output is rendered the way a
 * terminal (local echo off) would show it, and every MIO pin change the
 * firmware makes is printed as a [board] line. No hardware needed:
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -DSDT -Dmain=firmware_main -Imock -I../../src \
 *       sim_session.c mock/mock_uart.c mock/mock_board.c ../../src/main.c ../../src/hal.c \
 *       ../../src/gpio_drv.c ../../src/xadc_drv.c ../../src/parser.c ../../src/uart_driver.c \
 *       -o sim_session
 *   ./sim_session
 *
 * -Dmain=firmware_main renames the firmware's main(); this file takes it back.
 */
#undef main

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "mock_board.h"
#include "mock_uart.h"

#define TERM_LINE_LEN       256U

/* 0xA19: a Zynq at 44.91 C, the value the stage 1 sibling project measured. */
#define SIM_TEMP_CODE       2585U

int firmware_main(void);

typedef enum
{
    STEP_TYPE,              /* one interrupt per byte, as when typing */
    STEP_FRAMING_ERROR      /* like STEP_TYPE, with a framing error on the first byte */
} step_kind_t;

typedef struct
{
    step_kind_t kind;
    const char *bytes;
    size_t      length;
} step_t;

#define TYPE(text)          { STEP_TYPE, (text), sizeof(text) - 1U }
#define LINE_ERROR(text)    { STEP_FRAMING_ERROR, (text), sizeof(text) - 1U }

/* Each step runs when the firmware prints a fresh prompt. */
static const step_t s_script[] =
{
    TYPE("HELP\r"),
    TYPE("READ_TEMP\r"),
    TYPE("LED_SET 0 1\r"),
    TYPE("led_set 0 0\r"),
    TYPE("LED_SET 1 1\r"),
    TYPE("LED_SET 9 1\r"),
    TYPE("LED_SET 0 2\r"),
    TYPE("LED_SET 0\r"),
    TYPE("LED_BLINK 10000 250\r"),
    TYPE("STOP\r"),
    TYPE("FLASH_LED 3\r"),
    LINE_ERROR("READ_TEMP\r"),
    TYPE("LED_SET 0 1\r"),
    TYPE("STATUS\r"),
};

static const size_t s_step_count = sizeof(s_script) / sizeof(s_script[0]);

static size_t s_next_step;
static char   s_tx_tail[3];
static char   s_term_line[TERM_LINE_LEN];
static size_t s_term_col;
static size_t s_term_len;

/* Minimal terminal: CR returns to column 0 and overwrites, LF ends the line. */
static void term_put(char ch)
{
    if (ch == '\r')
    {
        s_term_col = 0U;
    }
    else if (ch == '\n')
    {
        printf("%.*s\n", (int)s_term_len, s_term_line);
        s_term_col = 0U;
        s_term_len = 0U;
    }
    else if (s_term_col < TERM_LINE_LEN)
    {
        s_term_line[s_term_col] = ch;
        s_term_col++;
        if (s_term_col > s_term_len)
        {
            s_term_len = s_term_col;
        }
    }
}

static void run_step(const step_t *step)
{
    size_t idx;

    for (idx = 0U; idx < step->length; idx++)
    {
        if ((idx == 0U) && (step->kind == STEP_FRAMING_ERROR))
        {
            mock_uart_raise_status(XUARTPS_IXR_FRAMING);
        }
        (void)mock_uart_receive(&step->bytes[idx], 1U);
        (void)mock_uart_service_irq();
    }
}

/* Sees every byte the firmware transmits. A prompt at the start of a line means "your turn". */
static void on_tx_byte(u8 byte)
{
    term_put((char)byte);

    s_tx_tail[0] = s_tx_tail[1];
    s_tx_tail[1] = s_tx_tail[2];
    s_tx_tail[2] = (char)byte;

    if ((s_tx_tail[0] == '\n') && (s_tx_tail[1] == '>') && (s_tx_tail[2] == ' '))
    {
        if (s_next_step == s_step_count)
        {
            printf("%.*s\n", (int)s_term_len, s_term_line);
            exit(0);
        }

        run_step(&s_script[s_next_step]);
        s_next_step++;
    }
}

/* Every MIO pin the firmware drives, so the hardware effect is visible too. */
static void on_pin_write(u32 pin, u32 level)
{
    printf("             [board] MIO%u -> %u\n", (unsigned int)pin, (unsigned int)level);
}

int main(void)
{
    mock_uart_reset();
    mock_board_reset();
    mock_board_set_temp_code(SIM_TEMP_CODE);

    mock_uart_set_tx_hook(on_tx_byte);
    mock_board_set_pin_hook(on_pin_write);

    (void)firmware_main();

    /* firmware_main() never returns; the session ends in on_tx_byte(). */
    return 1;
}
