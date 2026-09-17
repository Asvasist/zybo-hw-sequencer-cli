/*
 * test_sequencer.c
 *
 * Host-side unit tests for sequencer.c. The state machine runs against the
 * real HAL and the board model, with time supplied by the test, so a
 * ten-minute sequence or a 49.7-day wrap costs microseconds to check:
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -DSDT -Imock -I../../src \
 *       test_sequencer.c mock/mock_board.c mock/mock_time.c ../../src/sequencer.c \
 *       ../../src/hal.c ../../src/gpio_drv.c ../../src/xadc_drv.c ../../src/uptime_drv.c -o test_sequencer
 *   ./test_sequencer
 *
 * Exit code 0 means every check passed.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mock_board.h"
#include "mock_time.h"

#include "hal.h"
#include "sequencer.h"

#define PIN_LD4             7U      /* board_zybo.h maps LED 0 here */
#define TEST_LED            0U
#define TEST_UNAVAILABLE_LED 1U     /* a PL LED: reserved, not wired yet */

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

static void boot(void)
{
    mock_board_reset();
    mock_time_reset();
    CHECK(HAL_Init() == HAL_OK);
    sequencer_init();

    /* From here on, a write to the LED pin is the code under test doing it. */
    mock_board.pin[PIN_LD4].writes = 0U;
}

static bool led_is_on(void)
{
    return mock_board.pin[PIN_LD4].level != 0U;
}

/* Steps the machine over a stretch of time, one millisecond at a time. */
static seq_event_t run_until(uint32_t from_ms, uint32_t to_ms)
{
    seq_event_t last = SEQ_EVENT_NONE;
    uint32_t    now;

    for (now = from_ms; now != to_ms; now++)
    {
        const seq_event_t event = sequencer_step(now);

        if (event != SEQ_EVENT_NONE)
        {
            last = event;
        }
    }

    return last;
}

static void test_idle_does_nothing(void)
{
    seq_info_t info;

    printf("an idle sequencer touches nothing\n");
    boot();

    CHECK(!sequencer_busy());
    CHECK(sequencer_step(0U) == SEQ_EVENT_NONE);
    CHECK(sequencer_step(1000000U) == SEQ_EVENT_NONE);
    CHECK(!sequencer_stop());               /* nothing to stop */
    CHECK(mock_board.pin[PIN_LD4].writes == 0U);

    sequencer_get_info(&info, 5000U);
    CHECK(info.state == SEQ_STATE_IDLE);
    CHECK(info.elapsed_ms == 0U);
    CHECK(info.remaining_ms == 0U);
}

static void test_blink_schedule(void)
{
    seq_info_t info;

    printf("the LED follows the schedule, and the sequence ends on time\n");
    boot();

    /* 1000 ms of blinking, a change every 250 ms. */
    CHECK(sequencer_start_blink(TEST_LED, 1000U, 250U, 0U) == SEQ_OK);
    CHECK(sequencer_busy());
    CHECK(led_is_on());                     /* on immediately, not at the first step */
    CHECK(sequencer_step(0U) == SEQ_EVENT_NONE);
    CHECK(led_is_on());

    CHECK(run_until(1U, 250U) == SEQ_EVENT_NONE);
    CHECK(led_is_on());                     /* still on just before the first toggle */

    CHECK(sequencer_step(250U) == SEQ_EVENT_NONE);
    CHECK(!led_is_on());

    CHECK(run_until(251U, 500U) == SEQ_EVENT_NONE);
    CHECK(!led_is_on());
    CHECK(sequencer_step(500U) == SEQ_EVENT_NONE);
    CHECK(led_is_on());

    CHECK(sequencer_step(750U) == SEQ_EVENT_NONE);
    CHECK(!led_is_on());

    sequencer_get_info(&info, 900U);
    CHECK(info.state == SEQ_STATE_BLINK_OFF);
    CHECK(info.elapsed_ms == 900U);
    CHECK(info.remaining_ms == 100U);
    CHECK(info.toggles == 4U);              /* on, off, on, off */
    CHECK(info.missed_toggles == 0U);

    /* The end of the duration wins, and it leaves the LED off. */
    CHECK(sequencer_step(999U) == SEQ_EVENT_NONE);
    CHECK(sequencer_step(1000U) == SEQ_EVENT_FINISHED);
    CHECK(!sequencer_busy());
    CHECK(!led_is_on());

    /* Nothing more happens afterwards. */
    CHECK(sequencer_step(1001U) == SEQ_EVENT_NONE);
    CHECK(run_until(1002U, 2000U) == SEQ_EVENT_NONE);
}

