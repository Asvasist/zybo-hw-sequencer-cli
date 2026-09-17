/*
 * hal.h
 *
 * Hardware abstraction layer.
 *
 * Everything above this line - the CLI today, the sequencer FSM from stage 3 -
 * speaks in board-independent terms: LED ids and a temperature in
 * milli-degrees. Everything below it (gpio_drv, xadc_drv and the Xilinx
 * drivers) deals in pins, channels and registers. The application includes no
 * Xilinx header and touches no register, so it can be read, reviewed and host
 * tested without a board, and a move to different hardware stops here.
 *
 * LED ids are logical and stable. Id 0 is the LED the processing system
 * drives on its own, which is the only one that exists in a PS-only design.
 * The four PL LEDs keep their ids reserved and report themselves as
 * unavailable until the hardware design lands, so a command script written
 * today keeps working afterwards.
 *
 * Every call is safe before HAL_Init() and with any argument: the result is a
 * status code, never a wild write.
 */
#ifndef HAL_H
#define HAL_H

#include <stdbool.h>
#include <stdint.h>

/* Logical LEDs: id 0 is on the PS, ids 1..4 wait for the PL design. */
#define HAL_LED_COUNT       5U

typedef enum
{
    HAL_OK = 0,
    HAL_ERR_BAD_ARG,        /* NULL output pointer                                   */
    HAL_ERR_BAD_ID,         /* no such LED id                                        */
    HAL_ERR_UNAVAILABLE,    /* the id exists on the board, but not in this build      */
    HAL_ERR_NOT_READY,      /* that part of the hardware did not come up             */
    HAL_ERR_LED_INIT,       /* HAL_Init(): the GPIO driver failed                     */
    HAL_ERR_SENSOR_INIT,    /* HAL_Init(): the XADC driver failed                     */
    HAL_ERR_TIME_INIT       /* HAL_Init(): the time base never started                */
} hal_status_t;

/*
 * Brings up the board hardware: LED pins as driven outputs, all off, and the
 * XADC sequencer. The console UART is not part of this - the application
 * starts it first, so that a failure here can still be reported to the
 * terminal.
 *
 * Both parts are always attempted, so a broken sensor doesn't cost you the
 * LEDs. Returns HAL_OK only if both came up, otherwise the first failure.
 * Whatever did come up stays usable, and the rest reports HAL_ERR_NOT_READY.
 */
hal_status_t HAL_Init(void);

uint8_t     HAL_LEDCount(void);
bool        HAL_LEDAvailable(uint8_t id);

/* Board name of an LED, e.g. "LD4 on MIO7". Never NULL, "?" for a bad id. */
const char *HAL_LEDName(uint8_t id);

hal_status_t HAL_SetLED(uint8_t id, bool state);
hal_status_t HAL_GetLED(uint8_t id, bool *state_out);
hal_status_t HAL_ToggleLED(uint8_t id);

/* Die temperature in milli-degrees Celsius, e.g. 42310 for 42.31 C. */
hal_status_t HAL_ReadTemperature(int32_t *milli_celsius_out);

/*
 * Milliseconds since boot. Wraps after 49.7 days, so compare differences and
 * never the values themselves. False from HAL_TimeRunning() means the time
 * base never started and every timed command should be refused.
 */
uint32_t HAL_GetUptimeMs(void);
uint64_t HAL_GetUptimeUs(void);
bool     HAL_TimeRunning(void);

/* Short human-readable text for a status. Never NULL. */
const char *HAL_StatusText(hal_status_t status);

#endif /* HAL_H */
