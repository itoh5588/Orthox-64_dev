# Program the FPGA, load firmware into BRAM and a kernel ELF into DDR, run.
# usage: xsdb run.tcl <bitstream> <fw.elf> <kernel.elf> <bss_start> <bss_end> <zero_start_sym> <zero_end_sym> [rootfs.img initrd_start]
lassign $argv bit fw kernel bss_start bss_end zs ze rootfs initrd
connect
targets -set -filter {name =~ "xc7s50*"}
fpga -file $bit
after 3000
targets -set -filter {name =~ "Hart #0*"}
if {[catch {stop}]} { rst -processor; after 500 }
puts "fw     : [dow $fw]"
puts "kernel : [dow $kernel]"
mwr -size d $zs $bss_start
mwr -size d $ze $bss_end
puts "bss    : $bss_start..$bss_end cleared by fw"
if {$rootfs ne ""} {
    set t0 [clock milliseconds]
    dow -data $rootfs $initrd
    puts "rootfs : $rootfs at $initrd ([expr {[clock milliseconds] - $t0}] ms)"
}
rwr pc 0
con
puts "running"
