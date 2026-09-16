/*
 * gpio_drv.h
 *
 * PS MIO GPIO driver: the board LEDs the processing system drives directly.
 *
 * Low-level layer. It knows pin numbers and polarity and talks to the Xilinx
 * XGpioPs driver; it knows nothing about what an LED means to the
 * application. The HAL maps logical LED ids onto the ids here, and the
 * application never calls into this file at all.
 *
 * The PL LEDs LD0..LD3 need an AXI GPIO in the hardware design and get their
 * own driver beside this one in a later stage.
 */
#ifndef GPIO_DRV_H
#define GPIO_DRV_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    GPIO_DRV_OK = 0,
    GPIO_DRV_ERR_LOOKUP,        /* no config entry for the PS GPIO in xparameters.h */
    GPIO_DRV_ERR_INIT,          /* XGpioPs_CfgInitialize() rejected the config      */
    GPIO_DRV_ERR_BAD_ID,        /* no such LED on this board, or a NULL argument    */
    GPIO_DRV_ERR_NOT_READY      /* gpio_drv_init() has not succeeded                */
} gpio_drv_status_t;

typedef enum
{
    GPIO_LED_LD4 = 0,           /* MIO7, the PS user LED */
    GPIO_LED_COUNT
} gpio_led_id_t;

/* Configures every LED pin as a driven output and switches them all off. */
gpio_drv_status_t gpio_drv_init(void);

gpio_drv_status_t gpio_drv_led_write(gpio_led_id_t led, bool on);

/* Reads the pin back, so the answer is the hardware state, not a shadow copy. */
gpio_drv_status_t gpio_drv_led_read(gpio_led_id_t led, bool *on_out);

#endif /* GPIO_DRV_H */
