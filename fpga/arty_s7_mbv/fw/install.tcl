# Program the FPGA, load the firmware, and stage a kernel ELF file in DDR for
# the firmware to write to the SD card; the firmware then boots it from SD.
# usage: xsdb install.tcl <bitstream> <fw.elf> <kernel.elf> <stage_addr> <size_sym>
lassign $argv bit fw kernel stage size_sym
connect
targets -set -filter {name =~ "xc7s50*"}
fpga -file $bit
after 3000
targets -set -filter {name =~ "Hart #0*"}
if {[catch {stop}]} { rst -processor; after 500 }
puts "fw     : [dow $fw]"
dow -data $kernel $stage
mwr -size d $size_sym [file size $kernel]
puts "staged : $kernel ([file size $kernel] bytes) at $stage"
rwr pc 0
con
puts "running"
