/*
 * main.c - Stage 2: the CLI drives real hardware through the HAL.
 *
 * The UART ISR assembles received characters into complete lines (stage 1).
 * This super-loop takes each finished line, parses it, and now executes the
 * hardware commands through the HAL:
 *
 *   > LED_SET 0 1
 *   LED 0 (LD4 (PS MIO7)) is now ON
 *   > READ_TEMP
 *   Die temperature: 42.31 C
 *
 * Nothing here knows about MIO pins, XADC channels or registers, and no
 * Xilinx header is included: that is the HAL's side of the fence. Commands
 * that need the sequencer state machine (LED_BLINK, STOP) are still only
 * parsed and reported; they arrive in stage 3.
 *
 * Console: 115200 8N1 on the PROG/UART micro-USB port.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hal.h"
#include "parser.h"
#include "uart_driver.h"

#define APP_FW_VERSION          "0.2.0"
#define APP_CONSOLE_BAUD        115200U
#define APP_PROMPT              "> "

/* Longest formatted lines: the echo of a full 80-character command, and the HELP rows. */
#define APP_PRINT_BUF_LEN       160U

/* Enough for "-273.15" and its NUL, with room to spare. */
#define APP_NUM_BUF_LEN         16U

#define APP_LED_STATE_OFF       0
#define APP_LED_STATE_ON        1

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

/*
 * Fixed-point print of a milli-unit value with two decimals: 42310 -> "42.31".
 * Integer only, so the image never needs printf's floating point support.
 */
static const char *app_fmt_milli_2dp(char *buf, size_t buf_len, int32_t milli)
{
    const char *sign = "";
    uint32_t    magnitude;
    uint32_t    centi;

    if (milli < 0)
    {
        sign = "-";
        /* written this way so INT32_MIN doesn't overflow on negation */
        magnitude = (uint32_t)(-(milli + 1)) + 1U;
    }
    else
    {
        magnitude = (uint32_t)milli;
    }

    centi = (magnitude + 5U) / 10U;     /* round to the nearest hundredth */

    if (centi == 0U)
    {
        sign = "";                      /* no "-0.00" for a small negative value */
    }

    (void)snprintf(buf, buf_len, "%s%lu.%02lu", sign,
                   (unsigned long)(centi / 100U), (unsigned long)(centi % 100U));

    return buf;
}

static void app_print_banner(void)
{
    uart_driver_puts("\n\n"
                     "=========================================================\n"
                     " Event-driven hardware sequencer - stage 2, HAL\n");
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
                     "LED ids: 0 is the PS LED; 1..4 are the PL LEDs, which need the PL design.\n");
}

static void app_print_temperature(const char *indent)
{
    char               num_buf[APP_NUM_BUF_LEN];
    int32_t            milli_celsius = 0;
    const hal_status_t status        = HAL_ReadTemperature(&milli_celsius);

    if (status != HAL_OK)
    {
        app_printf("%serror: temperature sensor: %s\n", indent, HAL_StatusText(status));
        return;
    }

    app_printf("%sDie temperature: %s C\n", indent,
               app_fmt_milli_2dp(num_buf, sizeof(num_buf), milli_celsius));
}

static void app_print_led_states(void)
{
    uint8_t id;

    for (id = 0U; id < HAL_LEDCount(); id++)
    {
        bool               state  = false;
        const hal_status_t status = HAL_GetLED(id, &state);

        app_printf("  LED %u  %-14s %s\n", (unsigned int)id, HAL_LEDName(id),
                   (status == HAL_OK) ? (state ? "ON" : "OFF") : HAL_StatusText(status));
    }
}

static void app_print_status(void)
{
    uart_rx_stats_t stats;

    uart_driver_get_rx_stats(&stats);

    uart_driver_puts("Hardware\n");
    app_print_led_states();
    app_print_temperature("  ");

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

/*
 * LED_SET <led_id> <0|1>. The parser guarantees two integers; their ranges are
 * this layer's business, and the HAL checks the id again on its own side.
 */
static void app_cmd_led_set(const parser_result_t *result)
{
    const int32_t id    = result->args[0];
    const int32_t state = result->args[1];
    hal_status_t  status;

    if ((state != APP_LED_STATE_OFF) && (state != APP_LED_STATE_ON))
    {
        app_printf("error: state must be 0 (off) or 1 (on), not %ld\n", (long)state);
        return;
    }

    if ((id < 0) || (id >= (int32_t)HAL_LEDCount()))
    {
        app_printf("error: no LED with id %ld (valid: 0..%u)\n",
                   (long)id, (unsigned int)(HAL_LEDCount() - 1U));
        return;
    }

    status = HAL_SetLED((uint8_t)id, state == APP_LED_STATE_ON);
    if (status != HAL_OK)
    {
        app_printf("error: LED %ld (%s): %s\n", (long)id, HAL_LEDName((uint8_t)id), HAL_StatusText(status));
        return;
    }

    app_printf("LED %ld (%s) is now %s\n", (long)id, HAL_LEDName((uint8_t)id),
               (state == APP_LED_STATE_ON) ? "ON" : "OFF");
}

/* Reports a command the parser understood but no stage has implemented yet. */
static void app_cmd_not_yet(const parser_result_t *result, const char *reason)
{
    uint8_t idx;

    app_printf("Parsed Command: %s", parser_command_info(result->command)->keyword);
    for (idx = 0U; idx < result->arg_count; idx++)
    {
        app_printf("%s%ld", (idx == 0U) ? ", Args: " : ", ", (long)result->args[idx]);
    }
    uart_driver_puts("\n");

    app_printf("not yet: %s\n", reason);
}

static void app_execute(const parser_result_t *result)
{
    switch (result->command)
    {
    case CMD_HELP:
        app_print_help();
        break;

    case CMD_STATUS:
        app_print_status();
        break;

    case CMD_LED_SET:
        app_cmd_led_set(result);
        break;

    case CMD_READ_TEMP:
        app_print_temperature("");
        break;

    case CMD_LED_BLINK:
        app_cmd_not_yet(result, "timed sequences arrive with the FSM in stage 3");
        break;

    case CMD_STOP:
        app_cmd_not_yet(result, "there is nothing running to stop until stage 3");
        break;

    case CMD_COUNT:
        break;
    }
}

static void app_handle_parse_result(const uart_line_t *line, parser_status_t status, const parser_result_t *result)
{
    switch (status)
    {
    case PARSER_OK:
        app_execute(result);
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
    uart_line_t                line;
    const uart_driver_status_t uart_status = uart_driver_init(APP_CONSOLE_BAUD);
    hal_status_t               hal_status;

    if (uart_status != UART_DRIVER_OK)
    {
        /* Prints only if the UART got far enough to transmit (UART_DRIVER_ERR_IRQ). */
        app_printf("\nFATAL: UART driver init failed (code %d), halting.\n", (int)uart_status);
        for (;;)
        {
        }
    }

    app_print_banner();

    /*
     * A hardware failure is not fatal for a command line: the CLI still runs,
     * and each affected command says what is wrong when it is used.
     */
    hal_status = HAL_Init();
    if (hal_status != HAL_OK)
    {
        app_printf("warning: %s\n", HAL_StatusText(hal_status));
    }

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
