# Hardware

The programmable logic side of the sequencer, and the `.xsa` Vitis builds its
platform from. The exported handoff is committed, so nobody needs Vivado to
get the project running on a board:

```
hw/
  Readme.md
  scripts/create_hw_platform.tcl     rebuilds everything and re-exports the XSA
  src/constraints/zybo_pl_leds.xdc   the four PL LED pins
  export/zybo_seq_hw.xsa             the handoff, bitstream included (tracked)
```

## What the design contains

```
        +---------------------------------------------+
        |  Zynq7 Processing System (board preset)      |
        |    UART1  MIO48/49 -> USB-UART (console)     |
        |    GPIO   MIO7     -> LD4 (PS user LED)      |
        |    XADC   PS-XADC interface (die temperature)|
        |    DDR3, clocks, DDR/FIXED_IO pins           |
        +----+--------------------+-------------------+
             | M_AXI_GP0          | FCLK_CLK0 100 MHz
             | (AXI-Lite)         | FCLK_RESET0_N
             v                    v
        +----------------+   +--------------------+
        | AXI Interconnect|   | Processor System  |
        +--+----------+--+    | Reset             |
           |          |       +--------------------+
           v          v
    +-------------+  +------------------+
    | AXI Timer   |  | AXI GPIO         |
    | 2 x 32 bit  |  | 4 bits, outputs  |
    | 100 MHz     |  | 0 at reset       |
    +------+------+  +---------+--------+
           | interrupt          | led_pl[3:0]
           v                    v
      PS IRQ_F2P[0]      LD0..LD3 (M14, M15, G14, D18)
```

| Block | Why it is there |
|-------|-----------------|
| Zynq7 PS | the processor, its DDR and the MIO peripherals the firmware already uses |
| AXI Timer | the microsecond time base for the latency measurement: how long from a command being fully received to the hardware reacting |
| AXI GPIO | the four PL LEDs, which are the HAL's LED ids 1..4 |
| AXI Interconnect, Processor System Reset | added by Vivado's connection automation to join the two to `M_AXI_GP0` |

The timer runs from `FCLK_CLK0` at 100 MHz, so one tick is 10 ns and a 32-bit
count wraps after about 43 seconds - far longer than any command takes, and
fine for differences either way.

## Address map

Assigned by `assign_bd_address`, and the same numbers appear in the BSP's
`xparameters.h`, so the firmware never hard-codes them.

| Peripheral | Base address | Range |
|------------|--------------|-------|
| `axi_timer_0` | `0x42800000` | 64K |
| `axi_gpio_leds` | `0x41200000` | 64K |

The AXI Timer's interrupt reaches the GIC as ID 61, the first fabric
interrupt.

## Which board

Built and exported for the **Zybo Z7-20** (`xc7z020clg400-1`), the board this
project is brought up on. The committed XSA is for that board only - a
bitstream is device-specific.

For a **Zybo Z7-10** or the original **Zybo** (both `xc7z010clg400-1`), rebuild
with the script and pass the board name; everything else is identical,
including the LED pins, which are M14, M15, G14 and D18 on all three.

## Rebuilding it with the script

Needs Vivado 2025.2 with the Digilent board files installed. From the `hw`
folder:

```
vivado -mode batch -source scripts/create_hw_platform.tcl -tclargs zybo-z7-20
```

The board argument is `zybo`, `zybo-z7-10` or `zybo-z7-20` (default
`zybo-z7-20`). The script creates the project, builds the block design, runs
synthesis and implementation, checks that timing is met, writes the bitstream
and exports the XSA over `export/zybo_seq_hw.xsa`.

| Output | What |
|--------|------|
| `hw/build/zybo_seq_hw/` | the Vivado project - throwaway, git-ignored |
| `hw/export/zybo_seq_hw.xsa` | the handoff, with the bitstream inside - **tracked in git** |

The exported XSA is committed on purpose: it is what makes the repository
enough on its own. Someone who only wants to run the project never opens
Vivado.

## Building it by hand in the Vivado GUI

The same design, click by click, if you would rather see it being built. Menu
names are Vivado 2025.2.

**1. Project**

1. *File > Project > New > Next*.
2. Name `zybo_seq_hw`, location `<repo>/hw/build`, tick *Create project
   subdirectory* > *Next*.
3. *RTL Project*, tick *Do not specify sources at this time* > *Next*.
4. *Boards* tab, search `zybo`, pick **Zybo Z7-20** > *Next* > *Finish*.
   (No Zybo in the list means the Digilent board files are not installed.)

**2. Block design and the processing system**

