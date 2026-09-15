/*
 * main.c - Stage 1: interrupt-driven UART command parser.
 *
 * The UART ISR assembles incoming characters into complete lines. This super-
 * loop takes each finished line, parses it, and prints what it understood:
 *
 *   > LED_BLINK 500
 *   Parsed Command: LED_BLINK, Arg: 500
 *
 * There is no HAL and no sequencer FSM yet, so the hardware commands are only
 * parsed and confirmed. HELP and STATUS act already, since they only involve
 * the CLI itself.
 *
 * Console: 115200 8N1 on the PROG/UART micro-USB port.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "parser.h"
#include "uart_driver.h"

#define APP_FW_VERSION          "0.1.0"
#define APP_CONSOLE_BAUD        115200U
#define APP_PROMPT              "> "

/* Longest formatted lines: the echo of a full 80-character command, and the HELP rows. */
#define APP_PRINT_BUF_LEN       160U

/*
 * Formatting buffer for app_printf(). Static, not on the stack and not from
 * the heap; only the main loop prints, so one buffer is enough. vsnprintf()
 * with integer and string conversions doesn't allocate.
 */
static char s_print_buf[APP_PRINT_BUF_LEN];

/* lines_dropped as last reported, to print each new batch of drops once. */
static uint32_t s_reported_drops;

static void app_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void app_printf(const char *fmt, ...)
{
    va_list args;
    int     needed_len;

    va_start(args, fmt);
    needed_len = vsnprintf(s_print_buf, sizeof(s_print_buf), fmt, args);
    va_end(args);

    if (needed_len < 0)
    {
        return;
    }

    uart_driver_puts(s_print_buf);

    /* Make truncation visible instead of silently chopping the line. */
    if ((size_t)needed_len >= sizeof(s_print_buf))
    {
        uart_driver_puts("~\n");
    }
}

static void app_print_banner(void)
{
    uart_driver_puts("\n\n"
                     "=========================================================\n"
                     " Event-driven hardware sequencer - stage 1, UART parser\n");
    app_printf(" fw %s, built %s %s\n", APP_FW_VERSION, __DATE__, __TIME__);
    uart_driver_puts("=========================================================\n");
}

static void app_print_help(void)
{
    cmd_id_t id;

    uart_driver_puts("Commands (case-insensitive, arguments are decimal integers):\n");

    for (id = (cmd_id_t)0; id < CMD_COUNT; id++)
    {
        const parser_cmd_info_t *const info = parser_command_info(id);

        app_printf("  %-10s %-26s %s\n", info->keyword, info->arg_usage, info->summary);
    }

    uart_driver_puts("A line ends with CR, LF or CR LF; Backspace edits it.\n"
                     "Stage 1 parses every command; only HELP and STATUS act on it.\n");
}

static void app_print_rx_stats(void)
{
    uart_rx_stats_t stats;

    uart_driver_get_rx_stats(&stats);

    uart_driver_puts("UART receive statistics\n");
    app_printf("  bytes received : %lu\n", (unsigned long)stats.bytes_received);
    app_printf("  lines received : %lu (%lu too long)\n",
               (unsigned long)stats.lines_received, (unsigned long)stats.lines_too_long);
    app_printf("  lines dropped  : %lu (queue full)\n", (unsigned long)stats.lines_dropped);
    app_printf("  line errors    : overrun %lu, framing %lu, parity %lu\n",
               (unsigned long)stats.overrun_errors,
               (unsigned long)stats.framing_errors,
               (unsigned long)stats.parity_errors);
    app_printf("  line queue     : %u lines of up to %u characters\n",
               (unsigned int)UART_LINE_QUEUE_DEPTH, (unsigned int)UART_LINE_MAX_LEN);
}

static void app_print_usage(cmd_id_t command)
{
    const parser_cmd_info_t *const info = parser_command_info(command);

    if (info != NULL)
    {
        app_printf("usage: %s%s%s\n", info->keyword, (info->arg_usage[0] != '\0') ? " " : "", info->arg_usage);
    }
}

/* The stage 1 proof of life: echo back exactly what the parser extracted. */
static void app_print_parsed(const parser_result_t *result)
{
    uint8_t idx;

    app_printf("Parsed Command: %s", parser_command_info(result->command)->keyword);

    if (result->arg_count == 0U)
    {
        uart_driver_puts(", Args: none\n");
    }
    else if (result->arg_count == 1U)
    {
        app_printf(", Arg: %ld\n", (long)result->args[0]);
    }
    else
    {
        uart_driver_puts(", Args: ");
        for (idx = 0U; idx < result->arg_count; idx++)
        {
            app_printf("%s%ld", (idx == 0U) ? "" : ", ", (long)result->args[idx]);
        }
        uart_driver_puts("\n");
    }
}

/*
 * Executes the commands that only concern the CLI. The hardware commands get
 * their handlers once the HAL (stage 2) and the sequencer FSM (stage 3) exist.
 */
