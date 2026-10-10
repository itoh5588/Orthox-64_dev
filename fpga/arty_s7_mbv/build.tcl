# Vivado 2025.2, Arty S7-50, MicroBlaze V RV64/Sv39 prototype.
set here [file dirname [file normalize [info script]]]
set build_dir [file join $here build]
file mkdir $build_dir
if {[info exists ::env(MIG_SOURCE_PRJ)]} {
    set mig_source $::env(MIG_SOURCE_PRJ)
} else {
    set mig_source /home/itoh/PDP-11/w11/rtl/bplib/artys7/mig_a.prj
}
if {![file exists $mig_source]} {error "MIG source not found: $mig_source"}
set fd [open $mig_source r]
set mig_xml [read $fd]
close $fd
set mig_xml [string map {<ModuleName>migui_artys7</ModuleName> <ModuleName>mig_axi</ModuleName> <PortInterface>NATIVE</PortInterface> <PortInterface>AXI</PortInterface>} $mig_xml]
set mig_prj [file join $build_dir mig_axi.prj]
set fd [open $mig_prj w]
puts -nonewline $fd $mig_xml
close $fd
create_project -force mbv_soc [file join $build_dir vivado] -part xc7s50csga324-1
# RTL used as BD module references (SD card SPI controller).
add_files -norecurse [glob [file join $here rtl *.vhd]]
update_compile_order -fileset sources_1
create_bd_design soc
set cpu [create_bd_cell -type ip -vlnv xilinx.com:ip:microblaze_riscv:1.0 cpu]
set_property -dict [list CONFIG.C_DATA_SIZE 64 CONFIG.C_ADDR_SIZE 39 CONFIG.C_USE_MMU 3 CONFIG.C_USE_MULDIV 1 CONFIG.C_USE_FPU 2 CONFIG.C_USE_ATOMIC 1 CONFIG.C_USE_COMPRESSION 1 CONFIG.C_I_AXI 1 CONFIG.C_D_AXI 1 CONFIG.C_I_LMB 0 CONFIG.C_D_LMB 0 CONFIG.C_USE_ICACHE 1 CONFIG.C_USE_DCACHE 1 CONFIG.C_USE_INTERRUPT 1 CONFIG.C_ICACHE_BASEADDR 0x80000000 CONFIG.C_ICACHE_HIGHADDR 0x8fffffff CONFIG.C_DCACHE_BASEADDR 0x80000000 CONFIG.C_DCACHE_HIGHADDR 0x8fffffff CONFIG.C_USE_SLEEP 1 CONFIG.C_ENABLE_DISCRETE_PORTS 1] $cpu
# WFI sleeps until a Wakeup bit is 1 (UG1629). The Sstc timer has no external
# line to wake on, so tie Wakeup high and let WFI return at once.
set wake [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 wake]
set_property -dict [list CONFIG.CONST_VAL 3 CONFIG.CONST_WIDTH 2] $wake
set mig [create_bd_cell -type ip -vlnv xilinx.com:ip:mig_7series:4.2 mig]
set_property -dict [list CONFIG.XML_INPUT_FILE $mig_prj CONFIG.RESET_BOARD_INTERFACE Custom CONFIG.MIG_DONT_TOUCH_PARAM Custom CONFIG.BOARD_MIG_PARAM Custom] $mig
set clkbuf [create_bd_cell -type ip -vlnv xilinx.com:ip:util_ds_buf:2.2 clkbuf]
set_property CONFIG.C_BUF_TYPE BUFG $clkbuf
set clk [create_bd_cell -type ip -vlnv xilinx.com:ip:clk_wiz:6.0 clk]
set_property -dict [list CONFIG.PRIM_SOURCE Global_buffer CONFIG.PRIM_IN_FREQ 100 CONFIG.CLKOUT2_USED true CONFIG.CLKOUT3_USED true CONFIG.CLKOUT1_REQUESTED_OUT_FREQ 50 CONFIG.CLKOUT2_REQUESTED_OUT_FREQ 160 CONFIG.CLKOUT3_REQUESTED_OUT_FREQ 200] $clk
set rst [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 rst]
set ui_rst [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 ui_rst]
# aux_reset_in is tied low, so make it active-high (inactive). C_EXT_RESET_HIGH is
# propagated read-only (active-low), so ext_reset_in is tied high instead.
set_property CONFIG.C_AUX_RESET_HIGH 1 [list $rst $ui_rst]
# Two interconnects instead of one 4x6 SmartConnect (14.5k LUT): the cache
# masters (IC/DC) only reach DDR (the cacheable range is all of DDR), and the
# uncached masters (IP/DP) only reach the boot BRAM and the AXI4-Lite devices.
set sc [create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 sc]
set_property -dict [list CONFIG.NUM_SI 2 CONFIG.NUM_MI 1] $sc
set psc [create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 psc]
set_property -dict [list CONFIG.NUM_SI 2 CONFIG.NUM_MI 5] $psc
# Pmod MicroSD on JD (SPI mode), registers at 0x40800000; see rtl/sd_axi.vhd.
set sd [create_bd_cell -type module -reference sd_axi sd]
set cc [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_clock_converter:2.1 cc]
set uart [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_uartlite:2.0 uart]
set_property -dict [list CONFIG.C_BAUDRATE 115200 CONFIG.C_S_AXI_ACLK_FREQ_HZ 50000000] $uart
set timer [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_timer:2.0 timer]
set intc [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_intc:4.1 intc]

