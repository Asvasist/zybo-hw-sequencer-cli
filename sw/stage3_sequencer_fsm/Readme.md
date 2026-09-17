# Stage 3 - Non-blocking sequencer FSM and interrupt-driven UART TX

Status: **code complete. Compiles without warnings and links against the Vitis
2025.2 standalone BSP. Host unit tests and the host simulation pass. Board run
pending.**

Stage 2 could switch an LED on. This stage can run it on a schedule without
ever making the command line wait:

```
> LED_BLINK 2000 250
blinking LED 0 (LD4 on MIO7) every 250 ms for 2000 ms - the CLI stays open, STOP aborts
> STATUS                          <- answered while the LED is still blinking
Sequencer
  state          : blinking, LED off
  schedule       : every 250 ms for 2000 ms
  progress       : 751 ms elapsed, 1249 ms left, 4 changes, 0 missed
...
> STOP
sequence stopped
```

Two things had to stop blocking for that to be true:

- **The sequence.** A switch-case state machine, stepped from the main loop,
  that only ever does the work due at this instant and returns.
- **The console output.** Transmitting used to spin on the FIFO for about
  5 ms per response - long enough to make a 250 ms blink visibly late. It is
  now interrupt driven, so printing hands off in microseconds.

Exit criterion: `LED_BLINK 10000` keeps the LED blinking to schedule for ten
seconds while every command still answers, `STOP` aborts it, and no toggle
drifts or doubles up.

## What changed from stage 2

- New `sequencer.h/.c`: the state machine, its schedule and its rules.
- New `uptime_drv.h/.c`: milliseconds since boot from the Cortex-A9 global timer.
- `uart_driver`: a 1024-byte transmit ring, the TX-empty interrupt, and
  `uart_driver_flush()`. `uart_driver_write()` no longer waits, unless the
  application outruns the wire by a whole ring.
- `hal`: `HAL_GetUptimeMs()`, `HAL_GetUptimeUs()`, `HAL_TimeRunning()`, and
  `HAL_ERR_TIME_INIT` when the time base never started.
- `main.c`: steps the sequencer on every pass, runs `LED_BLINK` and `STOP`,
  reports a sequence that ends on its own, and shows both in `STATUS`.
- `parser.c`: `LED_BLINK` now documents its second argument as `[toggle_ms]`.
- Firmware version 0.3.0.

## Source layout

```
src/
  main.c             application: commands, formatting, the super-loop
  sequencer.h/.c     the state machine: schedule, phases, events
  hal.h/.c           the abstraction: LEDs, temperature, time
  gpio_drv.h/.c      PS MIO GPIO: LED pins (XGpioPs)
  xadc_drv.h/.c      die temperature (XAdcPs)
  uptime_drv.h/.c    milliseconds and microseconds since boot (global timer)
  uart_driver.h/.c   console UART: receive ISR, line queue, transmit ring
  parser.h/.c        command table, tokenizer, strict integer parsing
  board_zybo.h       the only board-specific file: base addresses and pins
tests/host/
  test_sequencer.c   the state machine, driven through simulated time
  test_uart_driver.c receive and transmit, against a model of the UART
  test_hal.c         HAL and drivers, including the time base
  test_parser.c      the parser
  sim_session.c      the whole firmware on a PC with a scripted session
  mock/              host stand-ins for the Xilinx headers, and the models
```

The layers are unchanged: the sequencer sits in the application layer, above
the HAL and below `main.c`. It includes no Xilinx header and no driver header,
and it never reads the clock itself - the caller passes the time in.

## The state machine

```
                sequencer_start_blink()
                        |
                        v
   +-------+  toggle due  +------------+  toggle due  +-------------+
   | IDLE  |              | BLINK_ON   | -----------> | BLINK_OFF   |
   |       |              |  LED on    | <----------- |  LED off    |
   +-------+              +------------+  toggle due  +-------------+
       ^                        |                            |
       |    duration elapsed / STOP / the LED stops answering |
       +------------------------+----------------------------+
```

`sequencer_step(now_ms)` is the whole of it: one switch, three cases, no loops
and no waiting. The state is also the LED phase, so a toggle is a plain write
of the next level rather than a read-modify-write of the pin.

**Deadlines are absolute.** `next_toggle_ms += toggle_ms` from the previous
deadline, not `now + toggle_ms`, so a step that arrives 3 ms late still toggles
on the original grid and the error does not accumulate. Over the full ten
minutes at a 1000 ms interval, every edge lands exactly 1000 ms after the last
one - checked by a test that walks all 600000 milliseconds.

**A late step resynchronises rather than catching up.** If the main loop is
held off for longer than a whole interval, firing the missed toggles back to
back would be a visible burst and would leave the phase wrong. Instead the
schedule is re-based on the current instant and the skipped deadlines are
counted, so `STATUS` shows them.

