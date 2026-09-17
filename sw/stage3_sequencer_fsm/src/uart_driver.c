/*
 * uart_driver.c
 *
 * Interrupt-driven console UART on top of the Xilinx XUartPs low-level driver.
 * See uart_driver.h for the usage model and the line discipline.
 * Reference: UG585 chapter 19 (UART controller), chapter 7 (interrupts).
 */
#include "uart_driver.h"

#include <string.h>

#include "xstatus.h"
#include "xuartps.h"
#include "xinterrupt_wrap.h"

#include "board_zybo.h"

/*
 * Written for the SDT BSP flow (Vitis 2023.2 and later, the only flow in
 * 2025.2): XUartPs_LookupConfig() takes a base address and the config table
 * carries the encoded interrupt ID and parent that XSetupInterruptSystem()
 * needs. The classic flow looks both up differently.
 */
#ifndef SDT
#error "uart_driver.c targets the SDT BSP flow (Vitis 2023.2 or later); SDT is not defined"
#endif

/*
 * Interrupt on every received character. At 115200 baud that's at most
 * ~11.5 kHz, trivial for the A9, and a line ending is seen the moment its stop
 * bit arrives - no waiting for a FIFO watermark or the receive timeout. That
 * matters once stage 4 timestamps "command fully received". With a trigger
 * level of 1 the receive timeout adds nothing, so it's switched off.
 */
#define UART_RX_FIFO_TRIGGER_LEVEL  1U
#define UART_RX_TIMEOUT_DISABLED    0U

#define UART_RX_ERROR_IRQS          (XUARTPS_IXR_OVER | XUARTPS_IXR_FRAMING | XUARTPS_IXR_PARITY)
#define UART_RX_IRQS                (XUARTPS_IXR_RXOVR | UART_RX_ERROR_IRQS)

#define UART_LINE_QUEUE_MASK        (UART_LINE_QUEUE_DEPTH - 1U)
#define UART_TX_RING_MASK           (UART_TX_RING_SIZE - 1U)

/* The transmitter asks for more when its FIFO runs empty. */
#define UART_TX_IRQS                (XUARTPS_IXR_TXEMPTY)

#define ASCII_BACKSPACE             0x08U
#define ASCII_LF                    0x0AU
#define ASCII_CR                    0x0DU
#define ASCII_DEL                   0x7FU

_Static_assert((UART_LINE_QUEUE_DEPTH > 0U) && ((UART_LINE_QUEUE_DEPTH & UART_LINE_QUEUE_MASK) == 0U),
               "UART_LINE_QUEUE_DEPTH must be a power of two");
_Static_assert(UART_LINE_MAX_LEN <= UINT16_MAX, "uart_line_t.length is 16 bits");
_Static_assert((UART_TX_RING_SIZE > 0U) && ((UART_TX_RING_SIZE & UART_TX_RING_MASK) == 0U),
               "UART_TX_RING_SIZE must be a power of two");

/* ---------------------------------------------------------------------------
 * State shared between the ISR and the main loop. volatile because each side
 * changes something the other one reads, invisibly to the compiler.
 *
 * Ownership of the queue slots:
 *   slots [taken, committed)  complete lines, owned by the main loop
 *   slot  committed           the line the ISR is assembling, if there is room
 * The ISR only writes slot `committed`, and only while committed - taken is
 * below the queue depth, so it can never touch a slot the main loop owns.
 * Both counters run freely and wrap at 2^32; their difference stays correct.
 * ------------------------------------------------------------------------- */
static volatile uart_line_t     s_line_queue[UART_LINE_QUEUE_DEPTH];
static volatile uint32_t        s_lines_committed;  /* ISR writes, main reads */
static volatile uint32_t        s_lines_taken;      /* main writes, ISR reads */
static volatile uart_rx_stats_t s_rx_stats;         /* ISR writes, main reads */

/* ---------------------------------------------------------------------------
 * Transmit ring. The main loop appends at the head, and whoever owns the
 * transmit path moves bytes from the tail into the FIFO - the ISR while
 * TX-empty is unmasked, the main loop while it has masked it to prime an idle
 * transmitter. Free-running counters again: fill is head - tail.
 * ------------------------------------------------------------------------- */