set bramc [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_bram_ctrl:4.1 bramc]
set_property -dict [list CONFIG.SINGLE_PORT_BRAM 1 CONFIG.PROTOCOL AXI4LITE] $bramc
set bram [create_bd_cell -type ip -vlnv xilinx.com:ip:blk_mem_gen:8.4 bram]
set_property -dict [list CONFIG.Memory_Type True_Dual_Port_RAM CONFIG.Assume_Synchronous_Clk true] $bram
set mdm [create_bd_cell -type ip -vlnv xilinx.com:ip:mdm_riscv:1.0 mdm]
set concat [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat:2.1 concat]
set_property CONFIG.NUM_PORTS 2 $concat
set not_lock [create_bd_cell -type ip -vlnv xilinx.com:ip:util_vector_logic:2.0 not_lock]
set_property CONFIG.C_OPERATION not $not_lock
set not_ui_rst [create_bd_cell -type ip -vlnv xilinx.com:ip:util_vector_logic:2.0 not_ui_rst]
set_property CONFIG.C_OPERATION not $not_ui_rst
set zero [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 zero]
set_property CONFIG.CONST_VAL 0 $zero
set one [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 one]
set_property CONFIG.CONST_VAL 1 $one
set temp_zero [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 temp_zero]
set_property -dict [list CONFIG.CONST_VAL 0 CONFIG.CONST_WIDTH 12] $temp_zero

