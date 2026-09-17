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

static void tx_drain(void);

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
    tx_drain();
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

/* Plays the wire until the driver has nothing left to send. */
static void tx_drain(void)
{
    unsigned guard = 0U;

    while ((mock_uart_tx_fifo_count() > 0U) && (guard < 10000U))
    {
        (void)mock_uart_tx_shift(MOCK_UART_TX_FIFO_DEPTH);
        (void)mock_uart_service_irq();
        guard++;
    }

    CHECK(guard < 10000U);
}

static void test_transmit(void)
{
    printf("transmit translates LF to CR LF\n");
    boot_driver();

    uart_driver_write("a\nb", 3U);
    tx_drain();
    CHECK(strcmp(mock_uart_tx_text(), "a\r\nb") == 0);

    mock_uart_tx_clear();
    uart_driver_write("hello", 2U);
    tx_drain();
    CHECK(strcmp(mock_uart_tx_text(), "he") == 0);

    mock_uart_tx_clear();
    uart_driver_puts("x\n\n");
    uart_driver_puts(NULL);
    uart_driver_write(NULL, 5U);
    tx_drain();
    CHECK(strcmp(mock_uart_tx_text(), "x\r\n\r\n") == 0);

    /* Transmitting doesn't disturb reception. */
    type_text("RX\r");
    uart_driver_puts("TX\n");
    tx_drain();
    CHECK_NEXT_LINE("RX", UART_LINE_OK);

    check_model_clean();
}

/*
 * The point of stage 3: the write hands off and returns. The first bytes are
 * primed into the FIFO, the rest follows on TX-empty interrupts, and the
 * interrupt is switched off again once the ring runs dry.
 */
static void test_transmit_is_interrupt_driven(void)
{
    char            message[200];
    uart_tx_stats_t stats;
    unsigned        isr_calls = 0U;
    size_t          idx;

    printf("long output is handed off and finished by the TX interrupt\n");
    boot_driver();

    for (idx = 0U; idx < (sizeof(message) - 1U); idx++)
    {
        message[idx] = (char)('A' + (int)(idx % 26U));
    }
    message[sizeof(message) - 1U] = '\0';

    uart_driver_write(message, sizeof(message) - 1U);

    /* Primed: the FIFO is full and the interrupt is armed for the remainder. */
    CHECK(mock_uart_tx_fifo_count() == MOCK_UART_TX_FIFO_DEPTH);
    CHECK((mock_uart.imr & XUARTPS_IXR_TXEMPTY) != 0U);

    while (mock_uart_tx_fifo_count() > 0U)
    {
        (void)mock_uart_tx_shift(MOCK_UART_TX_FIFO_DEPTH);
        isr_calls += mock_uart_service_irq();
    }

    CHECK(strcmp(mock_uart_tx_text(), message) == 0);
    CHECK(isr_calls >= 3U);      /* 200 bytes through a 64-byte FIFO */

    /* Nothing left to send, so the transmitter stops asking. */
    CHECK((mock_uart.imr & XUARTPS_IXR_TXEMPTY) == 0U);
    CHECK((mock_uart.imr & XUARTPS_IXR_RXOVR) != 0U);   /* receive stays armed */

    uart_driver_get_tx_stats(&stats);
    CHECK(stats.bytes_sent == (sizeof(message) - 1U));
    CHECK(stats.ring_high_water == (sizeof(message) - 1U));
    CHECK(stats.stalls == 0U);

    check_model_clean();
}

/*
 * More output than the ring holds. The write waits, but only as long as the
 * wire needs, and not one byte is lost or reordered.
 */
static void test_transmit_ring_full(void)
{
    static char     message[UART_TX_RING_SIZE + 500U];
    uart_tx_stats_t stats;
    size_t          idx;

    printf("output larger than the ring waits for the wire, and loses nothing\n");
    boot_driver();

    /* A wire that takes every byte as soon as the transmitter offers it. */
    mock_uart_set_tx_autoshift(true);

    for (idx = 0U; idx < (sizeof(message) - 1U); idx++)
    {
        message[idx] = (char)('0' + (int)(idx % 10U));
    }
    message[sizeof(message) - 1U] = '\0';

    uart_driver_write(message, sizeof(message) - 1U);
    tx_drain();

    CHECK(strcmp(mock_uart_tx_text(), message) == 0);

    uart_driver_get_tx_stats(&stats);
    CHECK(stats.bytes_sent == (sizeof(message) - 1U));
    CHECK(stats.stalls >= 1U);                          /* it did have to wait */
    CHECK(stats.ring_high_water <= UART_TX_RING_SIZE);  /* and never past the ring */

    CHECK(mock_uart.tx_fifo_overflows == 0U);
    mock_uart_set_tx_autoshift(false);
    check_model_clean();
}

static void test_flush(void)
{
    printf("flush waits for the ring, the FIFO and the shift register\n");
    boot_driver();

    uart_driver_flush();                    /* nothing queued: returns at once */
    CHECK(strcmp(mock_uart_tx_text(), "") == 0);

    mock_uart_set_tx_autoshift(true);
    uart_driver_puts("last words\n");
    uart_driver_flush();

    CHECK(strcmp(mock_uart_tx_text(), "last words\r\n") == 0);
    CHECK(mock_uart_tx_fifo_count() == 0U);
    CHECK((mock_uart.imr & XUARTPS_IXR_TXEMPTY) == 0U);

    mock_uart_set_tx_autoshift(false);
    check_model_clean();
}

