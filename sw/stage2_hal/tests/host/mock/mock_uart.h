/*
 * mock_uart.h - test-side controls of the host UART model.
 *
 * mock_uart.c models the parts of the Zynq UART and the XUartPs/interrupt
 * wrapper API that uart_driver.c uses: a 64-byte RX FIFO, the interrupt
 * status and mask registers (write-one-to-clear status, level-sensitive
 * interrupt line) and a TX capture buffer. The real uart_driver.c is compiled
 * against it unchanged - no #ifdefs in the production code.
 */
#ifndef MOCK_UART_H
#define MOCK_UART_H

#include <stdbool.h>
#include <stddef.h>

#include "xuartps.h"

#define MOCK_UART_RX_FIFO_DEPTH     64U     /* same as the Zynq UART */
#define MOCK_UART_TX_CAPTURE_LEN    4096U

typedef struct
{
    /* Failure injection: set after mock_uart_reset(), before uart_driver_init(). */
    bool    fail_lookup;
    bool    fail_cfg_init;
    bool    fail_set_format;
    bool    fail_irq_setup;

    /* What the driver configured. */
    u32     baud_rate;
    u32     data_bits;
    u32     parity;
    u8      stop_bits;
    u8      oper_mode;
    u8      rx_trigger_level;
    u8      rx_timeout;
    u32     imr;                    /* interrupt mask: 1 = source enabled */
    bool    irq_connected;
    u32     irq_id;
    UINTPTR irq_parent;
    u16     irq_priority;
    void   *irq_ctx;

    /* Things a correct driver never causes. */
    unsigned bad_base_accesses;     /* register access outside UART1 */
    unsigned unexpected_accesses;   /* unmodelled register, or FIFO read while empty */
    bool     irq_storm;             /* interrupt stayed asserted: status never acknowledged */
} mock_uart_state_t;

extern mock_uart_state_t mock_uart;

/* Power-on state: empty FIFOs, no status, nothing configured, no handler. */
void mock_uart_reset(void);

/*
 * Bytes arrive on the RX pin. Each lands in the FIFO and sets the trigger
 * status bit once the fill reaches the trigger level. A byte that finds the
 * FIFO full is lost and sets the overrun bit, as on the real UART.
 * Returns false if anything was lost. Does not run the ISR.
 */
bool mock_uart_receive(const char *bytes, size_t length);

/* Sets interrupt status bits directly, e.g. XUARTPS_IXR_FRAMING. */
void mock_uart_raise_status(u32 isr_bits);

/*
 * Plays the GIC: calls the registered handler for as long as an enabled
 * status bit is set, like a level-sensitive line. Returns how often the
 * handler ran. Gives up and sets irq_storm after 1000 calls.
 */
unsigned mock_uart_service_irq(void);

/*
 * Delivers `byte` the moment the driver next reads the status register and
 * finds the RX FIFO empty - the worst possible time, right after its last
 * look. Used to prove the ISR acknowledges before it drains.
 */
void mock_uart_arm_late_byte(char byte);

/*
 * Called with every transmitted byte, after it is captured. NULL removes it.
 * The simulator uses it to watch for the prompt and type the next line.
 */
typedef void (*mock_uart_tx_hook_t)(u8 byte);
void mock_uart_set_tx_hook(mock_uart_tx_hook_t hook);

size_t      mock_uart_rx_fifo_count(void);
u32         mock_uart_isr_bits(void);
const char *mock_uart_tx_text(void);        /* everything transmitted, NUL-terminated */
void        mock_uart_tx_clear(void);

#endif /* MOCK_UART_H */
