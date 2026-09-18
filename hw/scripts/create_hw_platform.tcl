#------------------------------------------------------------------------------
# create_hw_platform.tcl
#
# Builds the complete hardware design for the Event-Driven Hardware Sequencer
# and exports the handoff Vitis needs:
#
#   - Zynq PS7 with the Digilent board preset (DDR, clocks, MIO: UART1 on
#     MIO48/49, GPIO on MIO7 for LD4, XADC over the PS interface)
#   - AXI Timer, the free-running microsecond counter the firmware uses to
#     measure how long a command takes to reach the hardware
#   - AXI GPIO driving the four PL LEDs LD0..LD3
#
# It runs synthesis and implementation, writes the bitstream and exports
# hw/export/<project>.xsa with the bitstream inside, so anyone can build the
# Vitis platform from the repository without opening Vivado.
#
# Usage, from the hw/ directory:
#   vivado -mode batch -source scripts/create_hw_platform.tcl -tclargs zybo-z7-20
#
# Board argument: zybo | zybo-z7-10 | zybo-z7-20     (default: zybo-z7-20)
# The Digilent board files have to be installed for the preset to apply.
#------------------------------------------------------------------------------

set board_name [expr {[llength $argv] > 0 ? [lindex $argv 0] : "zybo-z7-20"}]

set script_dir [file dirname [file normalize [info script]]]
set hw_dir     [file normalize [file join $script_dir ..]]
set proj_name  "zybo_seq_hw"
set proj_dir   [file join $hw_dir build $proj_name]
set bd_name    "seq_system"
set xdc_file   [file join $hw_dir src constraints zybo_pl_leds.xdc]
set xsa_file   [file join $hw_dir export ${proj_name}.xsa]

# Newest installed revision of the board files for the requested board.
set board_part [lindex [lsort [get_board_parts -quiet -latest_file_version "*:${board_name}:*"]] end]
if {$board_part eq ""} {
    error "No board files found for '$board_name'. Install the Digilent board files and retry."
}
set part_name [get_property PART_NAME [get_board_parts $board_part]]
puts "INFO: board part $board_part, device $part_name"

create_project $proj_name $proj_dir -part $part_name -force
set_property board_part $board_part [current_project]

create_bd_design $bd_name

#------------------------------------------------------------------------------
# Processing system
#------------------------------------------------------------------------------
set ps7 [create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 ps7]

# The board preset sets up DDR, the MIO mux and the clocks for this board.
apply_bd_automation -rule xilinx.com:bd_rule:processing_system7 \
    -config {make_external "FIXED_IO, DDR" apply_board_preset "1" Master "Disable" Slave "Disable"} $ps7

# On top of the preset:
#   M_AXI_GP0     the port the AXI Timer and AXI GPIO hang off
#   FCLK_CLK0     100 MHz, clocks the PL. The timer counts at this rate, so one
#                 tick is 10 ns and a 32-bit count wraps after ~43 s
#   FCLK_RESET0_N the PL reset the processor system reset block synchronises
#   IRQ_F2P       one fabric interrupt, wired to the timer below. The latency
#                 measurement only reads the counter, but a timer that cannot
#                 interrupt would be a dead end for anything time-triggered
#   MIO50/51      the Z7 preset leaves the internal pull-ups on, which holds
#                 BTN4/BTN5 at "pressed"; the original Zybo preset disables
#                 them. Not used by this firmware, but it keeps the hardware
#                 description honest for whoever adds buttons
set_property -dict [list \
    CONFIG.PCW_USE_M_AXI_GP0            {1} \
    CONFIG.PCW_EN_CLK0_PORT             {1} \
    CONFIG.PCW_EN_RST0_PORT             {1} \
    CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {100} \
    CONFIG.PCW_USE_FABRIC_INTERRUPT     {1} \
    CONFIG.PCW_IRQ_F2P_INTR             {1} \
    CONFIG.PCW_MIO_50_PULLUP            {disabled} \
    CONFIG.PCW_MIO_51_PULLUP            {disabled} \
] $ps7

