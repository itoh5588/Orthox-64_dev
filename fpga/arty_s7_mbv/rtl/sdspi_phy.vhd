-- SPDX-License-Identifier: MIT
-- Copyright (c) 2026 Akio Itoh
--
-- Ported from the author's Arty S7 standalone w11 (artys7_standalone/sdspi_phy.vhd),
-- with the w11 slvtypes subtypes replaced by plain std_logic(_vector).
--
-- SD card SPI byte engine, SPI mode 0 (SCLK idles low, the card samples MOSI
-- on the rising and changes MISO after the falling edge).
--
-- SCLK is a fabric register; each phase lasts DIV+1 CLK cycles.  MISO passes
-- two synchronizer flops, so the value taken when a bit ends (at the falling
-- edge) was on the pin two cycles earlier, well inside the high phase for
-- DIV >= 2.  DIV = 2 gives 12.5 MHz at 75 MHz.  DIV = 1 (12.5 MHz at 50 MHz,
-- Orthox) samples at the rising edge itself, still a full phase after the
-- card changed MISO on the falling edge, which is the SPI mode 0 sample point.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;


entity sdspi_phy is
  port (
    CLK : in std_logic;
    RESET : in std_logic;
    DIV : in std_logic_vector(7 downto 0);                      -- phase length - 1 (>= 1)
    START : in std_logic;                   -- start byte transfer
    TXD : in std_logic_vector(7 downto 0);                      -- byte to send (msb first)
    RXD : out std_logic_vector(7 downto 0);                     -- byte received
    DONE : out std_logic;                   -- 1 cycle pulse: RXD valid
    BUSY : out std_logic;
    O_SCLK : out std_logic;
    O_MOSI : out std_logic;
    I_MISO : in std_logic
  );
end sdspi_phy;

architecture syn of sdspi_phy is

  type state_type is (s_idle, s_low, s_high);

  signal R_STATE : state_type := s_idle;
  signal R_CNT : unsigned(7 downto 0) := (others => '0');
  signal R_BIT : unsigned(2 downto 0) := (others => '0');
  signal R_TX : std_logic_vector(7 downto 0) := (others => '1');
  signal R_RX : std_logic_vector(7 downto 0) := (others => '1');
  signal R_DONE : std_logic := '0';
  signal R_SCLK : std_logic := '0';
  signal R_MOSI : std_logic := '1';
  signal R_MISO_1 : std_logic := '1';
  signal R_MISO_2 : std_logic := '1';

  attribute ASYNC_REG : string;
  attribute ASYNC_REG of R_MISO_1 : signal is "TRUE";
  attribute ASYNC_REG of R_MISO_2 : signal is "TRUE";

begin

  proc_regs: process (CLK)
  begin
    if rising_edge(CLK) then
      R_MISO_1 <= I_MISO;
      R_MISO_2 <= R_MISO_1;
      R_DONE <= '0';
      if RESET = '1' then
        R_STATE <= s_idle;
        R_SCLK <= '0';
        R_MOSI <= '1';
      else
        case R_STATE is
          when s_idle =>
            if START = '1' then
              R_TX <= TXD;
              R_MOSI <= TXD(7);
              R_BIT <= (others => '0');
              R_CNT <= unsigned(DIV);
              R_STATE <= s_low;
            end if;

          when s_low =>
            if R_CNT = 0 then
              R_SCLK <= '1';
              R_CNT <= unsigned(DIV);
              R_STATE <= s_high;
            else
              R_CNT <= R_CNT - 1;
            end if;

          when s_high =>
            if R_CNT = 0 then
              R_SCLK <= '0';
              R_RX <= R_RX(6 downto 0) & R_MISO_2;
              if R_BIT = 7 then
                R_MOSI <= '1';
                R_DONE <= '1';
                R_STATE <= s_idle;
              else
                R_TX <= R_TX(6 downto 0) & '1';
                R_MOSI <= R_TX(6);
                R_BIT <= R_BIT + 1;
                R_CNT <= unsigned(DIV);
                R_STATE <= s_low;
              end if;
            else
              R_CNT <= R_CNT - 1;
            end if;
        end case;
      end if;
    end if;
  end process proc_regs;

  RXD <= R_RX;
  DONE <= R_DONE;
  BUSY <= '0' when R_STATE = s_idle else '1';
  O_SCLK <= R_SCLK;
  O_MOSI <= R_MOSI;

end syn;