static void test_blink_leaves_led_off_from_either_phase(void)
{
    printf("a sequence always ends with the LED off\n");

    /* Ends during an ON phase: 300 ms long, toggling every 200 ms. */
    boot();
    CHECK(sequencer_start_blink(TEST_LED, 300U, 200U, 0U) == SEQ_OK);
    CHECK(sequencer_step(200U) == SEQ_EVENT_NONE);
    CHECK(!led_is_on());
    CHECK(run_until(201U, 300U) == SEQ_EVENT_NONE);
    CHECK(sequencer_step(300U) == SEQ_EVENT_FINISHED);
    CHECK(!led_is_on());

    /*
     * Ends while the LED is on: two toggles (off at 100, on at 200), then the
     * duration runs out at 250 with the LED lit. The end has to switch it off.
     */
    boot();
    CHECK(sequencer_start_blink(TEST_LED, 250U, 100U, 0U) == SEQ_OK);
    CHECK(sequencer_step(100U) == SEQ_EVENT_NONE);
    CHECK(!led_is_on());
    CHECK(sequencer_step(200U) == SEQ_EVENT_NONE);
    CHECK(led_is_on());
    CHECK(sequencer_step(250U) == SEQ_EVENT_FINISHED);
    CHECK(!led_is_on());

    /* A toggle falling due exactly at the end must not leave it on. */
    boot();
    CHECK(sequencer_start_blink(TEST_LED, 500U, 250U, 0U) == SEQ_OK);
    CHECK(sequencer_step(250U) == SEQ_EVENT_NONE);
    CHECK(sequencer_step(500U) == SEQ_EVENT_FINISHED);
    CHECK(!led_is_on());
    CHECK(!sequencer_busy());
}

static void test_stop_and_restart(void)
{
    printf("STOP aborts, and a new sequence can start straight away\n");
    boot();

    CHECK(sequencer_start_blink(TEST_LED, 10000U, 250U, 0U) == SEQ_OK);
    CHECK(run_until(1U, 600U) == SEQ_EVENT_NONE);
    CHECK(sequencer_busy());

    CHECK(sequencer_stop());
    CHECK(!sequencer_busy());
    CHECK(!led_is_on());                    /* stopping switches it off */
    CHECK(!sequencer_stop());               /* and again does nothing */

    /* No late event from the sequence that was aborted. */
    CHECK(run_until(601U, 11000U) == SEQ_EVENT_NONE);

    CHECK(sequencer_start_blink(TEST_LED, 100U, 50U, 11000U) == SEQ_OK);
    CHECK(led_is_on());
    CHECK(sequencer_step(11100U) == SEQ_EVENT_FINISHED);
}

static void test_start_is_refused_when_busy(void)
{
    seq_info_t info;

    printf("a second sequence is refused, not silently substituted\n");
    boot();

    CHECK(sequencer_start_blink(TEST_LED, 5000U, 500U, 0U) == SEQ_OK);
    CHECK(sequencer_start_blink(TEST_LED, 100U, 10U, 100U) == SEQ_ERR_BUSY);

    /* The original schedule is untouched. */
    sequencer_get_info(&info, 100U);
    CHECK(info.duration_ms == 5000U);
    CHECK(info.toggle_ms == 500U);
    CHECK(info.remaining_ms == 4900U);
}

