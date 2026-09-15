/*
 * sim_session.c
 *
 * Runs the real stage 1 firmware - main.c, parser.c and uart_driver.c,
 * unchanged - on a PC against the UART model in mock/. A scripted "user"
 * types into the modelled RX pin whenever the firmware shows its prompt, and
 * the output is rendered the way a terminal (local echo off) would show it.
 * No board needed:
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -DSDT -Dmain=firmware_main -Imock -I../../src \
 *       sim_session.c mock/mock_uart.c ../../src/main.c ../../src/parser.c ../../src/uart_driver.c \
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

#include "mock_uart.h"

#define TERM_LINE_LEN   256U

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
    TYPE("LED_BLINK 500\r"),
    TYPE("led_set 2 1\r\n"),
    TYPE("LED_BLINK 10000 250\r"),
    TYPE("READ_TEMP\r"),
    TYPE("LED_SEX\bT 0 0\r"),
    TYPE("\r"),
    TYPE("LED_BLINK\r"),
    TYPE("READ_TEMP now\r"),
    TYPE("LED_BLINK 12abc\r"),
    TYPE("LED_SET 1 99999999999\r"),
    TYPE("FLASH_LED 3\r"),
    TYPE("\x1b[A\r"),
    TYPE("LED_BLINK 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30\r"),
    LINE_ERROR("STOP\r"),
    /* A pasted script: ten lines arrive before the firmware gets to the first one. */
    TYPE("LED_SET 0 1\rLED_SET 1 1\rLED_SET 2 1\rLED_SET 3 1\rLED_BLINK 100\r"
         "STOP\rLED_SET 0 0\rLED_SET 1 0\rLED_SET 2 0\rLED_SET 3 0\r"),
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

int main(void)
{
    mock_uart_reset();
    mock_uart_set_tx_hook(on_tx_byte);

    (void)firmware_main();

    /* firmware_main() never returns; the session ends in on_tx_byte(). */
    return 1;
}
