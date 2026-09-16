# Stage 2 - Hardware abstraction layer

Status: **code complete. Compiles without warnings and links against the Vitis
2025.2 standalone BSP. Host unit tests and the host simulation pass. Board run
pending.**

Stage 1 turned received characters into parsed commands and printed them back.
This stage puts real hardware behind those commands, without letting the
hardware leak into the application:

```
> LED_SET 0 1
LED 0 (LD4 on MIO7) is now ON
> READ_TEMP
Die temperature: 44.91 C
```

`main.c` includes no Xilinx header and touches no register. It asks the HAL to
switch LED 0 on; the HAL knows LED 0 is an MIO pin and hands it to the GPIO
driver; the GPIO driver knows the pin number and talks to XGpioPs. Each layer
knows exactly one level below it.

Exit criterion: the application layer compiles with no Xilinx header in sight,
every LED and sensor operation reports a status instead of failing silently,
and the whole thing is testable on a PC with no board attached.

## Board resources used

Everything is on the PS side, so no bitstream is needed.

| Function | Zybo resource | Notes |
|----------|---------------|-------|
| Console | PS UART1, MIO48/49 | 115200 8N1, PROG/UART micro-USB (stage 1) |
| LED 0 | LD4, MIO7 | active high, output-only MIO pin |
| Die temperature | XADC via the PS-XADC interface | no PL logic involved |
| LEDs 1..4 | LD0..LD3 on PL pins | reserved ids, need the AXI GPIO in the PL design |

## Layers

```
   application        main.c            commands, formatting, the super-loop
        |                               no Xilinx header, no register access
        v
   abstraction        hal.h / hal.c     LED ids, temperature in milli-degrees
        |                               the board map lives here
        v
   drivers            gpio_drv  xadc_drv  uart_driver
        |                               pins, channels, FIFOs, interrupts
        v
   Xilinx BSP         XGpioPs   XAdcPs    XUartPs / XScuGic
```

Dependencies only point downwards, and `board_zybo.h` sits at the bottom: the
drivers take their pin numbers and base addresses from it, and nothing above
the driver layer includes it. Moving to another board is a change to that
header plus the driver tables, and the CLI is untouched.

## Source layout

```
src/
  main.c             application: commands, formatting, the super-loop
  hal.h/.c           the abstraction: LED map, LED control, temperature
  gpio_drv.h/.c      PS MIO GPIO: LED pins (XGpioPs)
  xadc_drv.h/.c      die temperature, sequencer setup, conversion (XAdcPs)
  uart_driver.h/.c   console UART: receive ISR, line queue, transmit (stage 1)
  parser.h/.c        command table, tokenizer, strict integer parsing (stage 1)
  board_zybo.h       the only board-specific file: base addresses and pins
tests/host/
  test_hal.c         HAL and driver tests against a model of the board
  test_parser.c      parser tests (stage 1)
  test_uart_driver.c UART driver tests (stage 1)
  sim_session.c      the whole firmware on a PC with a scripted session
  mock/              host stand-ins for the Xilinx headers, and the models
```

## What changed from stage 1

- New: `hal.*`, `gpio_drv.*`, `xadc_drv.*`, `board_zybo.h`, `test_hal.c`, and
  the board model `mock/mock_board.c` with its `xgpiops.h` / `xadcps.h`.
- `uart_driver.c` takes its base address from `board_zybo.h` instead of naming
  `XPAR_XUARTPS_0_BASEADDR` itself. That is the only change to stage 1 code.
- `parser.c` is untouched: the command table already described these commands.
- `main.c` executes `LED_SET` and `READ_TEMP` through the HAL instead of
  printing `Parsed Command: ...`. That line is still printed for `LED_BLINK`
  and `STOP`, which have nothing to run until the FSM arrives in stage 3.
- `STATUS` gained a hardware section: every LED with its state, and the
  temperature.
- Firmware version 0.2.0.

## The HAL

```c
hal_status_t HAL_Init(void);
uint8_t      HAL_LEDCount(void);
bool         HAL_LEDAvailable(uint8_t id);
const char  *HAL_LEDName(uint8_t id);
hal_status_t HAL_SetLED(uint8_t id, bool state);
hal_status_t HAL_GetLED(uint8_t id, bool *state_out);
hal_status_t HAL_ToggleLED(uint8_t id);
hal_status_t HAL_ReadTemperature(int32_t *milli_celsius_out);
const char  *HAL_StatusText(hal_status_t status);
```

