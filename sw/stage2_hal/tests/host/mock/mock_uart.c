/*
 * mock_uart.c - host model of the Zynq UART and the XUartPs driver calls.
 * See mock_uart.h.
 */
#include "mock_uart.h"

#include <string.h>

#include "xinterrupt_wrap.h"
#include "xparameters.h"

#define MOCK_IRQ_STORM_LIMIT        1000U
#define MOCK_UART_REF_CLOCK_HZ      100000000U
#define MOCK_CFGINIT_RX_TRIGGER     8U      /* XUartPs_CfgInitialize() defaults */
#define MOCK_CFGINIT_RX_TIMEOUT     1U
#define MOCK_COMPONENT_IS_READY     0x11111111U

typedef void (*mock_irq_handler_t)(void *callback_ref);

mock_uart_state_t mock_uart;

static XUartPs_Config      s_config;
static u8                  s_rx_fifo[MOCK_UART_RX_FIFO_DEPTH];
static size_t              s_rx_head;
static size_t              s_rx_count;
static u32                 s_isr;
static char                s_tx[MOCK_UART_TX_CAPTURE_LEN];
static size_t              s_tx_len;
static mock_irq_handler_t  s_irq_handler;
static bool                s_late_byte_armed;
static u8                  s_late_byte;
static mock_uart_tx_hook_t s_tx_hook;

static void check_base(u32 base_address)
{
    if (base_address != s_config.BaseAddress)
    {
        mock_uart.bad_base_accesses++;
    }
}

static bool rx_push(u8 byte)
{
    if (s_rx_count == MOCK_UART_RX_FIFO_DEPTH)
    {
        s_isr |= XUARTPS_IXR_OVER;
        return false;
    }

    s_rx_fifo[(s_rx_head + s_rx_count) % MOCK_UART_RX_FIFO_DEPTH] = byte;
    s_rx_count++;

    /* A trigger level of 0 disables the trigger, as on the real UART. */
    if ((mock_uart.rx_trigger_level != 0U) && (s_rx_count >= mock_uart.rx_trigger_level))
    {
        s_isr |= XUARTPS_IXR_RXOVR;
    }
    if (s_rx_count == MOCK_UART_RX_FIFO_DEPTH)
    {
        s_isr |= XUARTPS_IXR_RXFULL;
    }

    return true;
}

static void tx_append(u8 byte)
{
    if (s_tx_len < (MOCK_UART_TX_CAPTURE_LEN - 1U))
    {
        s_tx[s_tx_len] = (char)byte;
        s_tx_len++;
        s_tx[s_tx_len] = '\0';
    }

    if (s_tx_hook != NULL)
    {
        s_tx_hook(byte);
    }
}

void mock_uart_reset(void)
{
    memset(&mock_uart, 0, sizeof(mock_uart));

    s_config.Name               = "serial@e0001000";
    s_config.BaseAddress        = XPAR_XUARTPS_0_BASEADDR;
    s_config.InputClockHz       = MOCK_UART_REF_CLOCK_HZ;
    s_config.ModemPinsConnected = 0;
    s_config.RefClk             = MOCK_UART_REF_CLOCK_HZ;
    s_config.IntrId             = XPAR_XUARTPS_0_INTERRUPTS;
    s_config.IntrParent         = XPAR_XUARTPS_0_INTERRUPT_PARENT;

    s_rx_head         = 0U;
    s_rx_count        = 0U;
    s_isr             = 0U;
    s_irq_handler     = NULL;
    s_late_byte_armed = false;
    s_tx_hook         = NULL;
    mock_uart_tx_clear();
}

void mock_uart_set_tx_hook(mock_uart_tx_hook_t hook)
{
    s_tx_hook = hook;
}

bool mock_uart_receive(const char *bytes, size_t length)
{
    bool   all_stored = true;
    size_t idx;

    for (idx = 0U; idx < length; idx++)
    {
        all_stored = rx_push((u8)bytes[idx]) && all_stored;
    }

    return all_stored;
}

void mock_uart_raise_status(u32 isr_bits)
{
    s_isr |= isr_bits;
}

unsigned mock_uart_service_irq(void)
{
    unsigned calls = 0U;

    if (!mock_uart.irq_connected || (s_irq_handler == NULL))
    {
        return 0U;
    }

    while ((s_isr & mock_uart.imr) != 0U)
    {
        if (calls == MOCK_IRQ_STORM_LIMIT)
        {
            mock_uart.irq_storm = true;
            break;
        }
        s_irq_handler(mock_uart.irq_ctx);
        calls++;
    }

    return calls;
}

void mock_uart_arm_late_byte(char byte)
{
    s_late_byte       = (u8)byte;
    s_late_byte_armed = true;
}

size_t mock_uart_rx_fifo_count(void)
{
    return s_rx_count;
}

u32 mock_uart_isr_bits(void)
{
    return s_isr;
}

const char *mock_uart_tx_text(void)
{
    return s_tx;
}

void mock_uart_tx_clear(void)
{
    s_tx_len = 0U;
    s_tx[0]  = '\0';
}

/* ---- register model ---- */

