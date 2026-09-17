/*
 * sim_session.c
 *
 * Runs the real stage 3 firmware - main.c, sequencer.c, hal.c, the drivers,
 * parser.c and uart_driver.c, all unchanged - on a PC against the models in
 * mock/. A scripted "user" types at the prompt, the models play the UART
 * wire, the interrupt controller and the clock, and the output is rendered
 * the way a terminal (local echo off) would show it. Every MIO pin change is
 * printed as a [board] line, so a blink is visible between the responses.
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -DSDT -Dmain=firmware_main -Imock -I../../src \
 *       sim_session.c mock/mock_uart.c mock/mock_board.c mock/mock_time.c \
 *       ../../src/main.c ../../src/sequencer.c ../../src/hal.c ../../src/gpio_drv.c \
 *       ../../src/xadc_drv.c ../../src/uptime_drv.c ../../src/parser.c ../../src/uart_driver.c \
 *       -o sim_session
 *   ./sim_session
 *
 * -Dmain=firmware_main renames the firmware's main(); this file takes it back.
 *
 * Simulated time moves on every clock read, so the main loop's own passes
 * carry the sequence forward. The script therefore ends with a short blink
 * and the session closes when that sequence reports itself finished.
 */
#undef main

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mock_board.h"
#include "mock_time.h"
#include "mock_uart.h"

#define TERM_LINE_LEN       256U

/* 0xA19: a Zynq at 44.91 C. */
#define SIM_TEMP_CODE       2585U

/* One clock read moves the world on by a millisecond. */
#define SIM_MS_PER_READ_US  1000U

/* Stops a runaway session rather than filling the disk. */
#define SIM_MAX_TX_BYTES    200000U

int firmware_main(void);

typedef enum
{
    STEP_TYPE,              /* type a line, one interrupt per character */
    STEP_WAIT_TOGGLES       /* let the firmware run until the LED has changed N times */
} step_kind_t;

typedef struct
{
    step_kind_t kind;
    const char *bytes;
    size_t      length;
    unsigned    toggles;
} step_t;

#define TYPE(text)      { STEP_TYPE, (text), sizeof(text) - 1U, 0U }
#define WAIT(n)         { STEP_WAIT_TOGGLES, NULL, 0U, (n) }

static const step_t s_script[] =
{
    TYPE("HELP\r"),
    TYPE("READ_TEMP\r"),
    TYPE("LED_BLINK 2000 250\r"),
    WAIT(3U),                       /* the LED blinks on its own ... */
    TYPE("STATUS\r"),               /* ... and the CLI still answers */
    TYPE("READ_TEMP\r"),
    TYPE("LED_BLINK 500 100\r"),    /* refused: one sequence at a time */
    WAIT(2U),
    TYPE("STOP\r"),
    TYPE("STATUS\r"),
    TYPE("LED_BLINK 300 100\r"),    /* short, so the session ends when it finishes */
};

static const size_t s_step_count = sizeof(s_script) / sizeof(s_script[0]);

static size_t   s_next_step;
static unsigned s_toggles_left;
static bool     s_saw_finished;
static unsigned s_tx_bytes;
static char     s_tx_tail[3];
static char     s_term_line[TERM_LINE_LEN];
static size_t   s_term_col;
static size_t   s_term_len;

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

        if (strncmp(s_term_line, "sequence finished", strlen("sequence finished")) == 0)
        {
            s_saw_finished = true;
        }

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

static void type_line(const step_t *step)
{
    size_t idx;

    for (idx = 0U; idx < step->length; idx++)
    {
        (void)mock_uart_receive(&step->bytes[idx], 1U);
    }
}

/*
 * Runs script steps until one has to wait. Typing is safe at any moment: the
 * characters land in the receive FIFO and the ISR picks them up, exactly as
 * they would from a terminal.
 */
static void advance_script(void)
{
    while (s_next_step < s_step_count)
    {
        const step_t *const step = &s_script[s_next_step];

        if (step->kind == STEP_WAIT_TOGGLES)
        {
            if (s_toggles_left == 0U)
            {
                s_toggles_left = step->toggles;
            }
            return;                     /* the pin hook counts it down */
        }

        type_line(step);
        s_next_step++;
        return;                         /* one line per prompt */
    }
}

/* Every byte the firmware transmits. A prompt on a fresh line means "your turn". */
static void on_tx_byte(u8 byte)
{
    term_put((char)byte);

    s_tx_bytes++;
    if (s_tx_bytes > SIM_MAX_TX_BYTES)
    {
        printf("[sim] giving up: %u bytes and no end in sight\n", s_tx_bytes);
        exit(2);
    }

    s_tx_tail[0] = s_tx_tail[1];
    s_tx_tail[1] = s_tx_tail[2];
    s_tx_tail[2] = (char)byte;

    if ((s_tx_tail[0] == '\n') && (s_tx_tail[1] == '>') && (s_tx_tail[2] == ' '))
    {
        if ((s_next_step == s_step_count) && s_saw_finished)
        {
            printf("%.*s\n", (int)s_term_len, s_term_line);
            exit(0);
        }

        advance_script();
    }
}

/* Every MIO pin the firmware drives: the blink, seen from the board side. */
static void on_pin_write(u32 pin, u32 level)
{
    printf("             [board] MIO%u -> %u\n", (unsigned int)pin, (unsigned int)level);

    if (s_toggles_left > 0U)
    {
        s_toggles_left--;

        if (s_toggles_left == 0U)
        {
            s_next_step++;              /* the wait step is done */
            advance_script();
        }
    }
}

int main(void)
{
    mock_uart_reset();
    mock_board_reset();
    mock_time_reset();

    mock_board_set_temp_code(SIM_TEMP_CODE);

    /* The wire takes every byte offered, and an enabled interrupt fires at once. */
    mock_uart_set_tx_autoshift(true);
    mock_uart_set_autoservice(true);

    /* Time moves when the firmware looks at the clock, which it does every pass. */
    mock_time_set_drift_per_read_us(SIM_MS_PER_READ_US);

    mock_uart_set_tx_hook(on_tx_byte);
    mock_board_set_pin_hook(on_pin_write);

    (void)firmware_main();

    /* firmware_main() never returns; the session ends in on_tx_byte(). */
    return 1;
}
