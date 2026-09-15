# Stage 1 - Interrupt-driven UART command parser

Status: **code complete. Compiles without warnings and links against the Vitis
2025.2 standalone BSP. Host unit tests and the host simulation pass. Board run
pending.**

The CLI needs one thing before anything else: reliable command lines from the
UART that don't block the rest of the firmware. This stage builds only that.
There is no HAL, no sequencer FSM and no AXI Timer yet, so every command is
parsed and confirmed, and nothing drives hardware:

```
> LED_BLINK 500
Parsed Command: LED_BLINK, Arg: 500
```

Exit criterion: every line typed or pasted into the terminal is either parsed
and confirmed, rejected with a reason, or counted as dropped. Nothing is
silently lost, merged with another line, or executed half-received, and there
is no polling for received characters anywhere.

## Board resources used

Everything is on the PS side, so no bitstream is needed.

| Function | Zybo resource | Notes |
|----------|---------------|-------|
| Console | PS UART1, MIO48/49 | 115200 8N1, PROG/UART micro-USB |
| UART interrupt | GIC SPI 50 (ID 82), level | from the SDT config table, not hard-coded |

## Source layout

```
src/
  main.c           super-loop: take a line, parse it, print the result
  uart_driver.h/.c UART init, receive ISR, line queue, polled transmit
  parser.h/.c      command table, tokenizer, strict integer parsing
tests/host/
  test_parser.c        parser unit tests
  test_uart_driver.c   driver unit tests, the real ISR against a UART model
  sim_session.c        the whole firmware on a PC with a scripted terminal session
  mock/                XUartPs / interrupt wrapper / register model for the host
```

The dependencies point one way: `main.c` uses `parser.h` and `uart_driver.h`.
The parser knows nothing about UARTs, and the driver knows nothing about
commands. `main.c` includes no Xilinx header, and only `uart_driver.c` touches
the UART.

## How the ISR talks to the main loop

```
            RX pin
              |
     +--------v---------+  one interrupt per character (trigger level 1)
     |  UART RX FIFO    |
     +--------+---------+
              | uart_driver_isr(): ack status, drain FIFO
              v
     rx_process_byte()             ISR context, O(1) per byte
       CR / LF / CR LF  -> commit line
       BS / DEL         -> remove last char
       > 80 chars       -> flag TOO_LONG, keep first 80
       line error       -> flag RX_ERROR
              |
              v
     +--------------------------------------------------+
     | line queue: 8 x uart_line_t (static, .bss)       |
     |                                                  |
     |  [taken ...... committed)   [committed]          |
     |   complete lines, owned      line being built,   |
     |   by the main loop           owned by the ISR    |
     +-------------------------+------------------------+
       s_lines_committed++     |      s_lines_taken++
       (ISR only)              |      (main loop only)
                               v
     main loop: uart_driver_line_ready()  -> committed != taken
                uart_driver_take_line()   -> copy line, taken++
                parser_parse()            -> command + args
                print result
```

**One writer per shared variable.** The "a command is ready" flag is the
commit counter. Only the ISR increments it. The main loop owns a second
counter of lines taken, and a line is ready while the two differ. There's no
shared boolean that both sides write, and so no clear-versus-set race. Nothing
masks interrupts, and ten lines arriving back to back are ten events, not one.
Both counters are 32-bit, so every read and write is a single atomic
instruction on the A9. They wrap freely, and their difference stays correct.

**Slot ownership.** The ISR writes only the slot at `committed`, and only while
`committed - taken` is below the queue depth. The main loop reads only slots in
`[taken, committed)`. Those ranges can't overlap, so neither side ever sees a
slot the other is halfway through writing. A slot changes owner at exactly one
point, the ISR's `s_lines_committed++`. The queue, the counters and the
statistics are `volatile`. The line assembly state is used only by the ISR once
interrupts are enabled, so it doesn't need to be.

**The ISR only buffers.** Per byte it does one comparison chain and one store.
Parsing, number conversion, lookups and printing all happen in the main loop.