`HAL_SetLED(uint8_t id, bool state)` is the signature from the project
specification, so the HAL keeps that naming at its boundary; the layers below
stay with the `module_action()` style of the rest of the code.

### LED map

| Id | Board LED | Driven by | In this build |
|----|-----------|-----------|---------------|
| 0 | LD4 on MIO7 | PS MIO GPIO | yes |
| 1..4 | LD0..LD3 on PL pins | AXI GPIO, later stage | reserved, reported as unavailable |

Ids are logical and stay put. A script written today against LED 0 keeps
working when the PL LEDs arrive, and asking for LED 2 now gets a clear answer
rather than a silent success:

```
> LED_SET 1 1
error: LED 1 (LD0 on PL): not available in this build, it needs the PL design
> LED_SET 9 1
error: no LED with id 9 (valid: 0..4)
```

`HAL_ERR_UNAVAILABLE` ("the id exists, the hardware doesn't yet") and
`HAL_ERR_BAD_ID` ("no such id at all") are deliberately different. Adding the
PL LEDs later is a change to the table in `hal.c` plus an AXI GPIO driver; no
caller changes.

## Design notes

**Errors travel, they never hide.** Every HAL call returns a status, and every
call is safe before `HAL_Init()` and with any argument - the result is a status
code, never a wild write. The application turns a status into a sentence with
`HAL_StatusText()`.

**Start-up policy.** The console UART comes up first, in `main()`, so that any
later failure can be reported to the terminal. `HAL_Init()` then brings up both
the LEDs and the sensor, always attempting both, so a dead sensor doesn't cost
you the LEDs. It returns the first failure; whatever did come up stays usable
and the rest answers `HAL_ERR_NOT_READY` when a command touches it. A hardware
failure is a warning at boot, not a halt: a command line that still answers is
more useful than a dead board.

**The console keeps its own driver.** `uart_driver` is already
hardware-independent at its API (take a line, write text), and the application
uses it directly. Wrapping it in `HAL_ConsoleWrite()` would add a layer that
only forwards. The rule that matters - the application includes no Xilinx
header and touches no register - holds either way, and is checked in the build
below.

**MIO7 needs two enables.** The pin is driven low, then set to output, then
output-enabled. With only the direction set, the pin stays high-impedance and
nothing lights, silently; driving the off level first stops the LED flashing
while the pin is configured. `gpio_drv_led_read()` reads the pin back rather
than returning a shadow variable, so `STATUS` shows the hardware, not what the
firmware believes.

**The XADC needs no bitstream.** The PS reaches the XADC hard macro over its
own serial link (XADCIF), so the temperature works on a PS-only design. The
sequencer is parked in safe mode while its channels and averaging are selected
- the Xilinx driver refuses those calls otherwise, and the host model enforces
the same rule - then left running continuously with 16x averaging and the
factory calibration on.

**No floating point.** UG480's `T = code * 503.975 / 4096 - 273.15` is done in
integer milli-degrees: `code * 503975 / 4096 - 273150`, rounded to nearest. The
worst case product is 2.06e9, inside `uint32_t`. The application prints it with
two decimals by splitting the integer, so printf never needs float support.

## Commands

| Command | Arguments | Stage 2 behaviour |
|---------|-----------|-------------------|
| `HELP` | | the command list, generated from the parser table |
| `STATUS` | | LED states, temperature, UART receive statistics |
| `LED_SET` | `<led_id> <0\|1>` | switches an LED through the HAL |
| `READ_TEMP` | | reads the die temperature through the HAL |
| `LED_BLINK` | `<duration_ms> [period_ms]` | parsed and reported; needs the FSM (stage 3) |
| `STOP` | | parsed and reported; nothing runs yet (stage 3) |

Argument ranges are checked where the command executes: the state must be 0 or
1, and the id must exist. The parser still only checks types and counts.

## Memory

All static, no heap. Beyond stage 1's line queue (688 bytes) and print buffer
(160 bytes), this stage adds the two driver instances and their tables:

| Object | Where | Size on the target |
|--------|-------|--------------------|
| XGpioPs instance | `.bss` | 68 bytes |
| XAdcPs instance | `.bss` | 12 bytes |
| LED map (5 entries) | `.rodata` | 40 bytes |

The GPIO pin table has one entry and the compiler folds it into the code at
`-O2`; the sizes above are measured in the linked image.

## Build and run (Vivado / Vitis 2025.2)