static volatile uint8_t         s_tx_ring[UART_TX_RING_SIZE];
static volatile uint32_t        s_tx_head;          /* main loop writes */
static volatile uint32_t        s_tx_tail;          /* transmit path owner writes */
static volatile uart_tx_stats_t s_tx_stats;

/* ---------------------------------------------------------------------------
 * Line assembly state. Only the ISR touches these once interrupts are on
 * (init writes them before that), so they need no qualifier.
 * ------------------------------------------------------------------------- */
static bool               s_rx_line_open;       /* a line has started and not ended yet */
static bool               s_rx_line_has_slot;   /* ... and it is being stored (queue had room) */
static uint16_t           s_rx_line_len;
static uart_line_status_t s_rx_line_status;
static bool               s_rx_prev_was_cr;     /* to fold CR LF into one line ending */
static bool               s_rx_error_pending;   /* line error seen, mark the line it hit */

static XUartPs s_uart;
static u32     s_uart_base;
static bool    s_tx_ready;

static void rx_raise_line_status(uart_line_status_t status)
{
    if (status > s_rx_line_status)
    {
        s_rx_line_status = status;
    }
}

/*
 * A line starts with its first byte. Whether it gets a queue slot is decided
 * here, once: only the ISR adds lines, so the free room can only grow until
 * this line ends. A line that starts without room stays dropped even if the
 * main loop frees a slot halfway through - its beginning is already gone.
 */
static void rx_open_line(void)
{
    s_rx_line_open     = true;
    s_rx_line_has_slot = ((s_lines_committed - s_lines_taken) < UART_LINE_QUEUE_DEPTH);
    s_rx_line_len      = 0U;
    s_rx_line_status   = UART_LINE_OK;
}

static void rx_close_line(void)
{
    if (s_rx_line_has_slot)
    {
        volatile uart_line_t *const slot = &s_line_queue[s_lines_committed & UART_LINE_QUEUE_MASK];

        slot->text[s_rx_line_len] = '\0';
        slot->length              = s_rx_line_len;
        slot->status              = s_rx_line_status;

        if (s_rx_line_status == UART_LINE_TOO_LONG)
        {
            s_rx_stats.lines_too_long++;
        }
        s_rx_stats.lines_received++;

        /* The hand-off. From this increment on the slot belongs to the main loop. */
        s_lines_committed++;
    }
    else
    {
        s_rx_stats.lines_dropped++;
    }

    s_rx_line_open = false;
}

/* ISR context: everything that happens to one received byte. O(1), no loops. */
static void rx_process_byte(uint8_t byte)
{
    const bool is_line_end = (byte == ASCII_CR) || (byte == ASCII_LF);

    s_rx_stats.bytes_received++;

    /* The LF of a CR LF pair belongs to a line that has already ended. */
    if ((byte == ASCII_LF) && s_rx_prev_was_cr)
    {
        s_rx_prev_was_cr = false;
        return;
    }
    s_rx_prev_was_cr = (byte == ASCII_CR);

    if (!s_rx_line_open)
    {
        rx_open_line();
    }

    if (s_rx_error_pending)
    {
        rx_raise_line_status(UART_LINE_RX_ERROR);
        s_rx_error_pending = false;
    }

    if (is_line_end)
    {
        rx_close_line();
        return;
    }

    if (!s_rx_line_has_slot)
    {
        return;     /* line is being dropped, nothing to keep */
    }

    if ((byte == ASCII_BACKSPACE) || (byte == ASCII_DEL))
    {
        /* An oversize or damaged line is discarded anyway; editing can't rescue it. */
        if ((s_rx_line_status == UART_LINE_OK) && (s_rx_line_len > 0U))
        {
            s_rx_line_len--;
        }
        return;
    }

    if (s_rx_line_len < UART_LINE_MAX_LEN)
    {
        s_line_queue[s_lines_committed & UART_LINE_QUEUE_MASK].text[s_rx_line_len] = (char)byte;
        s_rx_line_len++;
    }
    else
    {
        rx_raise_line_status(UART_LINE_TOO_LONG);
    }
}

