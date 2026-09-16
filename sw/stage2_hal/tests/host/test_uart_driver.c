/*
 * test_uart_driver.c
 *
 * Host-side unit tests for uart_driver.c, run against the UART model in mock/.
 * The ISR is the real one; the tests feed bytes into the modelled RX FIFO and
 * play the interrupt controller. Any C11 compiler:
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -DSDT -Imock -I../../src \
 *       test_uart_driver.c mock/mock_uart.c ../../src/uart_driver.c -o test_uart_driver
 *   ./test_uart_driver
 *
 * Exit code 0 means every check passed.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mock_uart.h"
#include "xinterrupt_wrap.h"
#include "xparameters.h"

#include "uart_driver.h"

#define TEST_BAUD               115200U
#define WRAP_TEST_LINES         1000U
#define LINE_TEXT_LEN           32U

#define RX_IRQ_SOURCES          (XUARTPS_IXR_RXOVR | XUARTPS_IXR_OVER | XUARTPS_IXR_FRAMING | XUARTPS_IXR_PARITY)

static unsigned s_checks_run;
static unsigned s_checks_failed;

#define CHECK(cond)                                                             \
    do                                                                          \
    {                                                                           \
        s_checks_run++;                                                         \
        if (!(cond))                                                            \
        {                                                                       \
            s_checks_failed++;                                                  \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                       \
    } while (0)

#define CHECK_NEXT_LINE(text, status)   check_next_line((text), (status), __LINE__)

static const char *line_status_name(uart_line_status_t status)
{
    switch (status)
    {
    case UART_LINE_OK:       return "OK";
    case UART_LINE_TOO_LONG: return "TOO_LONG";
    case UART_LINE_RX_ERROR: return "RX_ERROR";
    }
    return "?";
}

/* Takes the next line and compares text, length and status. */
static void check_next_line(const char *text, uart_line_status_t status, int src_line)
{
    uart_line_t line;
    const bool  taken = uart_driver_take_line(&line);

    s_checks_run++;
    if (!taken)
    {
        s_checks_failed++;
        printf("  FAIL %s:%d: expected line \"%s\", queue empty\n", __FILE__, src_line, text);
        return;
    }

    if ((line.length != strlen(text)) || (strcmp(line.text, text) != 0) || (line.status != status))
    {
        s_checks_failed++;
        printf("  FAIL %s:%d: expected \"%s\" (%s), got \"%s\" len %u (%s)\n", __FILE__, src_line,
               text, line_status_name(status), line.text, (unsigned)line.length, line_status_name(line.status));
    }
}

/* A correct driver leaves the model in a sane state after every test. */
static void check_model_clean(void)
{
    CHECK(mock_uart.bad_base_accesses == 0U);
    CHECK(mock_uart.unexpected_accesses == 0U);
    CHECK(!mock_uart.irq_storm);
    CHECK(mock_uart_rx_fifo_count() == 0U);
}

static void boot_driver(void)
{
    mock_uart_reset();
    CHECK(uart_driver_init(TEST_BAUD) == UART_DRIVER_OK);
    mock_uart_tx_clear();
}

/* One interrupt per byte - what a trigger level of 1 does at human typing speed. */
static void type_bytes(const char *bytes, size_t length)
{
    size_t idx;

    for (idx = 0U; idx < length; idx++)
    {
        CHECK(mock_uart_receive(&bytes[idx], 1U));
        (void)mock_uart_service_irq();
    }
}

static void type_text(const char *text)
{
    type_bytes(text, strlen(text));
}

/* Several bytes per interrupt, as if the ISR was held off for a while. */
static void burst_text(const char *text)
{
    size_t remaining = strlen(text);

    while (remaining > 0U)
    {
        const size_t chunk = (remaining < MOCK_UART_RX_FIFO_DEPTH) ? remaining : MOCK_UART_RX_FIFO_DEPTH;

        CHECK(mock_uart_receive(text, chunk));
        (void)mock_uart_service_irq();
        text      += chunk;
        remaining -= chunk;
    }
}

static void type_repeated(char ch, size_t count)
{
    size_t idx;

    for (idx = 0U; idx < count; idx++)
    {
        type_bytes(&ch, 1U);
    }
}

