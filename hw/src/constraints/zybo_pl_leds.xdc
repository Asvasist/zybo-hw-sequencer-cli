#------------------------------------------------------------------------------
# zybo_pl_leds.xdc
#
# The four PL LEDs LD0..LD3, driven by the AXI GPIO in the block design.
#
# Same package pins on the original Zybo, the Zybo Z7-10 and the Zybo Z7-20 -
# checked against the pin tables in Vivado's own board files for all three.
# Anode-connected through a resistor, so a high level lights the LED.
#
# Nothing else in this design reaches the outside world: the console UART, the
# user LED LD4 and the XADC are all on fixed PS pins, which need no constraint.
#------------------------------------------------------------------------------

set_property -dict { PACKAGE_PIN M14  IOSTANDARD LVCMOS33 } [get_ports { led_pl[0] }];   # Sch=led[0], LD0
set_property -dict { PACKAGE_PIN M15  IOSTANDARD LVCMOS33 } [get_ports { led_pl[1] }];   # Sch=led[1], LD1
set_property -dict { PACKAGE_PIN G14  IOSTANDARD LVCMOS33 } [get_ports { led_pl[2] }];   # Sch=led[2], LD2
set_property -dict { PACKAGE_PIN D18  IOSTANDARD LVCMOS33 } [get_ports { led_pl[3] }];   # Sch=led[3], LD3
