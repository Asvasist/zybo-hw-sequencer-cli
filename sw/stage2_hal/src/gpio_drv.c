/*
 * gpio_drv.c
 *
 * PS MIO GPIO driver on top of the Xilinx XGpioPs low-level driver.
 * Reference: UG585 chapter 14 (GPIO controller).
 */
#include "gpio_drv.h"

#include <stddef.h>

#include "xgpiops.h"
#include "xstatus.h"

#include "board_zybo.h"

#ifndef SDT
#error "gpio_drv.c targets the SDT BSP flow (Vitis 2023.2 or later); SDT is not defined"
#endif

#define GPIO_PIN_DIRECTION_OUT      1U
#define GPIO_PIN_OUTPUT_ENABLE      1U

typedef struct
{
    uint32_t mio_pin;
    bool     active_high;
} gpio_led_hw_t;

/*
 * The board wiring. Every Zybo user LED is anode-connected through a resistor,
 * so driving its pin high lights it.
 */
static const gpio_led_hw_t s_led_hw[GPIO_LED_COUNT] =
{
    [GPIO_LED_LD4] = { BOARD_MIO_LED4, true },
};

static XGpioPs s_gpio_inst;
static bool    s_gpio_ready;

static uint32_t gpio_level_for(gpio_led_id_t led, bool on)
{
    const bool drive_high = s_led_hw[led].active_high ? on : !on;

    return drive_high ? 1U : 0U;
}

gpio_drv_status_t gpio_drv_init(void)
{
    XGpioPs_Config *gpio_cfg;
    uint32_t        led;

    s_gpio_ready = false;

    gpio_cfg = XGpioPs_LookupConfig(BOARD_PS_GPIO_BASEADDR);
    if (gpio_cfg == NULL)
    {
        return GPIO_DRV_ERR_LOOKUP;
    }

    if (XGpioPs_CfgInitialize(&s_gpio_inst, gpio_cfg, gpio_cfg->BaseAddr) != XST_SUCCESS)
    {
        return GPIO_DRV_ERR_INIT;
    }

    for (led = 0U; led < (uint32_t)GPIO_LED_COUNT; led++)
    {
        const gpio_led_id_t id = (gpio_led_id_t)led;

        /*
         * Drive the off level before enabling the output, so an LED cannot
         * flash while its pin is being configured. MIO7 is output-only and
         * needs the output enable as well as the direction: with only the
         * direction set the pin stays high-impedance and nothing lights,
         * silently.
         */
        XGpioPs_WritePin(&s_gpio_inst, s_led_hw[led].mio_pin, gpio_level_for(id, false));
        XGpioPs_SetDirectionPin(&s_gpio_inst, s_led_hw[led].mio_pin, GPIO_PIN_DIRECTION_OUT);
        XGpioPs_SetOutputEnablePin(&s_gpio_inst, s_led_hw[led].mio_pin, GPIO_PIN_OUTPUT_ENABLE);
    }

    s_gpio_ready = true;
    return GPIO_DRV_OK;
}

gpio_drv_status_t gpio_drv_led_write(gpio_led_id_t led, bool on)
{
    if (led >= GPIO_LED_COUNT)
    {
        return GPIO_DRV_ERR_BAD_ID;
    }
    if (!s_gpio_ready)
    {
        return GPIO_DRV_ERR_NOT_READY;
    }

    /* Pin writes go through the MIO mask-data register: no read-modify-write. */
    XGpioPs_WritePin(&s_gpio_inst, s_led_hw[led].mio_pin, gpio_level_for(led, on));

    return GPIO_DRV_OK;
}

gpio_drv_status_t gpio_drv_led_read(gpio_led_id_t led, bool *on_out)
{
    uint32_t level;

    if ((on_out == NULL) || (led >= GPIO_LED_COUNT))
    {
        return GPIO_DRV_ERR_BAD_ID;
    }
    if (!s_gpio_ready)
    {
        return GPIO_DRV_ERR_NOT_READY;
    }

    level   = XGpioPs_ReadPin(&s_gpio_inst, s_led_hw[led].mio_pin);
    *on_out = s_led_hw[led].active_high ? (level != 0U) : (level == 0U);

    return GPIO_DRV_OK;
}