5. *Flow Navigator > IP INTEGRATOR > Create Block Design*, name `seq_system` > *OK*.
6. In the diagram, **+** > `ZYNQ7 Processing System` > double-click to add it.
7. Green banner > **Run Block Automation** > leave *Apply Board Preset* ticked
   and DDR / FIXED_IO external > *OK*. That is what brings in DDR, the clocks
   and the MIO map for this board, UART1 on MIO48/49 and GPIO on MIO7 included.
8. Double-click the ZYNQ7 block and change four things:
   - *PS-PL Configuration > AXI Non Secure Enablement > GP Master AXI
     Interface*: tick **M AXI GP0 interface**.
   - *PS-PL Configuration > General > Interrupts > Fabric Interrupts*: tick it,
     then under *PL-PS Interrupt Ports* tick **IRQ_F2P[15:0]**.
   - *Clock Configuration > PL Fabric Clocks*: tick **FCLK_CLK0** and set it to
     **100 MHz**.
   - *MIO Configuration*, rows **50** and **51**: set *Pullup* to **disabled**.
     Not used by this firmware; it keeps BTN4/BTN5 honest for later.
   - *OK*.

**3. The PL peripherals**

9. **+** > `AXI Timer` > add. Leave every setting at its default.
10. **+** > `AXI GPIO` > add. Double-click it:
    - *GPIO*: tick **All Outputs**, set *GPIO Width* to **4**, leave
      *Default Output Value* at `0x00000000`.
    - leave *Enable Dual Channel* unticked > *OK*.
    - Right-click the block > *Block Properties* > rename it to
      **`axi_gpio_leds`** (the name the address map and `xparameters.h` will use).
11. Green banner > **Run Connection Automation** > tick **All Automation** >
    *OK*. This adds the AXI Interconnect and the Processor System Reset, and
    clocks both peripherals from FCLK_CLK0.

**4. The two connections automation does not make**

12. On `axi_gpio_leds`, click the **+** next to the `GPIO` interface to expand
    it, right-click the `gpio_io_o[3:0]` pin > **Make External**. Select the new
    port, and in *External Port Properties* rename it to **`led_pl`**. The name
    has to match `src/constraints/zybo_pl_leds.xdc`.
13. Drag a wire from `axi_timer_0/interrupt` to the PS `IRQ_F2P[0:0]` pin.

**5. Check, wrap, constrain, build**

14. *Window > Address Editor*: both peripherals should already have addresses.
    If not, right-click > *Assign All*.
15. Press **F6** (*Validate Design*). It should report no errors.
16. *Sources* > right-click `seq_system.bd` > **Create HDL Wrapper** > *Let
    Vivado manage wrapper and auto-update* > *OK*.
17. *Sources > Add Sources > Add or create constraints > Add Files* >
    `hw/src/constraints/zybo_pl_leds.xdc`. **Untick** *Copy constraints files
    into project*, so the repository copy stays the one in use > *Finish*.
18. *Flow Navigator >* **Generate Bitstream** > *Yes* to run synthesis and
    implementation. A few minutes on a normal laptop.
19. When it finishes, choose **Open Implemented Design** (the export needs it
    open to put the bitstream in the XSA).

**6. Export**

20. *File > Export > Export Hardware* > *Next* > **Include bitstream** > *Next*.
21. *Export to* `<repo>/hw/export`, file name `zybo_seq_hw` > *Next* > *Finish*.
22. Check that `hw/export/zybo_seq_hw.xsa` was written, and commit it. That
    single file is what the Vitis side of the repository needs.

## Using the exported XSA in Vitis

This is the path for someone who just pulled the repository and wants the
project running. Vivado is not needed.

1. Start **Vitis 2025.2** and open (or create) a workspace folder, for example
   `<repo>/sw/workspace` - it is git-ignored.
2. *File > New Component > **Platform***. Name it `zybo_seq_platform`, and for
   the hardware description browse to **`hw/export/zybo_seq_hw.xsa`**.
   Operating system **standalone**, processor **ps7_cortexa9_0** > *Finish*.
3. Select the platform in the FLOW panel > **Build**.
4. *File > New Component > **Application***. Name it `seq_cli`, platform
   `zybo_seq_platform`, domain `standalone_ps7_cortexa9_0` > *Finish*.