/*
 * Moves bytes from the ring into the TX FIFO until one of them runs out.
 * The caller must own the transmit path: either it is the ISR with TX-empty
 * unmasked, or the main loop with TX-empty masked.
 */
static void uart_tx_fill_fifo(void)
{
    while ((s_tx_head != s_tx_tail) &&
           ((XUartPs_ReadReg(s_uart_base, XUARTPS_SR_OFFSET) & XUARTPS_SR_TXFULL) == 0U))
    {
        XUartPs_WriteReg(s_uart_base, XUARTPS_FIFO_OFFSET, s_tx_ring[s_tx_tail & UART_TX_RING_MASK]);
        s_tx_tail++;
        s_tx_stats.bytes_sent++;
    }
}

/*
 * Gets an idle transmitter going, and keeps the TX interrupt on only while
 * bytes are still waiting.
 *
 * The TX-empty status bit is an event, not a level: it is raised when the FIFO
 * *becomes* empty. Enabling the interrupt on an already-empty FIFO would
 * therefore never fire, so the first bytes have to be written here. Masking
 * TX-empty first takes the transmit path away from the ISR for the few
 * instructions this needs; the mask goes back on at the end if there is more
 * to send, and any TX-empty that happened in between is still pending and
 * fires as soon as it is unmasked.
 */
static void uart_tx_kick(void)
{
    XUartPs_WriteReg(s_uart_base, XUARTPS_IDR_OFFSET, UART_TX_IRQS);

    uart_tx_fill_fifo();

    if (s_tx_head != s_tx_tail)
    {
        XUartPs_WriteReg(s_uart_base, XUARTPS_IER_OFFSET, UART_TX_IRQS);
    }
}

static uint32_t uart_tx_pending(void)
{
    return s_tx_head - s_tx_tail;
}

/*
 * The UART's IRQ handler, called by the GIC dispatcher.
 *
 * Order matters: the interrupt status is acknowledged *before* the FIFO is
 * drained. A character that arrives after the ack sets the trigger bit again,
 * so it is either read by the loop below or raises a fresh interrupt. Acking
 * after the loop would clear the trigger of a character that landed just
 * after the final "FIFO empty" check, and that character would sit unseen
 * until the next keypress - a line ending could be delayed indefinitely.
 *
 * The error bits are sticky events, not per-byte flags. With a trigger level
 * of 1 the FIFO holds the damaged byte itself when this runs, so the error is
 * charged to the line that byte belongs to.
 *
 * The drain loop can't spin: at 115200 baud a byte arrives every ~87 us, and
 * the loop removes one in a few register accesses.
 */
static void uart_driver_isr(void *callback_ref)
{
    const u32 enabled = XUartPs_ReadReg(s_uart_base, XUARTPS_IMR_OFFSET);
    const u32 pending = XUartPs_ReadReg(s_uart_base, XUARTPS_ISR_OFFSET) & enabled;

    (void)callback_ref;

    XUartPs_WriteReg(s_uart_base, XUARTPS_ISR_OFFSET, pending);

    if ((pending & UART_RX_ERROR_IRQS) != 0U)
    {
        if ((pending & XUARTPS_IXR_OVER) != 0U)
        {
            s_rx_stats.overrun_errors++;
        }
        if ((pending & XUARTPS_IXR_FRAMING) != 0U)
        {
            s_rx_stats.framing_errors++;
        }
        if ((pending & XUARTPS_IXR_PARITY) != 0U)
        {
            s_rx_stats.parity_errors++;
        }
        s_rx_error_pending = true;
    }

    while ((XUartPs_ReadReg(s_uart_base, XUARTPS_SR_OFFSET) & XUARTPS_SR_RXEMPTY) == 0U)
    {
        rx_process_byte((uint8_t)XUartPs_ReadReg(s_uart_base, XUARTPS_FIFO_OFFSET));
    }

    /*
     * The transmitter has run dry. Refill it, and drop the interrupt once
     * nothing is left: an empty FIFO with TX-empty still unmasked would
     * interrupt forever. Reaching here at all means TX-empty was unmasked,
     * so the main loop is not inside uart_tx_kick() and the ring tail is ours.
     */
    if ((pending & XUARTPS_IXR_TXEMPTY) != 0U)
    {
        uart_tx_fill_fifo();

        if (s_tx_head == s_tx_tail)
        {
            XUartPs_WriteReg(s_uart_base, XUARTPS_IDR_OFFSET, UART_TX_IRQS);
        }
    }
}

