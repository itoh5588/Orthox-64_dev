# Write a bitstream to the Arty S7 SPI flash (Micron MT25QL128 on the Rev E
# board), then boot the FPGA from it. Needs the MODE jumper (JP1) on.
#   vivado -mode batch -nolog -nojournal -source flash.tcl -tclargs build/mbv_soc_fw.bit [backup.bin]
# With backup.bin, the whole 16 MiB flash is first read back into that file.
# The bitstream uses the default SPI_BUSWIDTH = 1, so the .mcs is for SPIx1.

set bit [file normalize [lindex $argv 0]]
set backup [lindex $argv 1]
set mcs "[file rootname $bit].mcs"

puts "=== write_cfgmem: $bit -> $mcs"
write_cfgmem -force -format mcs -size 16 -interface SPIx1 \
             -loadbit "up 0x0 $bit" -file $mcs

open_hw_manager
connect_hw_server
open_hw_target
set dev [lindex [get_hw_devices xc7s50*] 0]
current_hw_device $dev
refresh_hw_device -update_hw_probes false $dev

create_hw_cfgmem -hw_device $dev [lindex [get_cfgmem_parts {mt25ql128-spi-x1_x2_x4}] 0]
set cm [get_property PROGRAM.HW_CFGMEM $dev]
set_property PROGRAM.ADDRESS_RANGE          {use_file} $cm
set_property PROGRAM.FILES                  [list $mcs] $cm
set_property PROGRAM.PRM_FILE               {}         $cm
set_property PROGRAM.UNUSED_PIN_TERMINATION {pull-none} $cm
set_property PROGRAM.BLANK_CHECK            0 $cm
set_property PROGRAM.ERASE                  1 $cm
set_property PROGRAM.CFG_PROGRAM            1 $cm
set_property PROGRAM.VERIFY                 1 $cm
set_property PROGRAM.CHECKSUM               0 $cm

puts "=== load the flash-programming bitstream into the FPGA"
create_hw_bitstream -hw_device $dev [get_property PROGRAM.HW_CFGMEM_BITFILE $dev]
program_hw_devices $dev
refresh_hw_device $dev

if {$backup ne ""} {
    puts "=== read back the whole flash -> $backup"
    set t0 [clock seconds]
    readback_hw_cfgmem -force -all -format bin -file $backup -hw_cfgmem $cm
    puts "=== read back in [expr {[clock seconds] - $t0}] s"
}

puts "=== program_hw_cfgmem (erase / program / verify)"
set t0 [clock seconds]
program_hw_cfgmem -hw_cfgmem $cm
puts "=== flash done in [expr {[clock seconds] - $t0}] s"

puts "=== boot_hw_device (the FPGA reloads from the flash)"
boot_hw_device $dev
after 8000
refresh_hw_device $dev
puts "DONE pin: [get_property REGISTER.CONFIG_STATUS.BIT14_DONE_PIN $dev]"

close_hw_target
disconnect_hw_server
close_hw_manager
puts "=== all done"