static void test_init_configures_uart(void)
{
    uart_rx_stats_t stats;

    printf("init configures the UART and the interrupt\n");

    mock_uart_reset();
    /* Keys pressed and a line error during boot, before the driver runs. */
    mock_uart.rx_trigger_level = 1U;
    CHECK(mock_uart_receive("junk\r", 5U));
    mock_uart_raise_status(XUARTPS_IXR_FRAMING);

    CHECK(uart_driver_init(TEST_BAUD) == UART_DRIVER_OK);

    CHECK(mock_uart.baud_rate == TEST_BAUD);
    CHECK(mock_uart.data_bits == XUARTPS_FORMAT_8_BITS);
    CHECK(mock_uart.parity == XUARTPS_FORMAT_NO_PARITY);
    CHECK(mock_uart.stop_bits == XUARTPS_FORMAT_1_STOP_BIT);
    CHECK(mock_uart.oper_mode == XUARTPS_OPER_MODE_NORMAL);
    CHECK(mock_uart.rx_trigger_level == 1U);
    CHECK(mock_uart.rx_timeout == 0U);
    CHECK(mock_uart.imr == RX_IRQ_SOURCES);
    CHECK(mock_uart.irq_connected);
    CHECK(mock_uart.irq_id == XPAR_XUARTPS_0_INTERRUPTS);
    CHECK(mock_uart.irq_parent == XPAR_XUARTPS_0_INTERRUPT_PARENT);
    CHECK(mock_uart.irq_priority == XINTERRUPT_DEFAULT_PRIORITY);

    /* Boot junk flushed, stale status acknowledged, nothing counted. */
    CHECK(mock_uart_rx_fifo_count() == 0U);
    CHECK(mock_uart_isr_bits() == 0U);
    CHECK(mock_uart_service_irq() == 0U);
    CHECK(!uart_driver_line_ready());
    uart_driver_get_rx_stats(&stats);
    CHECK(stats.bytes_received == 0U);
    CHECK(stats.lines_received == 0U);
    CHECK(stats.framing_errors == 0U);

    check_model_clean();
}

static void test_init_failures(void)
{
    printf("init failures\n");

    mock_uart_reset();
    mock_uart.fail_lookup = true;
    CHECK(uart_driver_init(TEST_BAUD) == UART_DRIVER_ERR_LOOKUP);
    uart_driver_puts("no\n");
    CHECK(strcmp(mock_uart_tx_text(), "") == 0);

    mock_uart_reset();
    mock_uart.fail_cfg_init = true;
    CHECK(uart_driver_init(TEST_BAUD) == UART_DRIVER_ERR_INIT);
    CHECK(!mock_uart.irq_connected);

    mock_uart_reset();
    mock_uart.fail_set_format = true;
    CHECK(uart_driver_init(TEST_BAUD) == UART_DRIVER_ERR_FORMAT);
    uart_driver_puts("no\n");
    CHECK(strcmp(mock_uart_tx_text(), "") == 0);
    CHECK(!mock_uart.irq_connected);

    /* Interrupt setup failed: RX stays masked, but TX works to report it. */
    mock_uart_reset();
    mock_uart.fail_irq_setup = true;
    CHECK(uart_driver_init(TEST_BAUD) == UART_DRIVER_ERR_IRQ);
    CHECK(mock_uart.imr == 0U);
    uart_driver_puts("irq failed\n");
    CHECK(strcmp(mock_uart_tx_text(), "irq failed\r\n") == 0);

    check_model_clean();
}

static void test_single_line(void)
{
    uart_rx_stats_t stats;
    uart_line_t     line;

    printf("single typed line\n");
    boot_driver();

    type_text("LED_BLINK 50");
    CHECK(!uart_driver_line_ready());
    type_text("0\r");
    CHECK(uart_driver_line_ready());

    CHECK_NEXT_LINE("LED_BLINK 500", UART_LINE_OK);
    CHECK(!uart_driver_line_ready());
    CHECK(!uart_driver_take_line(&line));

    uart_driver_get_rx_stats(&stats);
    CHECK(stats.bytes_received == 14U);
    CHECK(stats.lines_received == 1U);
    CHECK(stats.lines_dropped == 0U);

    check_model_clean();
}