static void test_argument_checks(void)
{
    printf("durations and toggle intervals are bounded\n");
    boot();

    CHECK(sequencer_start_blink(TEST_LED, 0U, 250U, 0U) == SEQ_ERR_BAD_DURATION);
    CHECK(sequencer_start_blink(TEST_LED, SEQ_MAX_DURATION_MS + 1U, 250U, 0U) == SEQ_ERR_BAD_DURATION);
    CHECK(sequencer_start_blink(TEST_LED, 1000U, 0U, 0U) == SEQ_ERR_BAD_TOGGLE);
    CHECK(sequencer_start_blink(TEST_LED, 1000U, 1001U, 0U) == SEQ_ERR_BAD_TOGGLE);
    CHECK(!sequencer_busy());
    CHECK(mock_board.pin[PIN_LD4].writes == 0U);        /* nothing was driven */

    /* The extremes themselves are fine. */
    CHECK(sequencer_start_blink(TEST_LED, SEQ_MAX_DURATION_MS, SEQ_MAX_DURATION_MS, 0U) == SEQ_OK);
    CHECK(sequencer_stop());
    CHECK(sequencer_start_blink(TEST_LED, 1U, 1U, 0U) == SEQ_OK);
    CHECK(sequencer_step(1U) == SEQ_EVENT_FINISHED);
}

static void test_hardware_refusal(void)
{
    printf("an LED the board cannot drive is refused before anything starts\n");
    boot();

    /* A reserved PL LED: the HAL says no, so the sequence never begins. */
    CHECK(sequencer_start_blink(TEST_UNAVAILABLE_LED, 1000U, 100U, 0U) == SEQ_ERR_HARDWARE);
    CHECK(!sequencer_busy());
    CHECK(mock_board.pin[PIN_LD4].writes == 0U);    /* and no other LED was touched */
}

/* A board that stops answering halfway through ends the sequence, and says so. */
static void test_hardware_failure_mid_sequence(void)
{
    printf("an LED that stops answering aborts the sequence\n");
    boot();

    CHECK(sequencer_start_blink(TEST_LED, 10000U, 100U, 0U) == SEQ_OK);
    CHECK(led_is_on());

    /* The GPIO driver loses its instance: every write from now on is refused. */
    mock_board.gpio_fail_lookup = true;
    CHECK(HAL_Init() == HAL_ERR_LED_INIT);
    CHECK(HAL_SetLED(TEST_LED, false) == HAL_ERR_NOT_READY);

    CHECK(sequencer_step(100U) == SEQ_EVENT_HARDWARE_FAILED);
    CHECK(!sequencer_busy());
    CHECK(sequencer_step(200U) == SEQ_EVENT_NONE);  /* and it stays stopped */

    mock_board.gpio_fail_lookup = false;
}

/*
 * A step that arrives a whole interval late must not fire the toggles it
 * missed back to back; it resynchronises and counts them.
 */
static void test_late_steps_resynchronise(void)
{
    seq_info_t info;

    printf("a late step resynchronises instead of firing a burst\n");
    boot();

    CHECK(sequencer_start_blink(TEST_LED, 10000U, 100U, 0U) == SEQ_OK);
    CHECK(sequencer_step(100U) == SEQ_EVENT_NONE);      /* first toggle, on time */
    CHECK(!led_is_on());

    /* Now nothing steps the machine for 850 ms - eight toggles missed. */
    CHECK(sequencer_step(950U) == SEQ_EVENT_NONE);
    CHECK(led_is_on());                                 /* exactly one change */

    /*
     * The toggle due at 200 ms is the one that just happened, late. The
     * deadlines at 300, 400 ... 900 went by unserved: seven of them.
     */
    sequencer_get_info(&info, 950U);
    CHECK(info.toggles == 3U);                          /* on, off, on */
    CHECK(info.missed_toggles == 7U);

    /* And the schedule continues from now, not from the old deadline. */
    CHECK(sequencer_step(1000U) == SEQ_EVENT_NONE);
    CHECK(led_is_on());
    CHECK(sequencer_step(1050U) == SEQ_EVENT_NONE);
    CHECK(!led_is_on());
}

/*
 * The millisecond stamp wraps every 49.7 days. A sequence started just before
 * the wrap has to keep its schedule across it.
 */