set clock_port [create_bd_port -dir I -type clk -freq_hz 100000000 clk_100mhz]
connect_bd_net $clock_port [get_bd_pins clkbuf/BUFG_I]
connect_bd_net [get_bd_pins clkbuf/BUFG_O] [get_bd_pins clk/clk_in1]
connect_bd_net [get_bd_pins zero/dout] [get_bd_pins clk/reset] [get_bd_pins rst/aux_reset_in] [get_bd_pins ui_rst/aux_reset_in]
connect_bd_net [get_bd_pins one/dout] [get_bd_pins rst/ext_reset_in] [get_bd_pins ui_rst/ext_reset_in]
connect_bd_net [get_bd_pins wake/dout] [get_bd_pins cpu/Wakeup]
connect_bd_net [get_bd_pins temp_zero/dout] [get_bd_pins mig/device_temp_i]
connect_bd_net [get_bd_pins clk/locked] [get_bd_pins not_lock/Op1] [get_bd_pins rst/dcm_locked]
connect_bd_net [get_bd_pins not_lock/Res] [get_bd_pins mig/sys_rst]
connect_bd_net [get_bd_pins clk/clk_out1] [get_bd_pins cpu/Clk] [get_bd_pins rst/slowest_sync_clk] [get_bd_pins sc/aclk] [get_bd_pins psc/aclk] [get_bd_pins cc/s_axi_aclk] [get_bd_pins uart/s_axi_aclk] [get_bd_pins timer/s_axi_aclk] [get_bd_pins intc/s_axi_aclk] [get_bd_pins bramc/s_axi_aclk] [get_bd_pins sd/s_axi_aclk]
connect_bd_net [get_bd_pins clk/clk_out2] [get_bd_pins mig/sys_clk_i]
connect_bd_net [get_bd_pins clk/clk_out3] [get_bd_pins mig/clk_ref_i]
connect_bd_net [get_bd_pins mig/ui_clk] [get_bd_pins cc/m_axi_aclk] [get_bd_pins ui_rst/slowest_sync_clk]
connect_bd_net [get_bd_pins mig/ui_clk_sync_rst] [get_bd_pins ui_rst/mb_debug_sys_rst] [get_bd_pins not_ui_rst/Op1]
connect_bd_net [get_bd_pins not_ui_rst/Res] [get_bd_pins mig/aresetn]
connect_bd_net [get_bd_pins mig/init_calib_complete] [get_bd_pins ui_rst/dcm_locked]
# Status LEDs: LD2 = clock locked, LD3 = DDR3 calibration complete.
connect_bd_net [create_bd_port -dir O led_locked] [get_bd_pins clk/locked]
connect_bd_net [create_bd_port -dir O led_calib] [get_bd_pins mig/init_calib_complete]
connect_bd_net [get_bd_pins mdm/Debug_SYS_Rst] [get_bd_pins rst/mb_debug_sys_rst]
connect_bd_net [get_bd_pins rst/mb_reset] [get_bd_pins cpu/Reset]
connect_bd_net [get_bd_pins rst/interconnect_aresetn] [get_bd_pins sc/aresetn] [get_bd_pins psc/aresetn] [get_bd_pins cc/s_axi_aresetn]
connect_bd_net [get_bd_pins rst/peripheral_aresetn] [get_bd_pins uart/s_axi_aresetn] [get_bd_pins timer/s_axi_aresetn] [get_bd_pins intc/s_axi_aresetn] [get_bd_pins bramc/s_axi_aresetn] [get_bd_pins sd/s_axi_aresetn]
connect_bd_net [get_bd_pins ui_rst/peripheral_aresetn] [get_bd_pins cc/m_axi_aresetn]

