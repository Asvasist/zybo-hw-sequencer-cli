/*
 * test_hal.c
 *
 * Host-side unit tests for hal.c and the drivers underneath it (gpio_drv.c,
 * xadc_drv.c), run against the board model in mock/. Any C11 compiler:
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -DSDT -Imock -I../../src \
 *       test_hal.c mock/mock_board.c ../../src/hal.c ../../src/gpio_drv.c ../../src/xadc_drv.c -o test_hal
 *   ./test_hal
 *
 * Exit code 0 means every check passed.
 *
 * Test order matters in one place: the "before init" test has to run first,
 * because the drivers keep their ready flag for the life of the process, the
 * same way they do on the board.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mock_board.h"

#include "hal.h"

#define PIN_LD4                 7U      /* board_zybo.h maps LED 0 here */
#define HAL_LED_PL_FIRST        1U      /* ids 1..4 wait for the PL design */

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

/* A correct driver never touches the model in these ways. */
static void check_model_clean(void)
{
    CHECK(mock_board.gpio_bad_pin_accesses == 0U);
    CHECK(mock_board.gpio_use_before_init == 0U);
    CHECK(mock_board.xadc_use_before_init == 0U);
    CHECK(mock_board.xadc_bad_channel_reads == 0U);
}

static void boot_hal(void)
{
    mock_board_reset();
    CHECK(HAL_Init() == HAL_OK);
}

/* Runs first: nothing has been initialised yet in this process. */
static void test_before_init(void)
{
    bool    state = true;
    int32_t milli = 0;

    printf("every call is safe before HAL_Init()\n");
    mock_board_reset();

    CHECK(HAL_SetLED(0U, true) == HAL_ERR_NOT_READY);
    CHECK(HAL_GetLED(0U, &state) == HAL_ERR_NOT_READY);
    CHECK(HAL_ToggleLED(0U) == HAL_ERR_NOT_READY);
    CHECK(HAL_ReadTemperature(&milli) == HAL_ERR_NOT_READY);

    /* Nothing was written to any pin, and no XADC read was attempted. */
    CHECK(mock_board.pin[PIN_LD4].writes == 0U);
    CHECK(mock_board.gpio_use_before_init == 0U);
    CHECK(mock_board.xadc_use_before_init == 0U);
}

static void test_init_configures_the_board(void)
{
    printf("HAL_Init() configures the LED pins and the XADC sequencer\n");
    mock_board_reset();
    CHECK(HAL_Init() == HAL_OK);

    /* LD4: driven output, off, and driven low before the output was enabled. */
    CHECK(mock_board.pin[PIN_LD4].direction == 1U);
    CHECK(mock_board.pin[PIN_LD4].output_enable == 1U);
    CHECK(mock_board.pin[PIN_LD4].level == 0U);
    CHECK(mock_board.pin[PIN_LD4].writes >= 1U);
    CHECK(!mock_board.pin[PIN_LD4].first_write_had_oe);

    /* XADC: link proven, then configured in safe mode and left running. */
    CHECK(mock_board.selftest_calls == 1U);
    CHECK(mock_board.seq_mode_at_channel_setup == XADCPS_SEQ_MODE_SAFE);
    CHECK(mock_board.seq_mode_at_average_setup == XADCPS_SEQ_MODE_SAFE);
    CHECK(mock_board.seq_mode == XADCPS_SEQ_MODE_CONTINPASS);
    CHECK(mock_board.average == XADCPS_AVG_16_SAMPLES);
    CHECK(mock_board.calibration_mask ==
          (XADCPS_CFR1_CAL_PS_GAIN_OFFSET_MASK | XADCPS_CFR1_CAL_ADC_GAIN_OFFSET_MASK));
    CHECK(mock_board.seq_channels == (XADCPS_SEQ_CH_CALIB | XADCPS_SEQ_CH_TEMP));
    CHECK(mock_board.seq_average_channels == XADCPS_SEQ_CH_TEMP);

    check_model_clean();
}

