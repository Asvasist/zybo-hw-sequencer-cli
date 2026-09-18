# Zybo Event-Driven Hardware Sequencer (CLI)

A bare-metal command line for the Digilent Zybo (Zynq-7000) that behaves like a
small automated test rig. Text commands arrive over the UART, such as
`LED_BLINK 10000` or `READ_TEMP`, and the firmware runs the matching sequence
of hardware events at millisecond precision. The CLI stays responsive while a
sequence runs. An AXI Timer in the PL measures how long the hardware took to
react after a command was fully received, and the result goes back to the
terminal (`Execution time: 142 us`).

What it covers:

- an interrupt-driven UART driver with lock-free hand-off from the ISR to the main loop
- a safe, table-driven command parser with static memory only
- a hardware abstraction layer, so the application never touches a register
- a non-blocking switch-case state machine for command execution
- microsecond latency measurement with the Zynq AXI Timer
- host-side unit tests against mocked hardware

## Stages

Firmware first, on the PS alone. The PL design arrives with the AXI Timer, and
its exported `.xsa` is committed, so the repository is enough on its own.

| Stage | Scope | Folder | Status |
|-------|-------|--------|--------|
| 1 | Interrupt-driven UART RX, line queue, command parser | `sw/stage1_uart_parser` | Code complete, builds against the 2025.2 BSP, host tests pass, board run pending |
| 2 | Hardware abstraction layer: LEDs and die temperature behind `HAL_SetLED()` and friends | `sw/stage2_hal` | Code complete, builds against the 2025.2 BSP, host tests pass, board run pending |
| 3 | Non-blocking sequencer FSM, interrupt-driven UART TX | `sw/stage3_sequencer_fsm` | Code complete, builds against the 2025.2 BSP, host tests pass, board run pending |
| 4 | PL design: AXI Timer and AXI GPIO for the PL LEDs, exported XSA | `hw/` | Hardware built and exported (timing met); the firmware that uses it is next |

Each stage has its own Readme covering the design, the build steps, the tests
and a bring-up checklist. Stage folders are kept as they were when completed,
so each step can be built and read on its own.

## Quick start (stage 3, the latest)

1. **XSA.** Use the one in the repository: `hw/export/zybo_seq_hw.xsa`, built
   for the Zybo Z7-20 with the bitstream inside. No Vivado needed. Another
   board, or a change to the PL: `hw/Readme.md` has the script and the
   click-by-click steps.
2. **Vitis 2025.2.** Create a standalone platform for `ps7_cortexa9_0`, then an
   empty application on it.
3. **Sources.** Copy the `.c`/`.h` files from `sw/stage3_sequencer_fsm/src`
   into the application's `src/` folder, build, and run. Open a terminal at
   115200 8N1, type `HELP`, then `LED_BLINK 10000 250` - and keep typing
   commands while LD4 blinks.

No board yet? `sw/stage3_sequencer_fsm/tests/host/sim_session.c` runs the same
firmware on a PC against models of the UART, the board and the clock.

## Repository layout

```
hw/
  Readme.md            what the PL design contains, how to rebuild it, how to use the XSA
  scripts/             Vivado script that builds the design and exports the XSA
  src/constraints/     the PL LED pins
  export/              the exported .xsa, bitstream included - tracked on purpose
sw/
  stage1_uart_parser/
    Readme.md            design, build and run, tests, bring-up checklist
    src/
      main.c             super-loop: take a line, parse it, print the result
      uart_driver.h/.c   UART init, receive ISR, line queue, polled transmit
      parser.h/.c        command table, tokenizer, strict integer parsing
    tests/host/
      test_parser.c      parser unit tests
      test_uart_driver.c driver unit tests against the UART model
      sim_session.c      whole firmware on a PC with a scripted session
      mock/              host stand-ins for the Xilinx headers + register model
  stage2_hal/
    Readme.md            layers, the HAL, build and run, tests, bring-up checklist
    src/
      main.c             application: commands and formatting, no register access
      hal.h/.c           the abstraction: LED map, LED control, temperature
      gpio_drv.h/.c      PS MIO GPIO driver (XGpioPs)
      xadc_drv.h/.c      die temperature driver (XAdcPs)
      uart_driver.h/.c   console UART, carried forward from stage 1
      parser.h/.c        command parser, carried forward from stage 1
      board_zybo.h       the only board-specific file
    tests/host/
      test_hal.c         HAL and driver tests against a model of the board
      ...                the stage 1 suites, the simulator and the mocks
  stage3_sequencer_fsm/
    Readme.md            the state machine, interrupt-driven transmit, tests
    src/
      main.c             application: commands, formatting, the super-loop
      sequencer.h/.c     the switch-case state machine and its schedule
      uptime_drv.h/.c    milliseconds since boot (Cortex-A9 global timer)
      hal.h/.c           LEDs, temperature, time
      uart_driver.h/.c   receive ISR and line queue, transmit ring and ISR
      ...                the drivers and the parser, carried forward
    tests/host/
      test_sequencer.c   the state machine through simulated time
      ...                the earlier suites, the simulator and the mocks
```

Vitis 2025.2 compiles only the sources that sit directly in an application's
`src/` folder, so a stage's sources are copied in flat. All includes are by
file name.

Build outputs aren't tracked (Vivado project, XSA, Vitis workspace, test
binaries).

## Hardware and tools

- Digilent Zybo, Zybo Z7-10 or Zybo Z7-20
- Micro-USB cable on the PROG/UART port, serial terminal at 115200 8N1
- Vivado + Vitis 2025.2 (SDT flow), with the Digilent board files
- For the host tests: any C11 compiler (tested with GCC 15.2 from MSYS2 UCRT64)

## References

- UG585 - Zynq-7000 SoC Technical Reference Manual (UART controller, interrupts)
- UG643 - OS and Libraries Document Collection (standalone BSP, XUartPs)
- PG079 - AXI Timer (from stage 4)
- Zybo / Zybo Z7 reference manuals (Digilent)