**The end of the duration wins** over a toggle that falls due at the same
millisecond, and the LED is always left off - including when the sequence ends
during an ON phase.

**One sequence at a time.** A second `LED_BLINK` is refused with "a sequence
is already running, STOP it first" rather than silently replacing the first.

**The clock is passed in.** Time comes from the caller, which keeps the file
free of hardware, and lets the tests run a ten-minute sequence or cross the
49.7-day millisecond wrap in microseconds. Every comparison is
`(int32_t)(now - deadline) >= 0`, so the wrap is a non-event; a test starts a
sequence 256 ms before it and follows the schedule across.

## Interrupt-driven transmit

```
   uart_driver_write()      ring (1024 bytes)        TX FIFO (64)      wire
        |                   +---------------+        +---------+
        +--- append ------> | head ... tail | -----> |         | ---->
                            +---------------+   ^    +---------+
                                                |         |
                         prime, if idle --------+         | FIFO empty
                         (main loop, TX masked)           v
                                                   uart_driver_isr()
                                                   refill, or mask off
```

**Who owns the transmit path is decided by the TX interrupt mask.** The ISR
moves bytes from the ring into the FIFO only while TX-empty is unmasked; the
main loop masks it for the handful of instructions it needs to prime an idle
transmitter, and unmasks it again if anything is still waiting. No critical
sections, no flags, and no case where both sides advance the tail.

**Priming is necessary, not an optimisation.** TX-empty is an event raised
when the FIFO *becomes* empty, not a level. Enabling the interrupt on an
already-empty FIFO would wait forever, so the first bytes are written by the
call itself. A test that removes the priming hangs, which is exactly what
would happen on the board.

**The interrupt is switched off when the ring runs dry**, otherwise an empty
FIFO would interrupt the processor for ever.

**Waiting only happens when the application outruns the wire.** The ring holds
1024 bytes, about six full responses. If it does fill, `uart_driver_write()`
waits - and keeps the transmitter fed from inside that wait, so it always
ends. Every wait is counted and shown in `STATUS`, so a "waits" figure above
zero is a signal that the output needs thinning, not a mystery.

**It still works with no interrupts at all.** Because the wait feeds the FIFO
itself, a firmware whose interrupt setup failed can still print - slowly, by
polling - which is what lets `main()` report that very failure. There is a
test for it.

## Timing

| Quantity | Value |
|----------|-------|
| Time base | Cortex-A9 global timer, 64 bits, half the CPU clock |
| Resolution as used | 1 ms for schedules, 1 us available for diagnostics |
| Millisecond stamp | 32 bits, wraps after 49.7 days, compared by difference |
| Blink interval | 1 ms to the sequence duration |
| Sequence duration | 1 ms to 600000 ms (ten minutes) |
| Main loop pass | a few microseconds, so a toggle lands within one pass of its deadline |

The conversion from timer ticks is exact - whole seconds first, remainder
second - so a millisecond stamp and a microsecond stamp taken from the same
tick always agree, and neither can overflow in any uptime this board will see.

Under the SDT BSP the xiltimer library only starts the global timer on the
first sleep call, so a firmware that never sleeps reads zero for ever.
`uptime_drv_init()` makes that one sleep at boot and then checks the counter
really advances; if it does not, `HAL_Init()` reports `HAL_ERR_TIME_INIT`, the
banner carries a warning and `LED_BLINK` refuses to start rather than running
a schedule that can never advance.

## Commands

| Command | Arguments | Behaviour |
|---------|-----------|-----------|
| `HELP` | | the command list, generated from the parser table |
| `STATUS` | | sequencer state and progress, LEDs, temperature, UART counters |
| `LED_SET` | `<led_id> <0\|1>` | switches an LED through the HAL |
| `LED_BLINK` | `<duration_ms> [toggle_ms]` | starts a sequence on LED 0, default 250 ms toggles |
| `READ_TEMP` | | reads the die temperature |
| `STOP` | | aborts the running sequence, or says nothing is running |

Ranges are checked where the command is executed, and again inside the
sequencer: 1 to 600000 ms of duration, a toggle interval of at least 1 ms and
at most the duration.

## Memory

All static, no heap. On top of stage 2:

| Object | Where | Size on the target |
|--------|-------|--------------------|
| transmit ring | `.bss` | 1024 bytes |
| transmit counters | `.bss` | 12 bytes |
| sequencer state | `.bss` | 32 bytes |

## Build and run (Vivado / Vitis 2025.2)

Same platform as stages 1 and 2 - a PS-only XSA with the board preset,
standalone on `ps7_cortexa9_0`. Nothing new is needed in the hardware design:
the global timer is inside the Cortex-A9.