/* Receiving while the transmitter is busy: both halves keep their own state. */
static void test_transmit_and_receive_together(void)
{
    uart_rx_stats_t rx_stats;

    printf("transmit and receive interleave without disturbing each other\n");
    boot_driver();

    uart_driver_puts("0123456789012345678901234567890123456789012345678901234567890123456789\n");

    type_text("HELP\r");                    /* arrives while the FIFO is still full */
    (void)mock_uart_tx_shift(10U);
    type_text("STATUS\r");
    tx_drain();

    CHECK_NEXT_LINE("HELP", UART_LINE_OK);
    CHECK_NEXT_LINE("STATUS", UART_LINE_OK);

    uart_driver_get_rx_stats(&rx_stats);
    CHECK(rx_stats.lines_received == 2U);
    CHECK(rx_stats.lines_dropped == 0U);
    CHECK(strstr(mock_uart_tx_text(), "0123456789") != NULL);

    check_model_clean();
}

/*
 * With no interrupt system, the transmitter has no one to refill it - except
 * the write itself, which keeps feeding the FIFO from inside its wait. Output
 * then goes out by polling, slowly but completely. That is what lets a fatal
 * message be printed when the interrupt setup is what failed.
 */
static void test_transmit_without_interrupts(void)
{
    char   message[300];
    size_t idx;

    printf("output still goes out when the interrupt system failed\n");

    mock_uart_reset();
    mock_uart.fail_irq_setup = true;
    CHECK(uart_driver_init(TEST_BAUD) == UART_DRIVER_ERR_IRQ);
    mock_uart_tx_clear();
    mock_uart_set_tx_autoshift(true);       /* the wire still works */

    for (idx = 0U; idx < (sizeof(message) - 1U); idx++)
    {
        message[idx] = (char)('a' + (int)(idx % 26U));
    }
    message[sizeof(message) - 1U] = '\0';

    uart_driver_write(message, sizeof(message) - 1U);
    uart_driver_flush();

    CHECK(strcmp(mock_uart_tx_text(), message) == 0);
    CHECK(mock_uart_service_irq() == 0U);   /* no handler was ever connected */

    mock_uart_set_tx_autoshift(false);
    check_model_clean();
}

/*
 * The transmit path belongs to whoever has TX-empty unmasked. While the main
 * loop primes an idle transmitter it masks that source, so an interrupt
 * arriving in the middle - here, because the wire empties the FIFO on the
 * spot - must not start draining the ring as well. If it does, the ring tail
 * moves under the main loop and bytes come out twice or not at all.
 */
static void test_interrupt_during_prime(void)
{
    static char message[300];
    size_t      idx;

    printf("an interrupt during priming does not touch the transmit path\n");
    boot_driver();

    for (idx = 0U; idx < (sizeof(message) - 1U); idx++)
    {
        message[idx] = (char)('A' + (int)(idx % 26U));
    }
    message[sizeof(message) - 1U] = '\0';

    /* A wire that takes every byte at once, and a GIC that fires immediately. */
    mock_uart_set_tx_autoshift(true);
    mock_uart_set_autoservice(true);

    uart_driver_write(message, sizeof(message) - 1U);
    uart_driver_flush();

    CHECK(strcmp(mock_uart_tx_text(), message) == 0);

    mock_uart_set_autoservice(false);
    mock_uart_set_tx_autoshift(false);
    check_model_clean();
}

/*
 * The ISR handles - and acknowledges - only the sources that are unmasked.
 *
 * That is what keeps the transmit path safe: while the main loop primes an
 * idle transmitter it masks TX-empty, and an interrupt that arrives in that
 * window (for a received character, say) must leave the transmit side alone,
 * status bit included. A model cannot reproduce an interrupt landing between
 * two instructions, but it can prove the rule the ISR follows: a masked
 * source is still pending when the handler returns.
 */
static void test_isr_only_touches_enabled_sources(void)
{
    printf("the ISR leaves masked sources alone, status bit included\n");
    boot_driver();

    /* TX-empty is masked while nothing is being sent. */
    CHECK((mock_uart.imr & XUARTPS_IXR_TXEMPTY) == 0U);

    /* The transmitter reports itself empty, and a character arrives. */
    mock_uart_raise_status(XUARTPS_IXR_TXEMPTY);
    CHECK(mock_uart_receive("Z", 1U));
    CHECK(mock_uart_service_irq() == 1U);

    /* The receive side was served ... */
    CHECK(mock_uart_rx_fifo_count() == 0U);
    /* ... and the masked transmit status was not consumed by it. */
    CHECK((mock_uart_isr_bits() & XUARTPS_IXR_TXEMPTY) != 0U);

    type_text("\r");
    CHECK_NEXT_LINE("Z", UART_LINE_OK);

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
    test_transmit_is_interrupt_driven();
    test_transmit_ring_full();
    test_flush();
    test_transmit_and_receive_together();
    test_transmit_without_interrupts();
    test_interrupt_during_prime();
    test_isr_only_touches_enabled_sources();

    printf("\n%u checks, %u failed\n", s_checks_run, s_checks_failed);
    return (s_checks_failed == 0U) ? 0 : 1;
}