#------------------------------------------------------------------------------
# PL peripherals
#------------------------------------------------------------------------------
# AXI Timer, left at its defaults: two 32-bit timers, both usable. The firmware
# runs timer 0 free-running as its microsecond time base.
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_timer:2.0 axi_timer_0

# AXI GPIO for LD0..LD3: one 4-bit channel, outputs only, all off at reset.
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio:2.0 axi_gpio_leds
set_property -dict [list \
    CONFIG.C_GPIO_WIDTH   {4} \
    CONFIG.C_ALL_OUTPUTS  {1} \
    CONFIG.C_IS_DUAL      {0} \
    CONFIG.C_DOUT_DEFAULT {0x00000000} \
] [get_bd_cells axi_gpio_leds]

#------------------------------------------------------------------------------
# Wiring
#------------------------------------------------------------------------------
# Connection automation builds the AXI interconnect and the processor system
# reset, and clocks both slaves from FCLK_CLK0.
apply_bd_automation -rule xilinx.com:bd_rule:axi4 -config { \
    Clk_master {Auto} Clk_slave {Auto} Clk_xbar {Auto} \
    Master {/ps7/M_AXI_GP0} Slave {/axi_timer_0/S_AXI} \
    ddr_seg {Auto} intc_ip {New AXI Interconnect} master_apm {0} } \
    [get_bd_intf_pins axi_timer_0/S_AXI]

apply_bd_automation -rule xilinx.com:bd_rule:axi4 -config { \
    Clk_master {Auto} Clk_slave {Auto} Clk_xbar {Auto} \
    Master {/ps7/M_AXI_GP0} Slave {/axi_gpio_leds/S_AXI} \
    ddr_seg {Auto} intc_ip {Auto} master_apm {0} } \
    [get_bd_intf_pins axi_gpio_leds/S_AXI]

# The four LEDs leave the chip. Named led_pl to match zybo_pl_leds.xdc.
create_bd_port -dir O -from 3 -to 0 led_pl
connect_bd_net [get_bd_pins axi_gpio_leds/gpio_io_o] [get_bd_port led_pl]

# Timer interrupt into the PS (GIC ID 61, the first fabric interrupt).
connect_bd_net [get_bd_pins axi_timer_0/interrupt] [get_bd_pins ps7/IRQ_F2P]

assign_bd_address
validate_bd_design
save_bd_design

puts "INFO: address map as seen by the processor"
foreach seg [get_bd_addr_segs -quiet -of_objects [get_bd_addr_spaces ps7/Data]] {
    puts [format "INFO:   %-30s offset %s  range %s" \
        [get_property NAME $seg] [get_property OFFSET $seg] [get_property RANGE $seg]]
}

#------------------------------------------------------------------------------
# Build
#------------------------------------------------------------------------------
set bd_file [get_files ${bd_name}.bd]
generate_target all $bd_file
add_files -norecurse [make_wrapper -files $bd_file -top]
set_property top ${bd_name}_wrapper [current_fileset]

add_files -fileset constrs_1 -norecurse $xdc_file
update_compile_order -fileset sources_1

launch_runs synth_1 -jobs 8
wait_on_run synth_1
if {[get_property PROGRESS [get_runs synth_1]] ne "100%"} {
    error "synthesis failed - see [get_property DIRECTORY [get_runs synth_1]]"
}

launch_runs impl_1 -to_step write_bitstream -jobs 8
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} {
    error "implementation failed - see [get_property DIRECTORY [get_runs impl_1]]"
}

# The implemented design has to be open for the bitstream to go into the XSA.
open_run impl_1

set wns [get_property STATS.WNS [get_runs impl_1]]
set whs [get_property STATS.WHS [get_runs impl_1]]
puts "INFO: timing: worst setup slack ${wns} ns, worst hold slack ${whs} ns"
if {($wns < 0) || ($whs < 0)} {
    error "timing not met - the design needs looking at before it is exported"
}

#------------------------------------------------------------------------------
# Export
#------------------------------------------------------------------------------
file mkdir [file dirname $xsa_file]
write_hw_platform -fixed -include_bit -force -file $xsa_file

puts "INFO: hardware platform written to $xsa_file"
puts "INFO: done"