u32 mock_uart_read_reg(u32 base_address, u32 offset)
{
    check_base(base_address);

    switch (offset)
    {
    case XUARTPS_ISR_OFFSET:
        return s_isr;

    case XUARTPS_IMR_OFFSET:
        return mock_uart.imr;

    case XUARTPS_SR_OFFSET:
    {
        u32 status = XUARTPS_SR_TXEMPTY;    /* the model transmits instantly */

        if (s_rx_count == 0U)
        {
            status |= XUARTPS_SR_RXEMPTY;
        }
        if (s_rx_count == MOCK_UART_RX_FIFO_DEPTH)
        {
            status |= XUARTPS_SR_RXFULL;
        }
        if ((mock_uart.rx_trigger_level != 0U) && (s_rx_count >= mock_uart.rx_trigger_level))
        {
            status |= XUARTPS_SR_RXOVR;
        }

        /* The caller has already been told "empty"; the byte lands right after. */
        if ((s_rx_count == 0U) && s_late_byte_armed)
        {
            s_late_byte_armed = false;
            (void)rx_push(s_late_byte);
        }

        return status;
    }

    case XUARTPS_FIFO_OFFSET:
    {
        u8 byte;

        if (s_rx_count == 0U)
        {
            mock_uart.unexpected_accesses++;
            return 0U;
        }

        byte      = s_rx_fifo[s_rx_head];
        s_rx_head = (s_rx_head + 1U) % MOCK_UART_RX_FIFO_DEPTH;
        s_rx_count--;
        return byte;
    }

    default:
        mock_uart.unexpected_accesses++;
        return 0U;
    }
}

void mock_uart_write_reg(u32 base_address, u32 offset, u32 value)
{
    check_base(base_address);

    switch (offset)
    {
    case XUARTPS_ISR_OFFSET:
        s_isr &= ~value;                    /* write one to clear */
        break;

    case XUARTPS_IER_OFFSET:
        mock_uart.imr |= value;
        break;

    case XUARTPS_IDR_OFFSET:
        mock_uart.imr &= ~value;
        break;

    case XUARTPS_FIFO_OFFSET:
        tx_append((u8)value);
        break;

    default:
        mock_uart.unexpected_accesses++;
        break;
    }
}

/* ---- XUartPs driver API ---- */

XUartPs_Config *XUartPs_LookupConfig(u32 BaseAddress)
{
    if (mock_uart.fail_lookup || (BaseAddress != s_config.BaseAddress))
    {
        return NULL;
    }

    return &s_config;
}

s32 XUartPs_CfgInitialize(XUartPs *InstancePtr, XUartPs_Config *Config, u32 EffectiveAddr)
{
    if (mock_uart.fail_cfg_init)
    {
        return XST_FAILURE;
    }

    InstancePtr->Config             = *Config;
    InstancePtr->Config.BaseAddress = EffectiveAddr;
    InstancePtr->IsReady            = MOCK_COMPONENT_IS_READY;

    /* What the real one leaves behind: trigger 8, timeout 1, every source masked. */
    mock_uart.rx_trigger_level = MOCK_CFGINIT_RX_TRIGGER;
    mock_uart.rx_timeout       = MOCK_CFGINIT_RX_TIMEOUT;
    mock_uart.imr              = 0U;

    return XST_SUCCESS;
}

s32 XUartPs_SetDataFormat(XUartPs *InstancePtr, XUartPsFormat *FormatPtr)
{
    if (mock_uart.fail_set_format)
    {
        return XST_FAILURE;
    }

    InstancePtr->BaudRate = FormatPtr->BaudRate;
    mock_uart.baud_rate   = FormatPtr->BaudRate;
    mock_uart.data_bits   = FormatPtr->DataBits;
    mock_uart.parity      = FormatPtr->Parity;
    mock_uart.stop_bits   = FormatPtr->StopBits;

    return XST_SUCCESS;
}

void XUartPs_SetOperMode(XUartPs *InstancePtr, u8 OperationMode)
{
    (void)InstancePtr;
    mock_uart.oper_mode = OperationMode;
}

void XUartPs_SetFifoThreshold(XUartPs *InstancePtr, u8 TriggerLevel)
{
    (void)InstancePtr;
    mock_uart.rx_trigger_level = TriggerLevel;
}

void XUartPs_SetRecvTimeout(XUartPs *InstancePtr, u8 RecvTimeout)
{
    (void)InstancePtr;
    mock_uart.rx_timeout = RecvTimeout;
}

void XUartPs_SetInterruptMask(XUartPs *InstancePtr, u32 Mask)
{
    (void)InstancePtr;
    mock_uart.imr = Mask;
}

void XUartPs_SendByte(u32 BaseAddress, u8 Data)
{
    check_base(BaseAddress);
    tx_append(Data);
}

/* ---- interrupt wrapper ---- */

int XSetupInterruptSystem(void *DriverInstance, void *IntrHandler, u32 IntrId, UINTPTR IntcParent, u16 Priority)
{
    if (mock_uart.fail_irq_setup)
    {
        return (int)XST_FAILURE;
    }

    s_irq_handler           = (mock_irq_handler_t)IntrHandler;
    mock_uart.irq_ctx       = DriverInstance;
    mock_uart.irq_id        = IntrId;
    mock_uart.irq_parent    = IntcParent;
    mock_uart.irq_priority  = Priority;
    mock_uart.irq_connected = true;

    return (int)XST_SUCCESS;
}
