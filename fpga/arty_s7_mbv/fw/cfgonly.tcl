# Configure the FPGA only; the firmware in the bitstream's BRAM then boots
# the kernel from the SD card by itself, as at power-on.
# usage: xsdb cfgonly.tcl <bitstream>
connect
targets -set -filter {name =~ "xc7s50*"}
fpga -file [lindex $argv 0]
puts "configured"