static void test_line_endings(void)
{
    uart_line_t line;

    printf("CR, LF and CR LF each end one line\n");
    boot_driver();

    type_text("A\r");
    CHECK_NEXT_LINE("A", UART_LINE_OK);

    type_text("B\n");
    CHECK_NEXT_LINE("B", UART_LINE_OK);

    type_text("C\r\n");
    CHECK_NEXT_LINE("C", UART_LINE_OK);
    CHECK(!uart_driver_line_ready());       /* the LF did not add an empty line */

    type_text("\r\n\r\n");                  /* two empty lines, not four */
    CHECK_NEXT_LINE("", UART_LINE_OK);
    CHECK_NEXT_LINE("", UART_LINE_OK);
    CHECK(!uart_driver_line_ready());

    type_text("\n\r");                      /* LF CR is two endings */
    CHECK_NEXT_LINE("", UART_LINE_OK);
    CHECK_NEXT_LINE("", UART_LINE_OK);
    CHECK(!uart_driver_line_ready());

    type_text("D\r\r");                     /* so is CR CR */
    CHECK_NEXT_LINE("D", UART_LINE_OK);
    CHECK_NEXT_LINE("", UART_LINE_OK);
    CHECK(!uart_driver_line_ready());

    type_text("E\r");
    type_text("\n");                        /* LF of a CR LF pair, in an interrupt of its own */
    CHECK_NEXT_LINE("E", UART_LINE_OK);
    CHECK(!uart_driver_line_ready());

    type_text("F\n\n");                     /* LF LF: the second one is an empty line */
    CHECK_NEXT_LINE("F", UART_LINE_OK);
    CHECK_NEXT_LINE("", UART_LINE_OK);
    CHECK(!uart_driver_take_line(&line));

    check_model_clean();
}

static void test_burst_matches_typed(void)
{
    uart_line_t line;

    printf("a burst in one interrupt gives the same lines\n");
    boot_driver();

    burst_text("HELP\r\nSTATUS\nLED_SET 1 1\rREAD_TEMP\r\n");

    CHECK_NEXT_LINE("HELP", UART_LINE_OK);
    CHECK_NEXT_LINE("STATUS", UART_LINE_OK);
    CHECK_NEXT_LINE("LED_SET 1 1", UART_LINE_OK);
    CHECK_NEXT_LINE("READ_TEMP", UART_LINE_OK);
    CHECK(!uart_driver_take_line(&line));

    check_model_clean();
}

static void test_backspace_and_delete(void)
{
    printf("backspace and DEL edit the line\n");
    boot_driver();

    type_text("LEDX\b_SET 2 1\r");
    type_text("AB\x7f\x7f" "CD\r");
    type_text("\b\b\x7fOK\r");          /* nothing to delete: harmless */
    type_text("X\b\r");                 /* edited down to an empty line */

    CHECK_NEXT_LINE("LED_SET 2 1", UART_LINE_OK);
    CHECK_NEXT_LINE("CD", UART_LINE_OK);
    CHECK_NEXT_LINE("OK", UART_LINE_OK);
    CHECK_NEXT_LINE("", UART_LINE_OK);

    check_model_clean();
}

static void test_line_too_long(void)
{
    char            expected[UART_LINE_MAX_LEN + 1U];
    uart_rx_stats_t stats;

    printf("overlong lines are cut and flagged, never overrun the buffer\n");
    boot_driver();

    /* Exactly at the limit is fine. */
    type_repeated('a', UART_LINE_MAX_LEN);
    type_text("\r");
    memset(expected, 'a', UART_LINE_MAX_LEN);
    expected[UART_LINE_MAX_LEN] = '\0';
    CHECK_NEXT_LINE(expected, UART_LINE_OK);

    /* One more is too long; the stored text is the first UART_LINE_MAX_LEN characters. */
    type_repeated('b', UART_LINE_MAX_LEN + 1U);
    type_text("\r");
    memset(expected, 'b', UART_LINE_MAX_LEN);
    CHECK_NEXT_LINE(expected, UART_LINE_TOO_LONG);

    /* Way too long, then backspaced below the limit: still discarded. */
    type_repeated('c', UART_LINE_MAX_LEN + 50U);
    type_repeated('\b', 60U);
    type_text("\r");
    memset(expected, 'c', UART_LINE_MAX_LEN);
    CHECK_NEXT_LINE(expected, UART_LINE_TOO_LONG);

    /* The next line is unaffected. */
    type_text("NEXT\r");
    CHECK_NEXT_LINE("NEXT", UART_LINE_OK);

    uart_driver_get_rx_stats(&stats);
    CHECK(stats.lines_too_long == 2U);
    CHECK(stats.lines_received == 4U);

    check_model_clean();
}

