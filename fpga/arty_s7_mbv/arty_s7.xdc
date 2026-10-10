# Arty S7-50 oscillator and USB-UART (Digilent board pinout).
set_property PACKAGE_PIN R2 [get_ports clk_100mhz]
set_property IOSTANDARD SSTL135 [get_ports clk_100mhz]
create_clock -name clk_100mhz -period 10.000 [get_ports clk_100mhz]
set_property PACKAGE_PIN V12 [get_ports UART_0_rxd]
set_property PACKAGE_PIN R12 [get_ports UART_0_txd]
set_property IOSTANDARD LVCMOS33 [get_ports {UART_0_rxd UART_0_txd}]

# Status LEDs LD2/LD3; both signals are quasi-static.
set_property PACKAGE_PIN E18 [get_ports led_locked]
set_property PACKAGE_PIN F13 [get_ports led_calib]
set_property IOSTANDARD LVCMOS33 [get_ports {led_locked led_calib}]
set_false_path -to [get_ports {led_locked led_calib}]

# Arty S7 configuration bank voltage.
set_property CFGBVS VCCO [current_design]
set_property CONFIG_VOLTAGE 3.3 [current_design]

# Digilent Pmod MicroSD (410-380) on JD, SPI mode (same pins as the author's
# Arty S7 standalone w11): jd1 CS, jd2 MOSI, jd3 MISO, jd4 SCLK, jd9 CD.
set_property -dict { PACKAGE_PIN V15 IOSTANDARD LVCMOS33 } [get_ports sd_cs_n]
set_property -dict { PACKAGE_PIN U12 IOSTANDARD LVCMOS33 } [get_ports sd_mosi]
set_property -dict { PACKAGE_PIN V13 IOSTANDARD LVCMOS33 } [get_ports sd_miso]
set_property -dict { PACKAGE_PIN T12 IOSTANDARD LVCMOS33 } [get_ports sd_sclk]
set_property -dict { PACKAGE_PIN T11 IOSTANDARD LVCMOS33 } [get_ports sd_cd]
set_property PULLUP true [get_ports {sd_miso sd_cd}]
set_property DRIVE 8 [get_ports {sd_cs_n sd_mosi sd_sclk}]
# SCLK is a fabric register (<= 8.3 MHz at 50 MHz); MISO goes through two
# synchronizer flops sampled mid high phase (sdspi_phy).
set_false_path -to [get_ports {sd_cs_n sd_mosi sd_sclk}]
set_false_path -from [get_ports {sd_miso sd_cd}]

# 3.3 V TTL serial cable (FTDI FT232R) on JC, same wiring as the author's
# Arty S7 standalone w11: jc1 U15 = FPGA TXD (to cable RXD),
# jc2 V16 = FPGA RXD (from cable TXD), jc5 = GND.  Mirrors the console.
set_property -dict { PACKAGE_PIN U15 IOSTANDARD LVCMOS33 } [get_ports jc_txd]
set_property -dict { PACKAGE_PIN V16 IOSTANDARD LVCMOS33 } [get_ports jc_rxd]
set_property PULLUP true [get_ports jc_rxd]
set_property DRIVE 8 [get_ports jc_txd]
set_false_path -to [get_ports jc_txd]
set_false_path -from [get_ports jc_rxd]