1. Create the platform from the XSA and build it (stage 1 Readme, section 1-2).
2. Create an application component on it, then copy the twelve files from
   `src/` into the component's `src/` folder, next to the generated
   `CMakeLists.txt`, `UserConfig.cmake` and `lscript.ld`. Don't replace it.
   ```powershell
   Copy-Item "<repo>\sw\stage3_sequencer_fsm\src\*" -Include *.c,*.h -Destination "<workspace>\seq_cli\src" -Force
   ```
   Don't copy `tests/`.
3. Build, then run with a terminal at 115200 8N1, local echo off.

In the platform's BSP settings, leave the **xiltimer** sleep timer at its
default. The global timer is what `uptime_drv` reads, and the first sleep at
boot is what starts it.

## Tests on the host

No board and no BSP needed. From `tests/host`:

```
gcc -std=c11 -Wall -Wextra -Werror -DSDT -Imock -I../../src \
    test_sequencer.c mock/mock_board.c mock/mock_time.c ../../src/sequencer.c \
    ../../src/hal.c ../../src/gpio_drv.c ../../src/xadc_drv.c ../../src/uptime_drv.c -o test_sequencer
./test_sequencer

gcc -std=c11 -Wall -Wextra -Werror -DSDT -Imock -I../../src \
    test_uart_driver.c mock/mock_uart.c ../../src/uart_driver.c -o test_uart_driver
./test_uart_driver

gcc -std=c11 -Wall -Wextra -Werror -DSDT -Imock -I../../src \
    test_hal.c mock/mock_board.c mock/mock_time.c ../../src/hal.c ../../src/gpio_drv.c \
    ../../src/xadc_drv.c ../../src/uptime_drv.c -o test_hal
./test_hal

gcc -std=c11 -Wall -Wextra -Werror -I../../src test_parser.c ../../src/parser.c -o test_parser
./test_parser
```

Current results: `151` (sequencer), `8803` (UART), `4242` (HAL) and `313`
(parser) checks, none failing, with GCC 15.2 from MSYS2 UCRT64. The sequencer
and UART suites also pass under `-fsanitize=undefined`.

**What the models now cover.** `mock_uart.c` grew a 64-byte transmit FIFO with
the real TX-empty semantics (an event when the FIFO *becomes* empty), a
"wire" the test plays by hand, and an option to deliver interrupts the moment
they become pending. `mock_time.c` is the global timer: it moves only when the
test moves it, or when the code under test sleeps.

`test_sequencer.c` covers:
- an idle machine touching nothing, and a schedule followed toggle by toggle
- the sequence ending on time, from either phase, always leaving the LED off
- `STOP`, restart afterwards, and no late event from an aborted sequence
- a second start refused while one runs, with the first schedule untouched
- duration and toggle bounds, including both extremes
- an LED the board cannot drive, and one that stops answering mid-sequence
- a step arriving a whole interval late: one toggle, a resynchronised
  schedule, and the skipped deadlines counted
- a sequence started 256 ms before the 49.7-day wrap, followed across it
- ten minutes of blinking with every edge exactly one interval apart

`test_uart_driver.c` adds, on top of the stage 1 receive tests:
- the hand-off: the FIFO primed at once, the interrupt armed for the rest,
  and disarmed again when the ring runs dry
- more output than the ring holds: the wait, and not a byte lost or reordered
- `uart_driver_flush()` down to the shift register
- transmit and receive interleaved
- output still leaving the chip when the interrupt system failed to start
- the ISR leaving masked sources, and their status bits, alone

To check the tests catch real bugs, eleven defects were planted one at a time
in scratch copies. Every one was caught:

| Planted defect | Result |
|----------------|--------|
| toggle rescheduled from "now" instead of the grid | 1 check failed |
| no resynchronisation after a late step | 3 checks failed |
| the duration no longer ends the sequence | 18 checks failed |
| LED left on when the sequence ends | 1 check failed |
| a second start replaces the running sequence | 4 checks failed |
| deadlines compared unsigned (breaks at the wrap) | 5 checks failed |
| STOP leaves the LED on | 1 check failed |
| TX interrupt never disabled | 1 check failed |
| an idle transmitter is not primed | the suite hangs, as the board would |
| the ring overwrites itself when full | 3 checks failed |
| the ISR ignores the interrupt mask | 1 check failed |

The last one is worth a note. A model cannot reproduce an interrupt landing
between two instructions, so the race itself is not directly testable here;
what the test pins down is the rule that prevents it - the ISR only handles,
and only acknowledges, sources that are unmasked.

### The whole firmware on a PC

`sim_session.c` links the real `main.c`, sequencer, HAL and drivers against
all three models, types a scripted session, and prints every MIO pin change as
a `[board]` line. Simulated time advances as the firmware reads the clock, so
the blink runs while the session goes on:

