/*
 * sequencer.c
 *
 * The sequencer state machine. See sequencer.h for the model and the rules.
 */
#include "sequencer.h"

#include <stddef.h>

#include "hal.h"

typedef struct
{
    seq_state_t state;
    uint8_t     led_id;
    uint32_t    duration_ms;
    uint32_t    toggle_ms;
    uint32_t    start_ms;
    uint32_t    end_ms;         /* absolute deadline for the whole sequence */
    uint32_t    next_toggle_ms; /* absolute deadline for the next change    */
    uint32_t    toggles;
    uint32_t    missed_toggles;
} sequencer_t;

static sequencer_t s_seq;

/*
 * Wrap-safe "is this deadline in the past?". The subtraction is done in
 * unsigned arithmetic and read as signed, so it stays right across the
 * 49.7-day wrap as long as deadlines are less than ~24 days away, which
 * SEQ_MAX_DURATION_MS (ten minutes) guarantees.
 */
static bool deadline_passed(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static void sequencer_go_idle(void)
{
    s_seq.state = SEQ_STATE_IDLE;
}

/* Drives the LED to the level the next state stands for. */
static seq_status_t sequencer_enter_phase(seq_state_t phase)
{
    const bool level = (phase == SEQ_STATE_BLINK_ON);

    if (HAL_SetLED(s_seq.led_id, level) != HAL_OK)
    {
        return SEQ_ERR_HARDWARE;
    }

    s_seq.state = phase;
    s_seq.toggles++;

    return SEQ_OK;
}

void sequencer_init(void)
{
    if (s_seq.state != SEQ_STATE_IDLE)
    {
        (void)HAL_SetLED(s_seq.led_id, false);
    }

    s_seq.state          = SEQ_STATE_IDLE;
    s_seq.led_id         = 0U;
    s_seq.duration_ms    = 0U;
    s_seq.toggle_ms      = 0U;
    s_seq.start_ms       = 0U;
    s_seq.end_ms         = 0U;
    s_seq.next_toggle_ms = 0U;
    s_seq.toggles        = 0U;
    s_seq.missed_toggles = 0U;
}

seq_status_t sequencer_start_blink(uint8_t led_id, uint32_t duration_ms, uint32_t toggle_ms, uint32_t now_ms)
{
    seq_status_t status;

    if (s_seq.state != SEQ_STATE_IDLE)
    {
        return SEQ_ERR_BUSY;
    }
    if ((duration_ms < SEQ_MIN_DURATION_MS) || (duration_ms > SEQ_MAX_DURATION_MS))
    {
        return SEQ_ERR_BAD_DURATION;
    }
    if ((toggle_ms < SEQ_MIN_TOGGLE_MS) || (toggle_ms > duration_ms))
    {
        return SEQ_ERR_BAD_TOGGLE;
    }

    s_seq.led_id         = led_id;
    s_seq.duration_ms    = duration_ms;
    s_seq.toggle_ms      = toggle_ms;
    s_seq.start_ms       = now_ms;
    s_seq.end_ms         = now_ms + duration_ms;
    s_seq.next_toggle_ms = now_ms + toggle_ms;
    s_seq.toggles        = 0U;
    s_seq.missed_toggles = 0U;

    /* Switching on is the first phase, and it happens now, not at the first step. */
    status = sequencer_enter_phase(SEQ_STATE_BLINK_ON);
    if (status != SEQ_OK)
    {
        sequencer_go_idle();
    }

    return status;
}

bool sequencer_stop(void)
{
    if (s_seq.state == SEQ_STATE_IDLE)
    {
        return false;
    }

    (void)HAL_SetLED(s_seq.led_id, false);
    sequencer_go_idle();

    return true;
}

seq_event_t sequencer_step(uint32_t now_ms)
{
    switch (s_seq.state)
    {
    case SEQ_STATE_IDLE:
        break;

    case SEQ_STATE_BLINK_ON:
    case SEQ_STATE_BLINK_OFF:
        /* The end of the sequence wins over a toggle that falls due together with it. */
        if (deadline_passed(now_ms, s_seq.end_ms))
        {
            (void)HAL_SetLED(s_seq.led_id, false);
            sequencer_go_idle();
            return SEQ_EVENT_FINISHED;
        }

        if (deadline_passed(now_ms, s_seq.next_toggle_ms))
        {
            const seq_state_t next = (s_seq.state == SEQ_STATE_BLINK_ON) ? SEQ_STATE_BLINK_OFF
                                                                         : SEQ_STATE_BLINK_ON;

            if (sequencer_enter_phase(next) != SEQ_OK)
            {
                (void)HAL_SetLED(s_seq.led_id, false);
                sequencer_go_idle();
                return SEQ_EVENT_HARDWARE_FAILED;
            }

            s_seq.next_toggle_ms += s_seq.toggle_ms;

            /*
             * Still in the past means a step arrived a whole interval late.
             * Count the toggles that were missed and resynchronise, instead
             * of firing them back to back on the next few passes.
             */
            if (deadline_passed(now_ms, s_seq.next_toggle_ms))
            {
                s_seq.missed_toggles += ((now_ms - s_seq.next_toggle_ms) / s_seq.toggle_ms) + 1U;
                s_seq.next_toggle_ms  = now_ms + s_seq.toggle_ms;
            }
        }
        break;

    case SEQ_STATE_COUNT:
    default:
        sequencer_go_idle();
        break;
    }

    return SEQ_EVENT_NONE;
}

bool sequencer_busy(void)
{
    return s_seq.state != SEQ_STATE_IDLE;
}

void sequencer_get_info(seq_info_t *info_out, uint32_t now_ms)
{
    if (info_out == NULL)
    {
        return;
    }

    info_out->state          = s_seq.state;
    info_out->led_id         = s_seq.led_id;
    info_out->duration_ms    = s_seq.duration_ms;
    info_out->toggle_ms      = s_seq.toggle_ms;
    info_out->toggles        = s_seq.toggles;
    info_out->missed_toggles = s_seq.missed_toggles;

    if (s_seq.state == SEQ_STATE_IDLE)
    {
        info_out->elapsed_ms   = 0U;
        info_out->remaining_ms = 0U;
        return;
    }

    info_out->elapsed_ms   = now_ms - s_seq.start_ms;
    info_out->remaining_ms = deadline_passed(now_ms, s_seq.end_ms) ? 0U : (s_seq.end_ms - now_ms);
}

const char *sequencer_state_text(seq_state_t state)
{
    switch (state)
    {
    case SEQ_STATE_IDLE:      return "idle";
    case SEQ_STATE_BLINK_ON:  return "blinking, LED on";
    case SEQ_STATE_BLINK_OFF: return "blinking, LED off";
    case SEQ_STATE_COUNT:     break;
    }

    return "unknown state";
}

const char *sequencer_status_text(seq_status_t status)
{
    switch (status)
    {
    case SEQ_OK:               return "ok";
    case SEQ_ERR_BUSY:         return "a sequence is already running, STOP it first";
    case SEQ_ERR_BAD_DURATION: return "duration out of range";
    case SEQ_ERR_BAD_TOGGLE:   return "toggle interval out of range";
    case SEQ_ERR_HARDWARE:     return "the LED could not be driven";
    }

    return "unknown sequencer status";
}