**Acknowledge, then drain.** The ISR clears the interrupt status *before* it
empties the FIFO. If it cleared afterwards, a character landing between the
final "FIFO empty" check and the clear would lose its interrupt and sit unseen
until the next keypress. If that character were the line ending, the command
would never run. `test_ack_before_drain` delivers a byte at exactly that moment.
A version with the order swapped fails it.

**Trigger level 1, no receive timeout.** The UART interrupts on every
character. At 115200 baud that's at most 11.5 kHz, which is trivial for the A9,
and the line ending is seen as soon as its stop bit arrives. A higher watermark
would hold the end of a command in the FIFO until the receive timeout expired.
That latency would count against the measurement stage 4 is for. With a
trigger level of 1 the receive timeout adds nothing, so it is disabled.

## Line discipline and overrun protection

| Input | Result |
|-------|--------|
| `CR`, `LF` or `CR LF` | ends one line. An `LF` right after a `CR` is swallowed, even in a separate interrupt |
| `LF CR`, `CR CR`, `LF LF` | two line endings |
| empty line | delivered, and the main loop just prints a new prompt |
| Backspace (0x08), DEL (0x7F) | removes the last character, nothing happens on an empty line |
| more than 80 characters | the rest is discarded, the line is delivered as `UART_LINE_TOO_LONG` |
| framing / parity / FIFO overrun | the line is delivered as `UART_LINE_RX_ERROR` |
| queue full when a line starts | the whole line is dropped and counted in `lines_dropped` |
| anything else, control bytes included | stored as-is. The parser rejects them with their position |

Overrun is handled at three levels, and none of them can write out of bounds:

1. **UART FIFO (64 bytes)** overflows only if the ISR is held off for about 5 ms.
   The overrun interrupt marks the line, which is then rejected.
2. **Line buffer (80 characters)**: the length is checked before every store.
   `text[80]` is reserved for the NUL.
3. **Line queue (8 lines)**: room is checked once, when a line *starts*. A line
   that starts while the queue is full stays dropped, even if the main loop
   frees a slot halfway through. Otherwise it would be delivered without its
   beginning. `test_line_started_while_full_stays_dropped` covers this case.

The main loop prints a warning with the number of dropped lines after each
batch. `STATUS` shows all the counters.

## Parser

```
line     = [blank] [keyword {blank argument}] [blank]
keyword  = a name from the command table, case-insensitive
argument = ["+" | "-"] digit {digit}       decimal, must fit in int32_t
blank    = one or more spaces or tabs
```

- **Table-driven.** `cmd_id_t` indexes a `const` table of keyword, usage,
  summary and min/max argument count. A build-time check fails if the enum and
  the table disagree in size. HELP is generated from the table, so adding a
  command is one enum value and one table row.
- **Works on (pointer, length).** It never writes to the line and never needs
  a NUL. It keeps no state between calls: no `strtok()` (hidden static state,
  modifies its input) and no `strtol()` (skips whitespace, accepts `0x`,
  reports overflow through `errno`).
- **Bytes are validated first.** Anything outside printable ASCII or tab is
  rejected before tokenizing, with its column. An arrow key (`ESC [ A`) can't
  turn into a strange command.
- **Keywords match whole tokens.** `LED` and `LED_SETX` are not `LED_SET`.
- **Integers are strict.** An optional sign plus digits, nothing else.
  Overflow is detected before each multiply, so `2147483648` and `4294967306`
  are rejected rather than wrapped. `-2147483648` is accepted.
- **`args[]` can't overflow**, even if a table row claimed more arguments than
  `PARSER_MAX_ARGS`. The parser clamps to the array size.
- **Errors name the offending token** (offset and length into the line), and
  on argument errors the command is still reported so the usage can be shown.

The parser checks argument *counts* only. Ranges such as LED id 0-3 belong to
the code that executes the command, which arrives in stages 2 and 3.

## Memory

All static. The code calls no `malloc`, `free`, `calloc` or `realloc`, and
the heap is not used anywhere.