/* A failure in one half of the board must not cost you the other half. */
static void test_init_failures_are_independent(void)
{
    int32_t milli = 0;
    bool    state = false;

    printf("a broken sensor keeps the LEDs, a broken GPIO keeps the sensor\n");

    mock_board_reset();
    mock_board.gpio_fail_lookup = true;
    CHECK(HAL_Init() == HAL_ERR_LED_INIT);
    CHECK(HAL_SetLED(0U, true) == HAL_ERR_NOT_READY);
    mock_board_set_temp_code(2585U);
    CHECK(HAL_ReadTemperature(&milli) == HAL_OK);       /* the XADC still came up */
    CHECK(milli == 44910);

    mock_board_reset();
    mock_board.gpio_fail_init = true;
    CHECK(HAL_Init() == HAL_ERR_LED_INIT);

    mock_board_reset();
    mock_board.xadc_fail_selftest = true;
    CHECK(HAL_Init() == HAL_ERR_SENSOR_INIT);
    CHECK(HAL_SetLED(0U, true) == HAL_OK);              /* the LEDs still came up */
    CHECK(HAL_GetLED(0U, &state) == HAL_OK);
    CHECK(state);
    CHECK(HAL_ReadTemperature(&milli) == HAL_ERR_NOT_READY);

    mock_board_reset();
    mock_board.xadc_fail_lookup = true;
    CHECK(HAL_Init() == HAL_ERR_SENSOR_INIT);

    mock_board_reset();
    mock_board.xadc_fail_init = true;
    CHECK(HAL_Init() == HAL_ERR_SENSOR_INIT);

    mock_board_reset();
    mock_board.xadc_fail_seq_channels = true;
    CHECK(HAL_Init() == HAL_ERR_SENSOR_INIT);

    mock_board_reset();
    mock_board.xadc_fail_seq_average = true;
    CHECK(HAL_Init() == HAL_ERR_SENSOR_INIT);

    check_model_clean();
}

static void test_led_map(void)
{
    uint8_t id;

    printf("the LED map: ids, names and what this build can drive\n");
    boot_hal();

    CHECK(HAL_LEDCount() == HAL_LED_COUNT);
    CHECK(HAL_LEDAvailable(0U));

    for (id = HAL_LED_PL_FIRST; id < HAL_LEDCount(); id++)
    {
        CHECK(!HAL_LEDAvailable(id));                       /* the PL LEDs, not yet */
        CHECK(HAL_LEDName(id)[0] != '\0');
    }

    CHECK(!HAL_LEDAvailable(HAL_LEDCount()));
    CHECK(strcmp(HAL_LEDName(HAL_LEDCount()), "?") == 0);
    CHECK(strcmp(HAL_LEDName(200U), "?") == 0);
    CHECK(HAL_LEDName(0U)[0] != '\0');

    check_model_clean();
}

static void test_led_control(void)
{
    bool state = false;

    printf("setting, reading back and toggling the PS LED\n");
    boot_hal();

    CHECK(HAL_SetLED(0U, true) == HAL_OK);
    CHECK(mock_board.pin[PIN_LD4].level == 1U);
    CHECK(HAL_GetLED(0U, &state) == HAL_OK);
    CHECK(state);

    CHECK(HAL_SetLED(0U, false) == HAL_OK);
    CHECK(mock_board.pin[PIN_LD4].level == 0U);
    CHECK(HAL_GetLED(0U, &state) == HAL_OK);
    CHECK(!state);

    /* Toggle reads the pin and writes the opposite, twice getting back to off. */
    CHECK(HAL_ToggleLED(0U) == HAL_OK);
    CHECK(mock_board.pin[PIN_LD4].level == 1U);
    CHECK(HAL_ToggleLED(0U) == HAL_OK);
    CHECK(mock_board.pin[PIN_LD4].level == 0U);

    /* The pin keeps its configuration: nothing else reconfigured it. */
    CHECK(mock_board.pin[PIN_LD4].direction == 1U);
    CHECK(mock_board.pin[PIN_LD4].output_enable == 1U);

    check_model_clean();
}

