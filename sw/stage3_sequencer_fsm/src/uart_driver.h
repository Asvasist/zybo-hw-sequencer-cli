/*
 * uart_driver.h
 *
 * Interrupt-driven console UART for the sequencer CLI (Zynq PS UART, XUartPs).
 *
 * Receive side - no polling anywhere:
 *
 *   The UART raises an interrupt for every received character. The ISR drops
 *   the character into the line currently being assembled, and when a CR or
 *   LF arrives it commits that line to a small queue of complete lines. The
 *   main loop takes whole lines out of the queue with uart_driver_take_line()
 *   and does all the parsing; the ISR never looks at what a line says.
 *
 *   ISR -> main loop hand-off is lock-free, with exactly one writer per shared
 *   variable:
 *     - lines committed : written by the ISR only  ("a command is ready" flag)
 *     - lines taken     : written by the main loop only
 *   A line is ready while the two differ. Neither side ever has to mask
 *   interrupts, and a burst of lines (a pasted script) can't merge into one
 *   event the way a single boolean flag would.
 *
 * Line discipline, applied in the ISR:
 *   - CR, LF and CR LF each end exactly one line. An empty line is delivered
 *     too, so the application can simply print a fresh prompt.
 *   - Backspace (0x08) and DEL (0x7F) remove the last character, if any.
 *   - More than UART_LINE_MAX_LEN characters: the extra ones are thrown away
 *     and the line is delivered with status UART_LINE_TOO_LONG.
 *   - A framing, parity or FIFO overrun error while a line is arriving: the
 *     line is delivered with status UART_LINE_RX_ERROR.
 *   - Queue full when a line starts: the whole line is dropped and counted in
 *     uart_rx_stats_t.lines_dropped. A line is only ever delivered complete,
 *     never with a missing start.
 *
 * Transmit side, from stage 3, is interrupt driven as well:
 *
 *   uart_driver_write() copies into a ring buffer and returns. If the
 *   transmitter is idle it primes the TX FIFO on the spot, so the first
 *   bytes go out immediately; from then on the TX-empty interrupt refills the
 *   FIFO from the ring until it runs dry. The main loop only waits if it
 *   produces more than UART_TX_RING_SIZE bytes of backlog, which is counted.
 *
 *   Ownership of the transmit path is decided by the TX interrupt mask: the
 *   ISR touches the ring tail and the FIFO only while TX-empty is unmasked,
 *   and the main loop masks it for the few instructions it needs to prime an
 *   idle transmitter. No other locking is involved.
 */
#ifndef UART_DRIVER_H
#define UART_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Longest command line accepted, not counting the line ending. */
#define UART_LINE_MAX_LEN           80U

/* Complete lines the ISR can hold before the main loop takes one. Power of two. */
#define UART_LINE_QUEUE_DEPTH       8U

/*
 * Bytes of pending transmit output. Power of two. One HELP listing is about
 * 600 characters, so this holds the longest response several times over and
 * the main loop hands it off without waiting.
 */
#define UART_TX_RING_SIZE           1024U

typedef enum
{
    UART_DRIVER_OK = 0,
    UART_DRIVER_ERR_LOOKUP,     /* no config entry for the console UART in xparameters.h */
    UART_DRIVER_ERR_INIT,       /* XUartPs_CfgInitialize() rejected the config */
    UART_DRIVER_ERR_FORMAT,     /* baud rate not reachable from the UART ref clock */
    UART_DRIVER_ERR_IRQ         /* interrupt controller setup failed - TX still works */
} uart_driver_status_t;

/* Ordered by severity: if several problems hit one line, the highest one is reported. */
typedef enum
{
    UART_LINE_OK = 0,
    UART_LINE_TOO_LONG,         /* text holds the first UART_LINE_MAX_LEN characters */
    UART_LINE_RX_ERROR          /* framing, parity or overrun error during the line */
} uart_line_status_t;

typedef struct
{
    char               text[UART_LINE_MAX_LEN + 1U];   /* always NUL-terminated */
    uint16_t           length;                         /* characters in text, without the NUL */
    uart_line_status_t status;
} uart_line_t;

/*
 * Receive counters since init, all written by the ISR only. Each field is a
 * single 32-bit value and is read atomically, but a copy of the whole struct
 * is not a snapshot of one instant - fine for diagnostics.
 */
typedef struct
{
    uint32_t bytes_received;    /* everything read from the RX FIFO, line endings included */
    uint32_t lines_received;    /* committed to the queue, whatever their status */
    uint32_t lines_dropped;     /* queue was full - never seen by the main loop */
    uint32_t lines_too_long;
    uint32_t overrun_errors;    /* RX FIFO overflowed inside the UART: bytes were lost */
    uint32_t framing_errors;
    uint32_t parity_errors;
} uart_rx_stats_t;

/*
 * Transmit counters since init. bytes_sent is written by whoever moves a byte
 * into the FIFO (the ISR, or the main loop when it primes an idle
 * transmitter); the others by the main loop.
 */
typedef struct
{
    uint32_t bytes_sent;
    uint32_t ring_high_water;   /* most bytes ever waiting in the ring      */
    uint32_t stalls;            /* writes that had to wait for ring space   */
} uart_tx_stats_t;

/*
 * Brings up the console UART at 8N1 and `baud_rate`, empties the receive
 * queue, hooks the ISR into the GIC and enables the receive interrupts.
 * Anything already sitting in the RX FIFO (keys pressed during boot) is
 * discarded. On UART_DRIVER_ERR_IRQ the transmit functions still work, so the
 * caller can report the failure.
 */
uart_driver_status_t uart_driver_init(uint32_t baud_rate);

/* True while at least one complete line is waiting. Cheap: two integer reads. */
bool uart_driver_line_ready(void);

/*
 * Copies the oldest complete line into *line_out and frees its queue slot.
 * Returns false, leaving *line_out untouched, if no line is waiting.
 * Main loop only - never call it from an interrupt handler.
 */
bool uart_driver_take_line(uart_line_t *line_out);

/*
 * Queues `length` bytes for transmission and returns without waiting, unless
 * the ring is full. Each '\n' goes out as "\r\n".
 */
void uart_driver_write(const char *data, size_t length);

/* uart_driver_write() for a NUL-terminated string. */
void uart_driver_puts(const char *text);

/*
 * Waits until everything queued has left the transmitter, shift register
 * included. For the last message before a reset, and for tests.
 */
void uart_driver_flush(void);

void uart_driver_get_rx_stats(uart_rx_stats_t *stats_out);
void uart_driver_get_tx_stats(uart_tx_stats_t *stats_out);

#endif /* UART_DRIVER_H */