connect_bd_intf_net [get_bd_intf_pins cpu/M_AXI_IC] [get_bd_intf_pins sc/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins cpu/M_AXI_DC] [get_bd_intf_pins sc/S01_AXI]
connect_bd_intf_net [get_bd_intf_pins cpu/M_AXI_IP] [get_bd_intf_pins psc/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins cpu/M_AXI_DP] [get_bd_intf_pins psc/S01_AXI]
connect_bd_intf_net [get_bd_intf_pins sc/M00_AXI] [get_bd_intf_pins cc/S_AXI]
connect_bd_intf_net [get_bd_intf_pins cc/M_AXI] [get_bd_intf_pins mig/S_AXI]
connect_bd_intf_net [get_bd_intf_pins psc/M00_AXI] [get_bd_intf_pins uart/S_AXI]
connect_bd_intf_net [get_bd_intf_pins psc/M01_AXI] [get_bd_intf_pins timer/S_AXI]
connect_bd_intf_net [get_bd_intf_pins psc/M02_AXI] [get_bd_intf_pins intc/s_axi]
connect_bd_intf_net [get_bd_intf_pins psc/M03_AXI] [get_bd_intf_pins bramc/S_AXI]
connect_bd_intf_net [get_bd_intf_pins psc/M04_AXI] [get_bd_intf_pins sd/s_axi]
foreach p {sd_cs_n sd_sclk sd_mosi} { connect_bd_net [create_bd_port -dir O $p] [get_bd_pins sd/$p] }
foreach p {sd_miso sd_cd} { connect_bd_net [create_bd_port -dir I $p] [get_bd_pins sd/$p] }
connect_bd_intf_net [get_bd_intf_pins bramc/BRAM_PORTA] [get_bd_intf_pins bram/BRAM_PORTA]
connect_bd_intf_net [get_bd_intf_pins mdm/MBDEBUG_0] [get_bd_intf_pins cpu/DEBUG]
connect_bd_net [get_bd_pins uart/interrupt] [get_bd_pins concat/In0]
connect_bd_net [get_bd_pins timer/interrupt] [get_bd_pins concat/In1]
connect_bd_net [get_bd_pins concat/dout] [get_bd_pins intc/intr]
connect_bd_intf_net [get_bd_intf_pins intc/interrupt] [get_bd_intf_pins cpu/INTERRUPT]
make_bd_intf_pins_external [get_bd_intf_pins mig/DDR3]
# Console on both the on-board USB-UART and a 3.3 V TTL cable on JC:
# TXD drives both pins, RXD is the AND of both (idle high).
set rx_and [create_bd_cell -type ip -vlnv xilinx.com:ip:util_vector_logic:2.0 rx_and]
set_property -dict [list CONFIG.C_OPERATION and CONFIG.C_SIZE 1] $rx_and
connect_bd_net [create_bd_port -dir I UART_0_rxd] [get_bd_pins rx_and/Op1]
connect_bd_net [create_bd_port -dir I jc_rxd] [get_bd_pins rx_and/Op2]
connect_bd_net [get_bd_pins rx_and/Res] [get_bd_pins uart/rx]
connect_bd_net [get_bd_pins uart/tx] [create_bd_port -dir O UART_0_txd] [create_bd_port -dir O jc_txd]
assign_bd_address
foreach space {Data Instruction} {
    # assign_bd_address may put the SD controller at 0x0; move it out first.
    set seg [get_bd_addr_segs /cpu/$space/SEG_sd_reg0]
    set_property range 64K $seg
    set_property offset 0x40800000 $seg
    set seg [get_bd_addr_segs /cpu/$space/SEG_bramc_Mem0]
    set_property range 64K $seg
    set_property offset 0x00000000 $seg
    set seg [get_bd_addr_segs /cpu/$space/SEG_mig_memaddr]
    set_property offset 0x80000000 $seg
}
validate_bd_design
save_bd_design
puts "ADDRESS_MAP"
foreach a [get_bd_addr_segs /cpu/*/SEG_*] {
    puts "SEG $a [get_property OFFSET $a] [get_property RANGE $a]"
}
set bd_file [get_files [file join $build_dir vivado mbv_soc.srcs sources_1 bd soc soc.bd]]
set wrapper [make_wrapper -files $bd_file -top]
add_files -norecurse $wrapper
set_property top soc_wrapper [current_fileset]
add_files -fileset constrs_1 [file join $here arty_s7.xdc]
update_compile_order -fileset sources_1
generate_target all $bd_file
launch_runs synth_1 -jobs 4
wait_on_run synth_1
if {[get_property STATUS [get_runs synth_1]] ne "synth_design Complete!"} {
    error "Synthesis failed: [get_property STATUS [get_runs synth_1]]"
}
open_run synth_1
report_utilization -file [file join $build_dir synth_utilization.rpt]
report_timing_summary -file [file join $build_dir synth_timing.rpt]
close_design
launch_runs impl_1 -to_step route_design -jobs 4
wait_on_run impl_1
if {[get_property STATUS [get_runs impl_1]] ne "route_design Complete!"} {
    error "Implementation failed: [get_property STATUS [get_runs impl_1]]"
}
open_run impl_1
report_utilization -file [file join $build_dir route_utilization.rpt]
report_timing_summary -file [file join $build_dir route_timing.rpt]
report_drc -file [file join $build_dir route_drc.rpt]
report_clock_networks -file [file join $build_dir route_clocks.rpt]
write_bitstream -force [file join $build_dir mbv_soc.bit]
# BRAM layout for updatemem (fw/embed.sh puts the firmware into the bitstream).
write_mem_info -force [file join $build_dir mbv_soc.mmi]
close_project
