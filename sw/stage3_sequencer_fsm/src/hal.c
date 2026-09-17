/*
 * hal.c
 *
 * Hardware abstraction layer. See hal.h for what it promises the application.
 *
 * All the board knowledge sits in the LED map below: which logical id is
 * which physical LED, and which driver drives it. Adding the PL LEDs later is
 * a source change to this table plus the AXI GPIO driver - no caller changes.
 */
#include "hal.h"

#include <stddef.h>

#include "gpio_drv.h"
#include "uptime_drv.h"
#include "xadc_drv.h"

/* Where an LED hangs. More sources (AXI GPIO in the PL) join this list later. */
typedef enum
{
    HAL_LED_SRC_NONE = 0,       /* reserved id, not wired in this build */
    HAL_LED_SRC_MIO             /* PS MIO pin, driven by gpio_drv       */
} hal_led_source_t;

typedef struct
{
    const char      *name;
    hal_led_source_t source;
    uint8_t          driver_id; /* index within that source's driver */
} hal_led_desc_t;

static bool s_time_ready;

static const hal_led_desc_t s_led_map[HAL_LED_COUNT] =
{
    { "LD4 on MIO7", HAL_LED_SRC_MIO,  (uint8_t)GPIO_LED_LD4 },
    { "LD0 on PL",   HAL_LED_SRC_NONE, 0U },
    { "LD1 on PL",   HAL_LED_SRC_NONE, 0U },
    { "LD2 on PL",   HAL_LED_SRC_NONE, 0U },
    { "LD3 on PL",   HAL_LED_SRC_NONE, 0U },
};

static hal_status_t hal_status_from_gpio(gpio_drv_status_t status)
{
    switch (status)
    {
    case GPIO_DRV_OK:            return HAL_OK;
    case GPIO_DRV_ERR_BAD_ID:    return HAL_ERR_BAD_ID;
    case GPIO_DRV_ERR_NOT_READY: return HAL_ERR_NOT_READY;
    case GPIO_DRV_ERR_LOOKUP:
    case GPIO_DRV_ERR_INIT:      return HAL_ERR_LED_INIT;
    }

    return HAL_ERR_LED_INIT;
}

static hal_status_t hal_status_from_xadc(xadc_drv_status_t status)
{
    switch (status)
    {
    case XADC_DRV_OK:            return HAL_OK;
    case XADC_DRV_ERR_ARG:       return HAL_ERR_BAD_ARG;
    case XADC_DRV_ERR_NOT_READY: return HAL_ERR_NOT_READY;
    case XADC_DRV_ERR_LOOKUP:
    case XADC_DRV_ERR_INIT:
    case XADC_DRV_ERR_SELFTEST:
    case XADC_DRV_ERR_SEQ_CONFIG: return HAL_ERR_SENSOR_INIT;
    }

    return HAL_ERR_SENSOR_INIT;
}

hal_status_t HAL_Init(void)
{
    const gpio_drv_status_t   led_status    = gpio_drv_init();
    const xadc_drv_status_t   sensor_status = xadc_drv_init();
    const uptime_drv_status_t time_status   = uptime_drv_init();

    s_time_ready = (time_status == UPTIME_DRV_OK);

    if (led_status != GPIO_DRV_OK)
    {
        return HAL_ERR_LED_INIT;
    }
    if (sensor_status != XADC_DRV_OK)
    {
        return HAL_ERR_SENSOR_INIT;
    }
    if (!s_time_ready)
    {
        return HAL_ERR_TIME_INIT;
    }

    return HAL_OK;
}

uint8_t HAL_LEDCount(void)
{
    return (uint8_t)HAL_LED_COUNT;
}

bool HAL_LEDAvailable(uint8_t id)
{
    return (id < HAL_LED_COUNT) && (s_led_map[id].source != HAL_LED_SRC_NONE);
}

const char *HAL_LEDName(uint8_t id)
{
    return (id < HAL_LED_COUNT) ? s_led_map[id].name : "?";
}

hal_status_t HAL_SetLED(uint8_t id, bool state)
{
    if (id >= HAL_LED_COUNT)
    {
        return HAL_ERR_BAD_ID;
    }

    switch (s_led_map[id].source)
    {
    case HAL_LED_SRC_MIO:
        return hal_status_from_gpio(gpio_drv_led_write((gpio_led_id_t)s_led_map[id].driver_id, state));

    case HAL_LED_SRC_NONE:
    default:
        return HAL_ERR_UNAVAILABLE;
    }
}

hal_status_t HAL_GetLED(uint8_t id, bool *state_out)
{
    if (state_out == NULL)
    {
        return HAL_ERR_BAD_ARG;
    }
    if (id >= HAL_LED_COUNT)
    {
        return HAL_ERR_BAD_ID;
    }

    switch (s_led_map[id].source)
    {
    case HAL_LED_SRC_MIO:
        return hal_status_from_gpio(gpio_drv_led_read((gpio_led_id_t)s_led_map[id].driver_id, state_out));

    case HAL_LED_SRC_NONE:
    default:
        return HAL_ERR_UNAVAILABLE;
    }
}

hal_status_t HAL_ToggleLED(uint8_t id)
{
    bool               state  = false;
    const hal_status_t status = HAL_GetLED(id, &state);

    if (status != HAL_OK)
    {
        return status;
    }

    return HAL_SetLED(id, !state);
}

hal_status_t HAL_ReadTemperature(int32_t *milli_celsius_out)
{
    uint16_t     code = 0U;
    hal_status_t status;

    if (milli_celsius_out == NULL)
    {
        return HAL_ERR_BAD_ARG;
    }

    status = hal_status_from_xadc(xadc_drv_read_temp_code(&code));
    if (status != HAL_OK)
    {
        return status;
    }

    *milli_celsius_out = xadc_drv_code_to_milli_c(code);
    return HAL_OK;
}

uint32_t HAL_GetUptimeMs(void)
{
    return uptime_drv_ms();
}

uint64_t HAL_GetUptimeUs(void)
{
    return uptime_drv_us();
}

bool HAL_TimeRunning(void)
{
    return s_time_ready;
}

const char *HAL_StatusText(hal_status_t status)
{
    switch (status)
    {
    case HAL_OK:              return "ok";
    case HAL_ERR_BAD_ARG:     return "internal error, NULL argument";
    case HAL_ERR_BAD_ID:      return "no such LED id";
    case HAL_ERR_UNAVAILABLE: return "not available in this build, it needs the PL design";
    case HAL_ERR_NOT_READY:   return "hardware did not come up at boot";
    case HAL_ERR_LED_INIT:    return "LED outputs failed to initialise";
    case HAL_ERR_SENSOR_INIT: return "temperature sensor failed to initialise";
    case HAL_ERR_TIME_INIT:   return "time base failed to start, timed commands are refused";
    }

    return "unknown hardware status";
}
