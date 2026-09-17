/*
 * sequencer.h
 *
 * The hardware sequencer: a switch-case state machine that runs timed LED
 * sequences without ever blocking the command line.
 *
 * The main loop calls sequencer_step() on every pass with the current
 * millisecond stamp. Each call does the work that is due right now and
 * returns immediately, so a ten-second blink costs nothing between its
 * toggles and the CLI keeps accepting and answering commands throughout.
 *
 *   IDLE  --sequencer_start_blink()-->  BLINK_ON
 *     ^                                   |  ^
 *     |                        toggle due  |  |  toggle due
 *     |                                   v  |
 *     +---- duration elapsed / stop ---  BLINK_OFF
 *
 * The state also *is* the LED phase, so a toggle is a plain write of the next
 * level, never a read-modify-write of the pin.
 *
 * Time is passed in rather than read here: the caller owns the clock, which
 * keeps this file free of hardware and lets the host tests drive it through
 * days of simulated time in a few microseconds. All comparisons are on
 * differences, so the 49.7-day wrap of a 32-bit millisecond stamp is harmless.
 *
 * Deadlines are absolute and derived from the start of the sequence, so
 * toggles do not drift even if a step arrives late. If the main loop is held
 * up long enough to miss a whole toggle, the schedule is resynchronised and
 * the miss is counted rather than fired as a burst.
 *
 * Only the LEDs are sequenced today. The pattern generalises: a longer
 * sequence becomes more states and one table of steps.
 */
#ifndef SEQUENCER_H
#define SEQUENCER_H

#include <stdbool.h>
#include <stdint.h>

/* Used when LED_BLINK is given no toggle interval. */
#define SEQ_DEFAULT_TOGGLE_MS   250U

/* Bounds the CLI enforces, kept here so the tests and the help text agree. */
#define SEQ_MIN_DURATION_MS     1U
#define SEQ_MAX_DURATION_MS     600000U     /* ten minutes */
#define SEQ_MIN_TOGGLE_MS       1U

typedef enum
{
    SEQ_STATE_IDLE = 0,
    SEQ_STATE_BLINK_ON,
    SEQ_STATE_BLINK_OFF,
    SEQ_STATE_COUNT
} seq_state_t;

typedef enum
{
    SEQ_OK = 0,
    SEQ_ERR_BUSY,               /* a sequence is already running    */
    SEQ_ERR_BAD_DURATION,
    SEQ_ERR_BAD_TOGGLE,
    SEQ_ERR_HARDWARE            /* the HAL refused to drive the LED */
} seq_status_t;

typedef enum
{
    SEQ_EVENT_NONE = 0,
    SEQ_EVENT_FINISHED,         /* the sequence ran to its end      */
    SEQ_EVENT_HARDWARE_FAILED   /* aborted: the LED stopped answering */
} seq_event_t;

typedef struct
{
    seq_state_t state;
    uint8_t     led_id;
    uint32_t    duration_ms;
    uint32_t    toggle_ms;
    uint32_t    elapsed_ms;
    uint32_t    remaining_ms;
    uint32_t    toggles;        /* LED changes so far, the first one included */
    uint32_t    missed_toggles; /* toggles skipped because a step came late   */
} seq_info_t;

/* Stops anything running and puts the state machine back to IDLE. */
void sequencer_init(void);

/*
 * Starts blinking `led_id`: on at once, then a change of state every
 * `toggle_ms`, until `duration_ms` have passed. The LED is left off.
 * Refuses a second sequence with SEQ_ERR_BUSY rather than silently
 * replacing the running one.
 */
seq_status_t sequencer_start_blink(uint8_t led_id, uint32_t duration_ms, uint32_t toggle_ms, uint32_t now_ms);

/* Aborts a running sequence and switches its LED off. False if nothing ran. */
bool sequencer_stop(void);

/* One non-blocking step. Call it on every pass of the main loop. */
seq_event_t sequencer_step(uint32_t now_ms);

bool sequencer_busy(void);
void sequencer_get_info(seq_info_t *info_out, uint32_t now_ms);

const char *sequencer_state_text(seq_state_t state);
const char *sequencer_status_text(seq_status_t status);

#endif /* SEQUENCER_H */