5. Copy the firmware sources into the application's `src/` folder, flat, next
   to the generated `CMakeLists.txt`, `UserConfig.cmake` and `lscript.ld`
   (don't replace that folder). From PowerShell, for the newest stage:
   ```powershell
   Copy-Item "<repo>\sw\stage3_sequencer_fsm\src\*" -Include *.c,*.h -Destination "<workspace>\seq_cli\src" -Force
   ```
6. Select `seq_cli` > **Build**.
7. Board: jumper **JP5 to JTAG**, micro-USB into **PROG/UART**, power on. Open a
   terminal on the board's COM port at **115200 8N1**, no flow control, local
   echo off.
8. Select `seq_cli` > **Run**. Vitis programs the PL with the bitstream from the
   platform, applies `ps7_init`, downloads the ELF and starts the core.

Type `HELP`, then `LED_BLINK 10000 250`, and keep typing while LD4 blinks.

## What the firmware uses, and what is waiting

The firmware in this repository today (stages 1 to 3) runs entirely on the PS:
console UART, LD4 on MIO7, the XADC and the Cortex-A9 global timer. It works
on this platform exactly as it did on a PS-only one - the PL peripherals are
present and idle.

Two of them are there for the stage 4 firmware, which is the next piece of
work:

| Hardware | Used by | Firmware still to write |
|----------|---------|-------------------------|
| AXI Timer | the execution-time measurement the project is named for | a timer driver, `HAL_GetTimestampUs()`, a timestamp taken when a line is completed in the UART ISR, and the delta printed after the command acts |
| AXI GPIO | HAL LED ids 1..4 (LD0..LD3) | an AXI GPIO driver behind the existing `hal_led_source_t` table - the HAL was built with that slot in it |

Until then `LED_SET 1 1` still answers "not available in this build", which is
the honest answer: the pins exist, the driver does not.

## Checking the design on the board

- [ ] Vitis creates the platform from the XSA without complaining that the file has no bitstream
- [ ] The platform's `xparameters.h` contains `XPAR_AXI_TIMER_0_BASEADDR` (0x42800000) and `XPAR_AXI_GPIO_LEDS_BASEADDR` (0x41200000)
- [ ] Running the application programs the PL: the **DONE** LED (LD12, green, next to the FPGA) lights
- [ ] The console banner appears and `HELP` answers - the PS side is unchanged by any of this
- [ ] LD0..LD3 stay dark (the GPIO resets to 0 and no firmware drives it yet)
- [ ] A quick check from the Vitis debugger or an `xsct` console: writing `0xF` to `0x41200000` lights all four PL LEDs, `0x0` clears them

That last one is worth doing once: it proves the bitstream, the AXI path and
the LED pins in one step, before any driver exists. In an `xsct` console with
the board connected:

```
connect
targets -set -filter {name =~ "ARM*#0"}
mwr 0x41200004 0x0        ;# direction register: all four bits outputs
mwr 0x41200000 0xF        ;# data register: all on
mwr 0x41200000 0x0        ;# all off
```

## The build that produced the committed XSA

Vivado 2025.2, board files `digilentinc.com:zybo-z7-20:part0:1.2`, device
`xc7z020clg400-1`, run through `scripts/create_hw_platform.tcl` end to end:

| | |
|---|---|
| Timing | met - worst setup slack 2.97 ns, worst hold slack 0.06 ns |
| LUTs | 842 of 53200 (1.6 %) |
| Flip-flops | 975 of 106400 (0.9 %) |
| Block RAM, DSP | none |
| Bonded IOBs | 4 (the LED pins; everything else is on fixed PS pins) |
| XSA | 354 KB, containing `zybo_seq_hw.bit` (4.0 MB uncompressed), `ps7_init.*` and `seq_system.hwh` |

The log has eight critical warnings, all from Digilent's board preset: the
DDR trace delays `PCW_UIPARAM_DDR_DQS_TO_CLK_DELAY_0..3` are negative for this
board. They come with the preset, not with anything here, and the same board
runs DDR fine. The pile of `Board 49-26` warnings is Vivado listing eval
boards the installed edition cannot use.

## Troubleshooting

| Symptom | Cause and fix |
|---------|---------------|
| `No board files found for 'zybo-z7-20'` | the Digilent board files are not installed - add them through *Tools > Vivado Store > Boards*, or drop them into `Vivado/data/boards/board_files` |
| No Zybo in the *Boards* tab of the New Project wizard | the same |
| Validation error about an undriven `M_AXI_GP0_ACLK` | M AXI GP0 was enabled but connection automation has not run yet - run it |
| `led_pl` not found when the constraints are read | the external port is named something else (`gpio_io_o_0`, `GPIO_0`); rename the port to `led_pl`, or rename the ports in the XDC to match |
| Export produces an XSA without a bitstream | the implemented design was not open, or *Include bitstream* was not ticked. Vitis then reports the platform has no bitstream and the PL stays unprogrammed |
| Vitis platform builds, but `XPAR_AXI_TIMER_0_*` is missing | the platform was built from an older PS-only XSA - point it at `hw/export/zybo_seq_hw.xsa` and rebuild |
| DONE LED stays dark when running from Vitis | the PL was not programmed: either the XSA has no bitstream, or the run configuration has bitstream programming turned off |
| The firmware runs but LD0..LD3 never light | expected today - the AXI GPIO has no driver yet, that is stage 4 firmware |