```
gcc -std=c11 -Wall -Wextra -Werror -DSDT -Dmain=firmware_main -Imock -I../../src \
    sim_session.c mock/mock_uart.c mock/mock_board.c mock/mock_time.c \
    ../../src/main.c ../../src/sequencer.c ../../src/hal.c ../../src/gpio_drv.c \
    ../../src/xadc_drv.c ../../src/uptime_drv.c ../../src/parser.c ../../src/uart_driver.c -o sim_session
./sim_session
```

Output of the current code, trimmed:

```
> LED_BLINK 2000 250
             [board] MIO7 -> 1
blinking LED 0 (LD4 on MIO7) every 250 ms for 2000 ms - the CLI stays open, STOP aborts
             [board] MIO7 -> 0
             [board] MIO7 -> 1
             [board] MIO7 -> 0
> STATUS
Sequencer
  state          : blinking, LED off
  LED            : 0 (LD4 on MIO7)
  schedule       : every 250 ms for 2000 ms
  progress       : 751 ms elapsed, 1249 ms left, 4 changes, 0 missed
...
  bytes sent     : 1112 (ring peak 204 of 1024, 0 waits)
> READ_TEMP
Die temperature: 44.91 C
> LED_BLINK 500 100
error: a sequence is already running, STOP it first
             [board] MIO7 -> 1
             [board] MIO7 -> 0
> STOP
             [board] MIO7 -> 0
sequence stopped
...
> LED_BLINK 300 100
             [board] MIO7 -> 1
blinking LED 0 (LD4 on MIO7) every 100 ms for 300 ms - the CLI stays open, STOP aborts
             [board] MIO7 -> 0
             [board] MIO7 -> 1
             [board] MIO7 -> 0
sequence finished: 3 LED changes in 300 ms
>
```

The `[board]` lines between the responses are the whole point of the stage:
the LED keeps its schedule while `STATUS` and `READ_TEMP` are answered.

## Target build check

Built with the Vitis 2025.2 `arm-none-eabi-gcc` (13.3) and the Vitis flags
(`-DSDT -mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=hard`), at `-O0` and `-O2`,
with `-Wall -Wextra -Wshadow -Wconversion`. The project sources compile without
warnings; the only one in the build comes from the generated `xparameters.h`.
Linked against a 2025.2 standalone BSP for the Zybo Z7-20: 82.6 KB text,
3.4 KB data, 25.2 KB bss.

The layering is checked in the object files:

| Check | Result |
|-------|--------|
| `main.o` references a Xilinx or driver symbol | none |
| `sequencer.o` references a Xilinx or driver symbol | none (it goes through the HAL) |
| any application object references an allocator | none |

## Bring-up checklist

- [ ] Banner prints with no warning line (a warning means the GPIO, XADC or time base did not come up)
- [ ] `LED_BLINK 10000 250` blinks LD4 for ten seconds, and `STATUS`, `READ_TEMP` and `HELP` all answer while it runs
- [ ] The blink looks even, with no visible stutter while responses are printed
- [ ] `STATUS` during a sequence shows elapsed plus remaining equal to the duration, and 0 missed
- [ ] `STOP` aborts within a toggle and leaves LD4 off; a second `STOP` says nothing is running
- [ ] `LED_BLINK` while one runs is refused, and the running sequence is unaffected
- [ ] A sequence left to finish prints "sequence finished" with the expected number of changes
- [ ] `LED_BLINK 600001` and `LED_BLINK 1000 0` are refused with their ranges
- [ ] Time a `LED_BLINK 60000 1000` against a watch: roughly a minute, no drift worth seeing
- [ ] Paste twenty commands at once: `STATUS` shows the transmit ring peak below 1024 and 0 waits
- [ ] Everything from the stage 1 and 2 checklists still holds

## Known limitations (on purpose, or for a later stage)

- **One sequence, one LED.** `LED_BLINK` drives LED 0. Several sequences at
  once, or a table of steps per sequence, is the natural next shape for this
  file - the state machine is already the right place for it.
- **`LED_SET` and a running sequence share the LED.** Setting LED 0 by hand
  while a sequence blinks it will be overwritten at the next toggle. Ownership
  per LED is worth adding when more than one sequence can run.
- **Millisecond resolution.** The schedule is stepped from the main loop, so a
  toggle lands within one loop pass of its deadline - microseconds today, but
  it is not a hardware-timed edge. Stage 4 measures what the real latency is.
- **No flow control**, as in stage 1: a long pasted script can still outrun
  the loop. Dropped command lines are counted and reported.
- **SDT BSP flow only** (Vitis 2023.2 and later).

## Next: stage 4

The AXI Timer in the PL, and the measurement the project is named for: the
time from a command being fully received to the hardware reacting, in
microseconds, printed back to the terminal. That is the first stage that needs
a hardware design, so `hw/` arrives with it.