static void fill_queue(void)
{
    char     text[LINE_TEXT_LEN];
    uint32_t idx;

    for (idx = 0U; idx < UART_LINE_QUEUE_DEPTH; idx++)
    {
        (void)snprintf(text, sizeof(text), "L%u\r", (unsigned)idx);
        type_text(text);
    }
}

static void check_queue_contents(uint32_t first, uint32_t count)
{
    char     text[LINE_TEXT_LEN];
    uint32_t idx;

    for (idx = first; idx < (first + count); idx++)
    {
        (void)snprintf(text, sizeof(text), "L%u", (unsigned)idx);
        CHECK_NEXT_LINE(text, UART_LINE_OK);
    }
}

static void test_queue_full_drops_whole_lines(void)
{
    uart_rx_stats_t stats;
    uart_line_t     line;

    printf("full queue drops whole lines and counts them\n");
    boot_driver();

    fill_queue();
    type_text("L8\r");
    type_text("L9 with more text\r");

    uart_driver_get_rx_stats(&stats);
    CHECK(stats.lines_received == UART_LINE_QUEUE_DEPTH);
    CHECK(stats.lines_dropped == 2U);

    check_queue_contents(0U, UART_LINE_QUEUE_DEPTH);
    CHECK(!uart_driver_take_line(&line));

    type_text("AFTER\r");
    CHECK_NEXT_LINE("AFTER", UART_LINE_OK);

    check_model_clean();
}

static void test_line_started_while_full_stays_dropped(void)
{
    uart_rx_stats_t stats;
    uart_line_t     line;

    printf("a line that starts while the queue is full is never delivered in part\n");
    boot_driver();

    fill_queue();
    type_text("PART");              /* starts with no room */
    CHECK_NEXT_LINE("L0", UART_LINE_OK);    /* room appears mid-line */
    type_text("IAL\r");

    uart_driver_get_rx_stats(&stats);
    CHECK(stats.lines_dropped == 1U);

    check_queue_contents(1U, UART_LINE_QUEUE_DEPTH - 1U);
    CHECK(!uart_driver_take_line(&line));

    type_text("WHOLE\r");
    CHECK_NEXT_LINE("WHOLE", UART_LINE_OK);

    check_model_clean();
}

static void test_rx_errors_mark_the_line(void)
{
    char            expected[UART_LINE_MAX_LEN + 1U];
    char            overflow_burst[MOCK_UART_RX_FIFO_DEPTH + 6U];
    uart_rx_stats_t stats;

    printf("framing, parity and overrun errors flag the line they hit\n");
    boot_driver();

    /* Framing error on a byte in the middle of a line. */
    type_text("LED_S");
    mock_uart_raise_status(XUARTPS_IXR_FRAMING);
    type_text("E");
    type_text("T 1 1\r");
    CHECK_NEXT_LINE("LED_SET 1 1", UART_LINE_RX_ERROR);

    type_text("STOP\r");
    CHECK_NEXT_LINE("STOP", UART_LINE_OK);

    /* Parity error on the first byte of a line. */
    mock_uart_raise_status(XUARTPS_IXR_PARITY);
    type_text("X");
    type_text("YZ\r");
    CHECK_NEXT_LINE("XYZ", UART_LINE_RX_ERROR);

    /* The UART's own FIFO overflows while the ISR is held off: 70 bytes arrive, 6 are lost. */
    memset(overflow_burst, 'x', sizeof(overflow_burst));
    CHECK(!mock_uart_receive(overflow_burst, sizeof(overflow_burst)));
    CHECK(mock_uart_rx_fifo_count() == MOCK_UART_RX_FIFO_DEPTH);
    (void)mock_uart_service_irq();
    type_text("\r");
    memset(expected, 'x', MOCK_UART_RX_FIFO_DEPTH);
    expected[MOCK_UART_RX_FIFO_DEPTH] = '\0';
    CHECK_NEXT_LINE(expected, UART_LINE_RX_ERROR);

    /* An error outranks "too long". */
    type_repeated('t', UART_LINE_MAX_LEN + 5U);
    mock_uart_raise_status(XUARTPS_IXR_FRAMING);
    type_text("t\r");
    memset(expected, 't', UART_LINE_MAX_LEN);
    expected[UART_LINE_MAX_LEN] = '\0';
    CHECK_NEXT_LINE(expected, UART_LINE_RX_ERROR);

    uart_driver_get_rx_stats(&stats);
    CHECK(stats.framing_errors == 2U);
    CHECK(stats.parity_errors == 1U);
    CHECK(stats.overrun_errors == 1U);

    check_model_clean();
}