Same platform as stage 1 - a PS-only XSA with the board preset, standalone on
`ps7_cortexa9_0`. The XADC needs nothing extra in the hardware design: the PS
reaches it without any PL logic.

1. Create the platform from the XSA and build it (stage 1 Readme, section 1-2).
2. Create an application component on it, then copy the nine files from `src/`
   into the component's `src/` folder, next to the generated `CMakeLists.txt`,
   `UserConfig.cmake` and `lscript.ld`. Don't replace that folder.
   ```powershell
   Copy-Item "<repo>\sw\stage2_hal\src\*" -Include *.c,*.h -Destination "<workspace>\seq_cli\src" -Force
   ```
   Don't copy `tests/`.
3. Build, then run with a terminal at 115200 8N1, local echo off.

The sources need the SDT BSP flow (Vitis 2023.2 or later); on the classic flow
the drivers stop the build with an `#error` instead of guessing at the older
lookup API.

## Tests on the host

No board and no BSP needed. From `tests/host`:

```
gcc -std=c11 -Wall -Wextra -Werror -DSDT -Imock -I../../src \
    test_hal.c mock/mock_board.c ../../src/hal.c ../../src/gpio_drv.c ../../src/xadc_drv.c -o test_hal
./test_hal

gcc -std=c11 -Wall -Wextra -Werror -I../../src test_parser.c ../../src/parser.c -o test_parser
./test_parser

gcc -std=c11 -Wall -Wextra -Werror -DSDT -Imock -I../../src \
    test_uart_driver.c mock/mock_uart.c ../../src/uart_driver.c -o test_uart_driver
./test_uart_driver
```

Current results: `4222 checks, 0 failed` (HAL), `313 checks, 0 failed`
(parser), `8716 checks, 0 failed` (UART driver), with GCC 15.2 from MSYS2
UCRT64.

**The board model.** `hal.c`, `gpio_drv.c` and `xadc_drv.c` are compiled
unchanged. The headers in `mock/` stand in for `xgpiops.h` and `xadcps.h`, and
`mock_board.c` models what the drivers actually depend on: per-pin level,
direction and output enable, and the XADC's sequencer settings and result. It
also enforces the rules the real hardware and driver enforce - a pin write
before initialisation is counted as a fault, and selecting sequencer channels
outside safe mode is refused - so a driver that skips a step fails a test
instead of quietly working in the model.

`test_hal.c` covers:
- every call being safe before `HAL_Init()`: statuses, and not one pin write
- init: LD4 driven low *before* its output enable, direction and enable set,
  XADC self-tested, parked in safe mode for channel and averaging setup, then
  left in continuous mode with 16x averaging and calibration on
- each init failure on either side, and that the other side still works
- the LED map: ids, names, which are available, and bad ids
- set, read back and toggle, with the pin state checked in the model
- bad and unavailable ids refused without touching a pin
- temperature conversion against hand-computed values (0, 2048, 2585, 4095
  codes), monotonic across all 4096 codes, and a NULL argument
- every status having a description

To check the tests catch real bugs, nine defects were planted one at a time in
scratch copies. Every one made the suite fail:

| Planted defect | Checks failed |
|----------------|---------------|
| output enable never set | 2 |
| pin driven after the output was enabled, not before | 1 |
| LED id bounds check removed | 2 |
| unavailable LED falls through to the MIO driver | 2 |
| `HAL_Init()` hides a GPIO failure | 2 |
| sequencer not parked in safe mode | 4115 |
| raw XADC result not shifted to 12 bits | 5 |
| conversion truncates instead of rounding | 2 |
| wrong temperature offset constant | 5 |

The safe-mode one is worth a note: it first survived, because the model's
sequencer happened to start in safe mode, so the check proved nothing. The
model now starts the sequencer running, the way a warm restart leaves it, and
refuses the setup calls outside safe mode exactly as the Xilinx driver does.

### The whole firmware on a PC

`sim_session.c` links the real `main.c`, HAL and drivers against both models,
types a scripted session at every prompt, and prints each MIO pin change the
firmware makes:

```
gcc -std=c11 -Wall -Wextra -Werror -DSDT -Dmain=firmware_main -Imock -I../../src \
    sim_session.c mock/mock_uart.c mock/mock_board.c ../../src/main.c ../../src/hal.c \
    ../../src/gpio_drv.c ../../src/xadc_drv.c ../../src/parser.c ../../src/uart_driver.c -o sim_session
./sim_session
```