static void app_run_cli_command(cmd_id_t command)
{
    switch (command)
    {
    case CMD_HELP:
        app_print_help();
        break;

    case CMD_STATUS:
        app_print_rx_stats();
        break;

    case CMD_LED_SET:
    case CMD_LED_BLINK:
    case CMD_READ_TEMP:
    case CMD_STOP:
    case CMD_COUNT:
        break;
    }
}

static void app_handle_parse_result(const uart_line_t *line, parser_status_t status, const parser_result_t *result)
{
    switch (status)
    {
    case PARSER_OK:
        app_print_parsed(result);
        app_run_cli_command(result->command);
        break;

    case PARSER_EMPTY:
        break;

    case PARSER_ERR_BAD_CHARACTER:
        app_printf("error: %s 0x%02X at column %u\n",
                   parser_status_text(status),
                   (unsigned int)(uint8_t)line->text[result->error_offset],
                   (unsigned int)result->error_offset + 1U);
        break;

    case PARSER_ERR_UNKNOWN_COMMAND:
        app_printf("error: %s '%.*s', type HELP for the list\n",
                   parser_status_text(status),
                   (int)result->error_length, &line->text[result->error_offset]);
        break;

    case PARSER_ERR_BAD_NUMBER:
    case PARSER_ERR_TOO_MANY_ARGS:
        app_printf("error: %s: '%.*s'\n",
                   parser_status_text(status),
                   (int)result->error_length, &line->text[result->error_offset]);
        app_print_usage(result->command);
        break;

    case PARSER_ERR_TOO_FEW_ARGS:
        app_printf("error: %s\n", parser_status_text(status));
        app_print_usage(result->command);
        break;

    case PARSER_ERR_NULL_ARG:
        app_printf("error: %s\n", parser_status_text(status));
        break;
    }
}

/*
 * Redraws the received line after the prompt, so the command shows up next to
 * its response whether the terminal echoes locally or not, and a pasted script
 * leaves a readable transcript. Control bytes (arrow keys send ESC sequences)
 * are shown as '.' instead of being sent back for the terminal to act on.
 */
static void app_echo_line(const uart_line_t *line)
{
    char     safe_text[UART_LINE_MAX_LEN + 1U];
    uint16_t idx;

    for (idx = 0U; idx < line->length; idx++)
    {
        const unsigned char ch = (unsigned char)line->text[idx];

        if (ch == (unsigned char)'\t')
        {
            safe_text[idx] = ' ';
        }
        else
        {
            safe_text[idx] = ((ch >= (unsigned char)' ') && (ch <= (unsigned char)'~')) ? (char)ch : '.';
        }
    }
    safe_text[line->length] = '\0';

    app_printf("\r" APP_PROMPT "%s\n", safe_text);
}

static void app_handle_line(const uart_line_t *line)
{
    parser_result_t result;
    parser_status_t status;

    app_echo_line(line);

    switch (line->status)
    {
    case UART_LINE_OK:
        break;

    case UART_LINE_TOO_LONG:
        app_printf("error: line longer than %u characters, discarded\n", (unsigned int)UART_LINE_MAX_LEN);
        return;

    case UART_LINE_RX_ERROR:
        uart_driver_puts("error: receive error (framing, parity or overrun), line discarded\n");
        return;
    }

    status = parser_parse(line->text, line->length, &result);
    app_handle_parse_result(line, status, &result);
}

static void app_report_dropped_lines(void)
{
    uart_rx_stats_t stats;

    uart_driver_get_rx_stats(&stats);

    if (stats.lines_dropped != s_reported_drops)
    {
        app_printf("warning: %lu line(s) dropped, receive queue was full\n",
                   (unsigned long)(stats.lines_dropped - s_reported_drops));
        s_reported_drops = stats.lines_dropped;
    }
}

int main(void)
{
    uart_line_t          line;
    uart_driver_status_t status;

    status = uart_driver_init(APP_CONSOLE_BAUD);
    if (status != UART_DRIVER_OK)
    {
        /* Prints only if the UART got far enough to transmit (UART_DRIVER_ERR_IRQ). */
        app_printf("\nFATAL: UART driver init failed (code %d), halting.\n", (int)status);
        for (;;)
        {
        }
    }

    app_print_banner();
    uart_driver_puts("Type HELP for the command list.\n" APP_PROMPT);

    for (;;)
    {
        if (uart_driver_line_ready())
        {
            /* Everything that queued up while the last response was printing. */
            while (uart_driver_take_line(&line))
            {
                app_handle_line(&line);
            }

            app_report_dropped_lines();
            uart_driver_puts(APP_PROMPT);
        }

        /*
         * Nothing else to do yet. From stage 3 the sequencer FSM gets its
         * non-blocking step here, between two looks at the line queue.
         */
    }

    /* not reached */
    return 0;
}