static void test_queue_indices_wrap(void)
{
    char            text[LINE_TEXT_LEN];
    uart_rx_stats_t stats;
    uart_line_t     line;
    uint32_t        idx;
    uint32_t        next_expected = 0U;
    bool            order_ok      = true;

    printf("queue slots are reused in order over many lines\n");
    boot_driver();

    for (idx = 0U; idx < WRAP_TEST_LINES; idx++)
    {
        (void)snprintf(text, sizeof(text), "CMD %u\r", (unsigned)idx);
        type_text(text);

        /* Let a varying backlog build up (never more than the depth) before draining it. */
        if ((idx % 5U) == 4U)
        {
            while (uart_driver_take_line(&line))
            {
                (void)snprintf(text, sizeof(text), "CMD %u", (unsigned)next_expected);
                order_ok = order_ok && (strcmp(line.text, text) == 0) && (line.status == UART_LINE_OK);
                next_expected++;
            }
        }
    }

    CHECK(order_ok);
    CHECK(next_expected == WRAP_TEST_LINES);

    uart_driver_get_rx_stats(&stats);
    CHECK(stats.lines_received == WRAP_TEST_LINES);
    CHECK(stats.lines_dropped == 0U);

    check_model_clean();
}

static void test_ack_before_drain(void)
{
    unsigned isr_calls;

    printf("a byte arriving right after the ISR's last look is not stranded\n");
    boot_driver();

    type_text("HELP");

    /* The line ending lands just after the ISR has seen the FIFO empty. */
    mock_uart_arm_late_byte('\r');
    CHECK(mock_uart_receive("!", 1U));
    isr_calls = mock_uart_service_irq();

    CHECK(isr_calls == 2U);          /* the late byte raised a second interrupt */
    CHECK(uart_driver_line_ready());
    CHECK_NEXT_LINE("HELP!", UART_LINE_OK);

    check_model_clean();
}

static void test_take_line_arguments(void)
{
    uart_line_t line;

    printf("take_line argument handling\n");
    boot_driver();

    CHECK(!uart_driver_take_line(NULL));
    CHECK(!uart_driver_take_line(&line));

    type_text("KEEP\r");
    CHECK(!uart_driver_take_line(NULL));    /* doesn't consume the line */
    CHECK(uart_driver_line_ready());
    CHECK_NEXT_LINE("KEEP", UART_LINE_OK);

    uart_driver_get_rx_stats(NULL);         /* must not crash */

    check_model_clean();
}

static void test_transmit(void)
{
    printf("transmit translates LF to CR LF\n");
    boot_driver();

    uart_driver_write("a\nb", 3U);
    CHECK(strcmp(mock_uart_tx_text(), "a\r\nb") == 0);

    mock_uart_tx_clear();
    uart_driver_write("hello", 2U);
    CHECK(strcmp(mock_uart_tx_text(), "he") == 0);

    mock_uart_tx_clear();
    uart_driver_puts("x\n\n");
    uart_driver_puts(NULL);
    uart_driver_write(NULL, 5U);
    CHECK(strcmp(mock_uart_tx_text(), "x\r\n\r\n") == 0);

    /* Transmitting doesn't disturb reception. */
    type_text("RX\r");
    uart_driver_puts("TX\n");
    CHECK_NEXT_LINE("RX", UART_LINE_OK);

    check_model_clean();
}

int main(void)
{
    test_init_configures_uart();
    test_init_failures();
    test_single_line();
    test_line_endings();
    test_burst_matches_typed();
    test_backspace_and_delete();
    test_line_too_long();
    test_queue_full_drops_whole_lines();
    test_line_started_while_full_stays_dropped();
    test_rx_errors_mark_the_line();
    test_queue_indices_wrap();
    test_ack_before_drain();
    test_take_line_arguments();
    test_transmit();

    printf("\n%u checks, %u failed\n", s_checks_run, s_checks_failed);
    return (s_checks_failed == 0U) ? 0 : 1;
}