/* Both run with the UART's interrupt sources masked, before the ISR is enabled. */
static void tx_reset_state(void)
{
    const uart_tx_stats_t zero_stats = { 0U };

    s_tx_head  = 0U;
    s_tx_tail  = 0U;
    s_tx_stats = zero_stats;
}

static void rx_reset_state(void)
{
    const uart_rx_stats_t zero_stats = { 0U };

    s_lines_committed  = 0U;
    s_lines_taken      = 0U;
    s_rx_stats         = zero_stats;
    s_rx_line_open     = false;
    s_rx_line_has_slot = false;
    s_rx_line_len      = 0U;
    s_rx_line_status   = UART_LINE_OK;
    s_rx_prev_was_cr   = false;
    s_rx_error_pending = false;
}

/*
 * Initialization sequence:
 *   1. Look up UART1 and wait for the TX FIFO to drain (boot messages).
 *   2. XUartPs_CfgInitialize(): driver instance, all interrupt sources masked.
 *   3. 8N1 at the requested baud, normal mode. TX works from here.
 *   4. RX trigger level 1, receive timeout off.
 *   5. Reset the line queue; ack stale interrupt status, then empty the RX FIFO.
 *   6. XSetupInterruptSystem(): GIC init (first caller only), level trigger and
 *      default priority from the SDT interrupt ID, ISR connected, GIC line and
 *      CPU IRQs enabled. The UART still can't interrupt - its sources are masked.
 *   7. Unmask the receive interrupt sources in the UART.
 */
uart_driver_status_t uart_driver_init(uint32_t baud_rate)
{
    XUartPs_Config *uart_cfg;
    XUartPsFormat   line_format;

    s_tx_ready = false;

    uart_cfg = XUartPs_LookupConfig(BOARD_CONSOLE_UART_BASEADDR);
    if (uart_cfg == NULL)
    {
        return UART_DRIVER_ERR_LOOKUP;
    }

    /*
     * This UART is already running: FSBL or ps7_init enabled it for the BSP's
     * stdout. Changing the baud generator mid-character garbles the tail of
     * the boot output, so let the TX FIFO empty first. TXEMPTY resets to 1, so
     * this can't hang on a UART that was never enabled.
     */
    while ((XUartPs_ReadReg(uart_cfg->BaseAddress, XUARTPS_SR_OFFSET) & XUARTPS_SR_TXEMPTY) == 0U)
    {
        /* wait */
    }

    if (XUartPs_CfgInitialize(&s_uart, uart_cfg, uart_cfg->BaseAddress) != XST_SUCCESS)
    {
        return UART_DRIVER_ERR_INIT;
    }
    s_uart_base = uart_cfg->BaseAddress;

    line_format.BaudRate = baud_rate;
    line_format.DataBits = XUARTPS_FORMAT_8_BITS;
    line_format.Parity   = XUARTPS_FORMAT_NO_PARITY;
    line_format.StopBits = XUARTPS_FORMAT_1_STOP_BIT;

    /*
     * SetDataFormat() searches the CD/BDIV divisors and refuses the rate if the
     * best match is more than 3 % off. 115200 from the 100 MHz UART ref clock
     * comes out at CD=124, BDIV=6: 115207 baud (+0.006 %).
     */
    if (XUartPs_SetDataFormat(&s_uart, &line_format) != XST_SUCCESS)
    {
        return UART_DRIVER_ERR_FORMAT;
    }

    XUartPs_SetOperMode(&s_uart, XUARTPS_OPER_MODE_NORMAL);
    s_tx_ready = true;

    XUartPs_SetFifoThreshold(&s_uart, (u8)UART_RX_FIFO_TRIGGER_LEVEL);
    XUartPs_SetRecvTimeout(&s_uart, (u8)UART_RX_TIMEOUT_DISABLED);

    /* Same ack-then-drain order as the ISR, for the same reason. */
    rx_reset_state();
    tx_reset_state();
    XUartPs_WriteReg(s_uart_base, XUARTPS_ISR_OFFSET, XUARTPS_IXR_MASK);
    while ((XUartPs_ReadReg(s_uart_base, XUARTPS_SR_OFFSET) & XUARTPS_SR_RXEMPTY) == 0U)
    {
        (void)XUartPs_ReadReg(s_uart_base, XUARTPS_FIFO_OFFSET);
    }

    if (XSetupInterruptSystem(&s_uart, uart_driver_isr, uart_cfg->IntrId, uart_cfg->IntrParent,
                              XINTERRUPT_DEFAULT_PRIORITY) != XST_SUCCESS)
    {
        return UART_DRIVER_ERR_IRQ;
    }

    XUartPs_SetInterruptMask(&s_uart, UART_RX_IRQS);

    return UART_DRIVER_OK;
}