| Object | Where | Size on the target |
|--------|-------|--------------------|
| line queue, 8 x `uart_line_t` | `.bss` | 688 bytes |
| receive statistics | `.bss` | 28 bytes |
| print format buffer | `.bss` | 160 bytes |
| command table | `.rodata` | 96 bytes |
| line copy, parser result, echo buffer | main loop stack | ~200 bytes |

Output is formatted with `vsnprintf()` into the static buffer. With the integer
and string conversions used here, newlib doesn't allocate. The linked ELF does
contain newlib's `_malloc_r`, but the BSP's C runtime pulls it in: a probe build
with no formatting code at all links it too. Nothing in this application
references it.

## Commands

| Command | Arguments | Stage 1 behaviour |
|---------|-----------|-------------------|
| `HELP` | | confirmation, then the command list |
| `STATUS` | | confirmation, then the UART receive statistics |
| `LED_SET` | `<led_id> <0\|1>` | confirmation only |
| `LED_BLINK` | `<duration_ms> [period_ms]` | confirmation only |
| `READ_TEMP` | | confirmation only |
| `STOP` | | confirmation only |

Keywords are case-insensitive. HELP and STATUS already run because they only
involve the CLI itself.

## Build and run (Vivado / Vitis 2025.2)

### 1. Hardware platform

Stage 1 needs only the PS with the board preset. The PL design for this
project arrives with the AXI Timer. Any Zybo / Zybo Z7 XSA with PS UART1
enabled works. To make one:

1. Vivado: **Create Project** > RTL project, no sources > **Boards** tab >
   your Zybo board > Finish.
2. **Create Block Design** > add **ZYNQ7 Processing System** > **Run Block
   Automation** with *Apply Board Preset* ticked.
3. Double-click the Zynq block > *PS-PL Configuration* > untick **M AXI GP0**
   (nothing is connected to it yet) > OK.
4. **Validate Design** > **Create HDL Wrapper** > **Generate Output Products**.
5. File > Export > **Export Hardware** > *Pre-synthesis* > save the `.xsa`.

### 2. Platform and application in Vitis

1. File > New Component > **Platform** > name `seq_platform` > select the XSA >
   OS **standalone**, processor **ps7_cortexa9_0** > Finish > **Build**.
2. File > New Component > **Application** > name `seq_cli` > platform
   `seq_platform` > domain `standalone_ps7_cortexa9_0` > Finish.
3. Copy the five files from `src/` into the component's `src/` folder, next to
   the generated `CMakeLists.txt`, `UserConfig.cmake` and `lscript.ld`. Don't
   replace the folder. From PowerShell:
   ```powershell
   Copy-Item "<repo>\sw\stage1_uart_parser\src\*" -Include *.c,*.h -Destination "<workspace>\seq_cli\src" -Force
   ```
   Don't copy `tests/`.
4. Select `seq_cli` > **Build**. The only warning comes from the generated
   `xparameters.h` (`XPS_BOARD_ZYBO-Z7-20`, harmless).

The sources need the SDT BSP flow (Vitis 2023.2 or later). On the classic flow,
`uart_driver.c` stops the build with an `#error` rather than guessing at the
older lookup API.

### 3. Board

1. Boot mode jumper **JP5 to JTAG**, micro-USB into **PROG/UART**, power on.
2. Terminal on the board's COM port: **115200 8N1, no flow control**. Leave
   local echo **off**. The firmware echoes each received line after the prompt.
3. Select `seq_cli` > **Run**.

The terminal can send CR, LF or CR LF for Enter. All three work.

**Scripts** should wait for the `> ` prompt before sending the next line, as
pyserial/pexpect-style tools normally do. The responses are several times
longer than the commands, and transmit is still blocking, so a burst of more
than about eight lines outruns the main loop. The extra lines are dropped and
reported, never half-run.

## Tests on the host

No board and no BSP needed, just any C11 compiler. From `tests/host`:

```
gcc -std=c11 -Wall -Wextra -Werror -I../../src test_parser.c ../../src/parser.c -o test_parser
./test_parser

gcc -std=c11 -Wall -Wextra -Werror -DSDT -Imock -I../../src test_uart_driver.c mock/mock_uart.c ../../src/uart_driver.c -o test_uart_driver
./test_uart_driver
```

Current result: `313 checks, 0 failed` and `8716 checks, 0 failed` (GCC 15.2,
MSYS2 UCRT64). Both suites also pass with `-fsanitize=undefined`.

**The hardware mock.** `uart_driver.c` is compiled unchanged. The headers in
`mock/` stand in for `xuartps.h`, `xparameters.h` and `xinterrupt_wrap.h`, and
route register access to a model of the Zynq UART in `mock_uart.c`. The model
has a 64-byte RX FIFO, write-one-to-clear interrupt status, a level-sensitive
interrupt line and a TX capture buffer. The tests drop bytes onto the RX pin
and play the interrupt controller. There are no `#ifdef TEST` blocks in the
production code.

`test_parser.c` covers:
- the command table: complete, upper-case keywords, argument counts within `PARSER_MAX_ARGS`
- every command at its minimum and maximum argument count, and the spec examples
- case-insensitive keywords, tabs and extra blanks
- empty lines, unknown keywords, prefixes and extensions of real keywords
- too few and too many arguments, including far more tokens than `args[]` holds
- integer limits, overflow (including values that would wrap to a valid number), malformed numbers
- control bytes, non-ASCII and embedded NULs rejected with their position; `length` honoured
- 200 000 random int32 round trips, and 200 000 fuzzed lines checked against the result invariants

`test_uart_driver.c` covers:
- init: 8N1 at 115200, trigger level 1, timeout off, RX sources enabled, SDT interrupt ID and parent, boot junk flushed
- each init failure, including interrupt setup failing with TX still usable
- CR / LF / CR LF, including the LF arriving in its own interrupt; byte-per-interrupt and burst delivery
- backspace and DEL, overlong lines (exactly 80 is fine, 81 is not)
- full queue dropping whole lines, including a line that starts while the queue is full
- framing, parity and FIFO overrun errors flagging the right line, and error outranking "too long"
- 1000 lines through the queue in order, with the slot indexes wrapping
- the late byte arriving right after the ISR's final look
- LF to CR LF translation on transmit

To check that the tests catch real bugs, 12 defects were planted one at a time
in scratch copies of the sources. Every one made a suite fail:

| Planted defect | Checks failed |
|----------------|---------------|
| ISR acknowledges after draining | 4 |
| no CR LF folding | 13 |
| queue room check `<=` instead of `<` | 7 |
| line length check `<=` instead of `<` | 4 |
| line errors ignored | 4 |
| line without a slot stored anyway | 9 |
| boot junk not flushed | 2 |
| no integer overflow check | 15 |
| keyword prefix accepted | 6 |
| surplus argument check off by one | 7 |
| control characters allowed | 4 |
| wrong `INT32_MIN` conversion | 1 |

### The whole firmware on a PC

`sim_session.c` links the real `main.c`, `parser.c` and `uart_driver.c` against
the same model. It types a scripted session whenever the prompt appears and
renders the output the way a terminal would:

```
gcc -std=c11 -Wall -Wextra -Werror -DSDT -Dmain=firmware_main -Imock -I../../src \
    sim_session.c mock/mock_uart.c ../../src/main.c ../../src/parser.c ../../src/uart_driver.c -o sim_session
./sim_session
```

Output of the current code (the banner and HELP are left out):