Output of the current code (banner and HELP left out; the model reports 44.91 C):

```
> READ_TEMP
Die temperature: 44.91 C
> LED_SET 0 1
             [board] MIO7 -> 1
LED 0 (LD4 on MIO7) is now ON
> led_set 0 0
             [board] MIO7 -> 0
LED 0 (LD4 on MIO7) is now OFF
> LED_SET 1 1
error: LED 1 (LD0 on PL): not available in this build, it needs the PL design
> LED_SET 9 1
error: no LED with id 9 (valid: 0..4)
> LED_SET 0 2
error: state must be 0 (off) or 1 (on), not 2
> LED_BLINK 10000 250
Parsed Command: LED_BLINK, Args: 10000, 250
not yet: timed sequences arrive with the FSM in stage 3
> STATUS
Hardware
  LED 0  LD4 on MIO7    ON
  LED 1  LD0 on PL      not available in this build, it needs the PL design
  ...
  Die temperature: 44.91 C
UART receive statistics
  bytes received : 151
  lines received : 14 (0 too long)
  lines dropped  : 0 (queue full)
  line errors    : overrun 0, framing 1, parity 0
  line queue     : 8 lines of up to 80 characters
```

The `[board]` lines come from the model, not the firmware: they are the pin
writes themselves. The first one appears during `HAL_Init()`, driving LD4 off.

## Target build check

Built with the Vitis 2025.2 `arm-none-eabi-gcc` (13.3) and the Vitis flags
(`-DSDT -mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=hard`), at `-O0` and `-O2`,
with `-Wall -Wextra -Wshadow -Wconversion`. The project sources compile without
warnings; the only one in the build comes from the generated `xparameters.h`
(`XPS_BOARD_ZYBO-Z7-20`). Linked against a 2025.2 standalone BSP for the Zybo
Z7-20: 78.4 KB text, 3.4 KB data, 24 KB bss.

The layering is checked in the object files, not just by reading the code:

| Check | Result |
|-------|--------|
| `main.c` includes any `x*.h` Xilinx header | none |
| `main.o` references XGpioPs / XAdcPs / XUartPs or a driver symbol | none |
| `hal.o` references any Xilinx symbol | none |
| Xilinx GPIO/XADC symbols referenced from | `gpio_drv.o` (6), `xadc_drv.o` (9) |

## Bring-up checklist

- [ ] Banner prints, no warning line after it (a warning means the GPIO or XADC didn't come up)
- [ ] `LED_SET 0 1` lights LD4, `LED_SET 0 0` turns it off
- [ ] LD4 is dark from reset until the first `LED_SET` - no flash during init
- [ ] `STATUS` shows LED 0 matching what the board actually looks like, after both on and off
- [ ] `READ_TEMP` is plausible (roughly 35-60 C on an idle board in a room)
- [ ] Temperature rises by a degree or two with a fingertip held on the Zynq
- [ ] `LED_SET 1 1` says "not available in this build"; `LED_SET 9 1` says "no LED with id 9"
- [ ] `LED_SET 0 2` is refused, and LD4 doesn't change
- [ ] Everything from the stage 1 checklist still holds (line endings, backspace, overlong lines, paste)

## Known limitations (on purpose, handled in later stages)

- **Nothing is timed yet.** `LED_BLINK` and `STOP` are parsed and reported.
  The non-blocking sequencer FSM is stage 3, and it is what makes
  `LED_BLINK 10000` run for ten seconds while the CLI keeps answering.
- **Transmit is still polled** and blocks the loop for a few milliseconds per
  response, as in stage 1.
- **Only one LED exists.** Ids 1..4 are reserved for LD0..LD3 and need the AXI
  GPIO in the PL design.
- **One temperature read per command.** Each `READ_TEMP` reads the XADC's
  latest averaged result; there is no sampling history, and no minimum or
  maximum tracking.
- **`HAL_ToggleLED()` is a read-modify-write** on the pin. Nothing else drives
  the LEDs today; once the FSM blinks in the background, ownership of an LED
  will need to be explicit.
- **SDT BSP flow only** (Vitis 2023.2 and later).

## Next: stage 3

The sequencer state machine: a switch-case FSM stepped from the main loop, so
`LED_BLINK 10000 250` toggles LD4 every 250 ms for ten seconds while the CLI
keeps accepting and answering commands, and `STOP` aborts it. That needs a
millisecond time base and non-blocking UART transmit, which arrive with it.