static void test_millisecond_wrap(void)
{
    const uint32_t start = 0xFFFFFF00UL;    /* 256 ms before the wrap */
    seq_info_t     info;

    printf("a sequence started just before the 49.7-day wrap keeps its schedule\n");
    boot();

    CHECK(sequencer_start_blink(TEST_LED, 500U, 100U, start) == SEQ_OK);
    CHECK(led_is_on());

    CHECK(sequencer_step(start + 100U) == SEQ_EVENT_NONE);
    CHECK(!led_is_on());
    CHECK(sequencer_step(start + 200U) == SEQ_EVENT_NONE);
    CHECK(led_is_on());

    /* These stamps are past the wrap: small numbers, and still later in time. */
    CHECK(sequencer_step(start + 300U) == SEQ_EVENT_NONE);
    CHECK(!led_is_on());

    sequencer_get_info(&info, start + 400U);
    CHECK(info.elapsed_ms == 400U);
    CHECK(info.remaining_ms == 100U);
    CHECK(info.missed_toggles == 0U);

    CHECK(sequencer_step(start + 500U) == SEQ_EVENT_FINISHED);
    CHECK(!led_is_on());
    CHECK(!sequencer_busy());
}

/* Ten minutes at 1 ms per step, to show the schedule never drifts. */
static void test_long_sequence_does_not_drift(void)
{
    const uint32_t duration = SEQ_MAX_DURATION_MS;
    const uint32_t toggle   = 1000U;
    seq_info_t     info;
    uint32_t       now;
    seq_event_t    event      = SEQ_EVENT_NONE;
    uint32_t       last_edge  = 0U;
    bool           spacing_ok = true;
    bool           was_on;

    printf("ten minutes of blinking: every edge exactly %lu ms apart\n", (unsigned long)toggle);
    boot();

    CHECK(sequencer_start_blink(TEST_LED, duration, toggle, 0U) == SEQ_OK);
    was_on = led_is_on();

    for (now = 1U; now <= duration; now++)
    {
        event = sequencer_step(now);

        if (led_is_on() != was_on)
        {
            spacing_ok = spacing_ok && ((now - last_edge) == toggle);
            last_edge  = now;
            was_on     = led_is_on();
        }

        if (event != SEQ_EVENT_NONE)
        {
            break;
        }
    }

    CHECK(event == SEQ_EVENT_FINISHED);
    CHECK(now == duration);
    CHECK(spacing_ok);

    sequencer_get_info(&info, now);
    CHECK(info.missed_toggles == 0U);
    CHECK(info.toggles == (duration / toggle));     /* the initial on, plus one per interval */
    CHECK(!led_is_on());
}

static void test_text_helpers(void)
{
    int value;

    printf("every state and status has a description\n");

    for (value = (int)SEQ_STATE_IDLE; value < (int)SEQ_STATE_COUNT; value++)
    {
        const char *const text = sequencer_state_text((seq_state_t)value);

        CHECK((text != NULL) && (text[0] != '\0'));
        CHECK(strcmp(text, "unknown state") != 0);
    }
    CHECK(strcmp(sequencer_state_text((seq_state_t)99), "unknown state") == 0);

    for (value = (int)SEQ_OK; value <= (int)SEQ_ERR_HARDWARE; value++)
    {
        const char *const text = sequencer_status_text((seq_status_t)value);

        CHECK((text != NULL) && (text[0] != '\0'));
        CHECK(strcmp(text, "unknown sequencer status") != 0);
    }
    CHECK(strcmp(sequencer_status_text((seq_status_t)99), "unknown sequencer status") == 0);

    sequencer_get_info(NULL, 0U);       /* must not crash */
}

int main(void)
{
    test_idle_does_nothing();
    test_blink_schedule();
    test_blink_leaves_led_off_from_either_phase();
    test_stop_and_restart();
    test_start_is_refused_when_busy();
    test_argument_checks();
    test_hardware_refusal();
    test_hardware_failure_mid_sequence();
    test_late_steps_resynchronise();
    test_millisecond_wrap();
    test_long_sequence_does_not_drift();
    test_text_helpers();

    printf("\n%u checks, %u failed\n", s_checks_run, s_checks_failed);
    return (s_checks_failed == 0U) ? 0 : 1;
}