```
> LED_BLINK 500
Parsed Command: LED_BLINK, Arg: 500
> led_set 2 1
Parsed Command: LED_SET, Args: 2, 1
> LED_BLINK 10000 250
Parsed Command: LED_BLINK, Args: 10000, 250
> READ_TEMP
Parsed Command: READ_TEMP, Args: none
> LED_SET 0 0
Parsed Command: LED_SET, Args: 0, 0
>
> LED_BLINK
error: too few arguments
usage: LED_BLINK <duration_ms> [period_ms]
> READ_TEMP now
error: too many arguments: 'now'
usage: READ_TEMP
> LED_BLINK 12abc
error: argument is not a 32-bit decimal integer: '12abc'
usage: LED_BLINK <duration_ms> [period_ms]
> LED_SET 1 99999999999
error: argument is not a 32-bit decimal integer: '99999999999'
usage: LED_SET <led_id> <0|1>
> FLASH_LED 3
error: unknown command 'FLASH_LED', type HELP for the list
> .[A
error: invalid character 0x1B at column 1
> LED_BLINK 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 2
error: line longer than 80 characters, discarded
> STOP
error: receive error (framing, parity or overrun), line discarded
> LED_SET 0 1
Parsed Command: LED_SET, Args: 0, 1
...                                   (a pasted script of 10 lines, 8 fit in the queue)
> LED_SET 1 0
Parsed Command: LED_SET, Args: 1, 0
warning: 2 line(s) dropped, receive queue was full
> STATUS
Parsed Command: STATUS, Args: none
UART receive statistics
  bytes received : 373
  lines received : 24 (1 too long)
  lines dropped  : 2 (queue full)
  line errors    : overrun 0, framing 1, parity 0
  line queue     : 8 lines of up to 80 characters
>
```

`LED_SEX<BS>T 0 0` was typed with a backspace, and it arrives as `LED_SET 0 0`.
The `STOP` was sent with a framing error on its first byte.

## Target build check

Built with the Vitis 2025.2 `arm-none-eabi-gcc` (13.3) and the Vitis flags
(`-DSDT -mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=hard`), at `-O0` and `-O2`,
with `-Wall -Wextra -Wshadow -Wconversion`. The project sources compile without
warnings. The image links against a 2025.2 standalone BSP for the Zybo Z7-20:
64.6 KB text, 3.4 KB data, 24 KB bss (mostly the BSP's stacks and tables).

## Bring-up checklist

- [ ] Banner prints cleanly after the boot output, followed by the prompt
- [ ] `HELP` + Enter lists the commands. One response per Enter with the terminal sending CR, then LF, then CR LF
- [ ] `LED_BLINK 500` prints `Parsed Command: LED_BLINK, Arg: 500`
- [ ] Backspace fixes a typo before Enter
- [ ] An 81-character line gives `line longer than 80 characters`, and the next line works
- [ ] Arrow keys then Enter give `invalid character 0x1B`, and the terminal isn't disturbed
- [ ] Breakpoint in `uart_driver_isr`: hit once per typed character. No code reads the RX FIFO anywhere else
- [ ] Paste 20 short lines at once: a warning with the dropped count, and in `STATUS` the lines received plus dropped equal the lines sent
- [ ] Terminal at 57600 baud, type a few keys, back to 115200: `STATUS` framing errors went up, and the CLI works without a reset
- [ ] After normal use `STATUS` shows 0 errors and 0 dropped lines

## Known limitations (on purpose, handled in later stages)

- **Transmit is polled.** A typical response (echo plus confirmation, ~60
  characters) blocks the main loop for about 5 ms at 115200 baud. Reception
  keeps going underneath, which is the point of this stage, but a non-blocking
  sequencer can't afford the wait. Interrupt-driven transmit with a ring buffer
  comes with the FSM.
- **No flow control.** Output is longer than input, so a long pasted script
  outruns the loop. Lines are dropped whole and counted.
- **Error attribution assumes one byte per interrupt.** The UART's error bits
  aren't tied to a specific byte. With a trigger level of 1 the damaged byte is
  alone in the FIFO when the ISR runs. If interrupts were ever held off long
  enough for several bytes to queue, an error could be charged to the
  neighbouring line.
- **Hardware commands are parsed only.** Their argument ranges are checked
  where they execute, from stage 2 on.
- **SDT BSP flow only** (Vitis 2023.2 and later).

## Next: stage 2

A hardware abstraction layer, starting with `HAL_SetLED(uint8_t id, bool state)`.
`main.c` and the future FSM will call the HAL and never touch a register, and
the HAL gets the same host mock treatment as the UART driver.