bool uart_driver_line_ready(void)
{
    return s_lines_committed != s_lines_taken;
}

bool uart_driver_take_line(uart_line_t *line_out)
{
    if ((line_out == NULL) || !uart_driver_line_ready())
    {
        return false;
    }

    *line_out = s_line_queue[s_lines_taken & UART_LINE_QUEUE_MASK];

    /* Releases the slot to the ISR. A single store, so the ISR can't see half of it. */
    s_lines_taken++;

    return true;
}

/*
 * Appends one byte to the ring. It waits only when the ring is full, which
 * means the application is producing faster than 115200 baud can carry away.
 * The kick inside the wait keeps the transmitter running, so the wait ends.
 */
static void uart_tx_push(uint8_t byte)
{
    uint32_t pending = uart_tx_pending();

    if (pending >= UART_TX_RING_SIZE)
    {
        s_tx_stats.stalls++;

        do
        {
            uart_tx_kick();
            pending = uart_tx_pending();
        } while (pending >= UART_TX_RING_SIZE);
    }

    s_tx_ring[s_tx_head & UART_TX_RING_MASK] = byte;
    s_tx_head++;

    pending++;
    if (pending > s_tx_stats.ring_high_water)
    {
        s_tx_stats.ring_high_water = pending;
    }
}

void uart_driver_write(const char *data, size_t length)
{
    size_t idx;

    if ((!s_tx_ready) || (data == NULL))
    {
        return;
    }

    for (idx = 0U; idx < length; idx++)
    {
        /* Terminals want CR LF; the application sources just use '\n'. */
        if (data[idx] == '\n')
        {
            uart_tx_push((uint8_t)ASCII_CR);
        }

        uart_tx_push((uint8_t)data[idx]);
    }

    uart_tx_kick();
}

/*
 * Waits for the ring, the FIFO and the shift register. Kicking inside the loop
 * means this also works with interrupts disabled, which is what a last message
 * before a reset needs.
 */
void uart_driver_flush(void)
{
    uint32_t status;

    if (!s_tx_ready)
    {
        return;
    }

    while (uart_tx_pending() != 0U)
    {
        uart_tx_kick();
    }

    do
    {
        status = XUartPs_ReadReg(s_uart_base, XUARTPS_SR_OFFSET);
    } while (((status & XUARTPS_SR_TXEMPTY) == 0U) || ((status & XUARTPS_SR_TACTIVE) != 0U));
}

void uart_driver_puts(const char *text)
{
    if (text != NULL)
    {
        uart_driver_write(text, strlen(text));
    }
}

void uart_driver_get_rx_stats(uart_rx_stats_t *stats_out)
{
    if (stats_out != NULL)
    {
        *stats_out = s_rx_stats;
    }
}

void uart_driver_get_tx_stats(uart_tx_stats_t *stats_out)
{
    if (stats_out != NULL)
    {
        *stats_out = s_tx_stats;
    }
}
