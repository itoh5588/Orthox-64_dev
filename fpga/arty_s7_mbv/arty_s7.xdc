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