static void test_bad_and_unavailable_leds(void)
{
    unsigned writes_before;
    bool     state = false;

    printf("bad ids and unavailable LEDs are refused without touching a pin\n");
    boot_hal();

    writes_before = mock_board.pin[PIN_LD4].writes;

    CHECK(HAL_SetLED(HAL_LEDCount(), true) == HAL_ERR_BAD_ID);
    CHECK(HAL_SetLED(200U, true) == HAL_ERR_BAD_ID);
    CHECK(HAL_GetLED(HAL_LEDCount(), &state) == HAL_ERR_BAD_ID);
    CHECK(HAL_ToggleLED(HAL_LEDCount()) == HAL_ERR_BAD_ID);

    CHECK(HAL_SetLED(HAL_LED_PL_FIRST, true) == HAL_ERR_UNAVAILABLE);
    CHECK(HAL_GetLED(HAL_LED_PL_FIRST, &state) == HAL_ERR_UNAVAILABLE);
    CHECK(HAL_ToggleLED(HAL_LED_PL_FIRST) == HAL_ERR_UNAVAILABLE);

    CHECK(HAL_GetLED(0U, NULL) == HAL_ERR_BAD_ARG);

    /* Not one of those calls wrote to the hardware. */
    CHECK(mock_board.pin[PIN_LD4].writes == writes_before);
    CHECK(mock_board.gpio_bad_pin_accesses == 0U);

    check_model_clean();
}

/*
 * UG480: T[degC] = code * 503.975 / 4096 - 273.15. The expected values are
 * worked out by hand rather than with the same expression as the code.
 */
static void test_temperature_conversion(void)
{
    static const struct
    {
        uint16_t code;
        int32_t  milli_celsius;
    } cases[] =
    {
        { 0U,    -273150 },     /* the bottom of the scale         */
        { 2048U,  -21162 },     /* mid scale, below zero           */
        { 2585U,   44910 },     /* 0xA19, a warm Zynq: 44.91 C     */
        { 4095U,  230702 },     /* full scale                      */
    };
    int32_t milli = 0;
    size_t  idx;

    printf("temperature conversion against hand-computed values\n");
    boot_hal();

    for (idx = 0U; idx < (sizeof(cases) / sizeof(cases[0])); idx++)
    {
        mock_board_set_temp_code(cases[idx].code);
        CHECK(HAL_ReadTemperature(&milli) == HAL_OK);
        if (milli != cases[idx].milli_celsius)
        {
            printf("  code %u: expected %ld, got %ld\n",
                   (unsigned)cases[idx].code, (long)cases[idx].milli_celsius, (long)milli);
        }
        CHECK(milli == cases[idx].milli_celsius);
    }

    /* The conversion is monotonic across the whole 12-bit range. */
    {
        uint32_t code;
        int32_t  previous = -273151;
        bool     monotonic = true;

        for (code = 0U; code < 4096U; code++)
        {
            mock_board_set_temp_code((uint16_t)code);
            CHECK(HAL_ReadTemperature(&milli) == HAL_OK);
            monotonic = monotonic && (milli >= previous);
            previous  = milli;
        }
        CHECK(monotonic);
    }

    CHECK(HAL_ReadTemperature(NULL) == HAL_ERR_BAD_ARG);

    check_model_clean();
}

static void test_status_text(void)
{
    int status;

    printf("every status has a description\n");

    for (status = (int)HAL_OK; status <= (int)HAL_ERR_SENSOR_INIT; status++)
    {
        const char *const text = HAL_StatusText((hal_status_t)status);

        CHECK((text != NULL) && (text[0] != '\0'));
        CHECK(strcmp(text, "unknown hardware status") != 0);
    }

    CHECK(strcmp(HAL_StatusText((hal_status_t)99), "unknown hardware status") == 0);
}

int main(void)
{
    test_before_init();             /* must stay first */
    test_init_configures_the_board();
    test_init_failures_are_independent();
    test_led_map();
    test_led_control();
    test_bad_and_unavailable_leds();
    test_temperature_conversion();
    test_status_text();

    printf("\n%u checks, %u failed\n", s_checks_run, s_checks_failed);
    return (s_checks_failed == 0U) ? 0 : 1;
}
