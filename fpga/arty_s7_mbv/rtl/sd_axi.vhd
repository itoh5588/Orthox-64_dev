-- SPDX-License-Identifier: MIT
-- Copyright (c) 2026 Akio Itoh
--
-- AXI4-Lite front end for the SD card SPI controller (sdspi_ctl/sdspi_phy).
-- Single block reads and writes; the CPU polls BUSY.
--
-- Registers (byte offsets, 32-bit):
--   0x000 ctrl/stat  w: bits 2:0 operation, started at once if not busy
--                       (1 init, 2 CID, 3 CSD, 4 read block, 6 write block)
--                    r: bit0 busy, bit1 init ok, bit2 SD v2, bit3 block
--                       addressing (SDHC/SDXC), bit4 card present,
--                       bits 15:8 error code, 23:16 last R1, 31:24 last token
--   0x004 lba        rw block number
--   0x008 fdiv       rw SPI phase length - 1 after init (>= 2)
--   0x00C resp       r  last R3/R7 response (OCR after init)
--   0x400-0x5FF rbuf r  last block read (or CID/CSD), little-endian words
--   0x600-0x7FF wbuf rw data for the next block write, little-endian words

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity sd_axi is
  generic (
    CLK_HZ  : natural := 50000000;
    SLOWDIV : natural := 63);           -- init: 50 MHz / 128 = 390 kHz
  port (
    s_axi_aclk    : in  std_logic;
    s_axi_aresetn : in  std_logic;
    s_axi_awaddr  : in  std_logic_vector(15 downto 0);
    s_axi_awvalid : in  std_logic;
    s_axi_awready : out std_logic;
    s_axi_wdata   : in  std_logic_vector(31 downto 0);
    s_axi_wstrb   : in  std_logic_vector(3 downto 0);
    s_axi_wvalid  : in  std_logic;
    s_axi_wready  : out std_logic;
    s_axi_bresp   : out std_logic_vector(1 downto 0);
    s_axi_bvalid  : out std_logic;
    s_axi_bready  : in  std_logic;
    s_axi_araddr  : in  std_logic_vector(15 downto 0);
    s_axi_arvalid : in  std_logic;
    s_axi_arready : out std_logic;
    s_axi_rdata   : out std_logic_vector(31 downto 0);
    s_axi_rresp   : out std_logic_vector(1 downto 0);
    s_axi_rvalid  : out std_logic;
    s_axi_rready  : in  std_logic;
    sd_cs_n       : out std_logic;
    sd_sclk       : out std_logic;
    sd_mosi       : out std_logic;
    sd_miso       : in  std_logic;
    sd_cd         : in  std_logic);    -- card detect, low = card present
end sd_axi;

architecture syn of sd_axi is

  constant MSEC_DIV : natural := CLK_HZ / 1000;

  type word_ram is array (0 to 127) of std_logic_vector(15 downto 0);
  type wbuf_ram is array (0 to 127) of std_logic_vector(31 downto 0);

  signal RESET    : std_logic;
  signal R_MSEC   : natural range 0 to MSEC_DIV - 1 := 0;
  signal CE_MSEC  : std_logic := '0';

  signal R_OP     : std_logic_vector(2 downto 0) := (others => '0');
  signal R_GO     : std_logic := '0';
  signal R_LBA    : std_logic_vector(31 downto 0) := (others => '0');
  signal R_FDIV   : std_logic_vector(7 downto 0) := x"02";

  signal BUSY, INITOK, V2, HC : std_logic;
  signal ERR, R1, TOKEN       : std_logic_vector(7 downto 0);
  signal RESP                 : std_logic_vector(31 downto 0);
  signal BUF_WE               : std_logic;
  signal BUF_ADDR, WBUF_ADDR  : std_logic_vector(7 downto 0);
  signal BUF_DI, WBUF_DATA    : std_logic_vector(15 downto 0);
  signal PHY_DIV, PHY_TXD, PHY_RXD : std_logic_vector(7 downto 0);
  signal PHY_START, PHY_DONE  : std_logic;

  -- rbuf as two 16-bit halves so the controller can write one word at a time
  signal RBUF_LO, RBUF_HI : word_ram := (others => (others => '0'));
  signal WBUF             : wbuf_ram := (others => (others => '0'));
  signal R_CD_1, R_CD_2   : std_logic := '1';

  signal R_AWREADY, R_WREADY, R_BVALID : std_logic := '0';
  signal R_ARREADY, R_RVALID           : std_logic := '0';
  signal R_RDATA                       : std_logic_vector(31 downto 0) := (others => '0');

begin

  RESET <= not s_axi_aresetn;

  CTL : entity work.sdspi_ctl
    generic map (SLOWDIV => SLOWDIV)
    port map (
      CLK => s_axi_aclk, RESET => RESET, CE_MSEC => CE_MSEC,
      OP => R_OP, GO => R_GO, LBA => R_LBA, NBLK => x"0000", HOLD => '0',
      ABORT => '0', FASTDIV => R_FDIV,
      BUSY => BUSY, INITOK => INITOK, V2 => V2, HC => HC,
      ERR => ERR, R1 => R1, TOKEN => TOKEN, RESP => RESP,
      BUF_WE => BUF_WE, BUF_ADDR => BUF_ADDR, BUF_DI => BUF_DI,
      WBUF_ADDR => WBUF_ADDR, WBUF_DATA => WBUF_DATA,
      O_CS_N => sd_cs_n, PHY_DIV => PHY_DIV, PHY_START => PHY_START,
      PHY_TXD => PHY_TXD, PHY_RXD => PHY_RXD, PHY_DONE => PHY_DONE);

  PHY : entity work.sdspi_phy
    port map (
      CLK => s_axi_aclk, RESET => RESET, DIV => PHY_DIV, START => PHY_START,
      TXD => PHY_TXD, RXD => PHY_RXD, DONE => PHY_DONE, BUSY => open,
      O_SCLK => sd_sclk, O_MOSI => sd_mosi, I_MISO => sd_miso);

  -- write data: 16-bit word WBUF_ADDR out of the 32-bit wbuf words (async read)
  WBUF_DATA <= WBUF(to_integer(unsigned(WBUF_ADDR(7 downto 1))))(15 downto 0)
               when WBUF_ADDR(0) = '0' else
               WBUF(to_integer(unsigned(WBUF_ADDR(7 downto 1))))(31 downto 16);

  proc_msec: process (s_axi_aclk)
  begin
    if rising_edge(s_axi_aclk) then
      CE_MSEC <= '0';
      if R_MSEC = MSEC_DIV - 1 then
        R_MSEC <= 0;
        CE_MSEC <= '1';
      else
        R_MSEC <= R_MSEC + 1;
      end if;
      R_CD_1 <= sd_cd;
      R_CD_2 <= R_CD_1;
    end if;
  end process proc_msec;

  proc_rbuf: process (s_axi_aclk)
  begin
    if rising_edge(s_axi_aclk) then
      if BUF_WE = '1' then
        if BUF_ADDR(0) = '0' then
          RBUF_LO(to_integer(unsigned(BUF_ADDR(7 downto 1)))) <= BUF_DI;
        else
          RBUF_HI(to_integer(unsigned(BUF_ADDR(7 downto 1)))) <= BUF_DI;
        end if;
      end if;
    end if;
  end process proc_rbuf;

  -- AXI4-Lite write: raise AWREADY/WREADY when both are valid, write and
  -- raise BVALID in the cycle after the handshake
  proc_write: process (s_axi_aclk)
    variable a : unsigned(15 downto 0);
  begin
    if rising_edge(s_axi_aclk) then
      R_GO <= '0';
      if s_axi_aresetn = '0' then
        R_AWREADY <= '0';
        R_WREADY <= '0';
        R_BVALID <= '0';
        R_FDIV <= x"02";
      else
        if R_BVALID = '1' and s_axi_bready = '1' then
          R_BVALID <= '0';
        end if;
        if R_AWREADY = '1' then           -- AW and W handshakes happen now
          R_AWREADY <= '0';
          R_WREADY <= '0';
          R_BVALID <= '1';
          a := unsigned(s_axi_awaddr);
          if a(11 downto 9) = "011" then                 -- 0x600-0x7FF wbuf
            WBUF(to_integer(a(8 downto 2))) <= s_axi_wdata;
          else
            case to_integer(a(11 downto 0)) is
              when 16#000# =>
                if BUSY = '0' then
                  R_OP <= s_axi_wdata(2 downto 0);
                  R_GO <= '1';
                end if;
              when 16#004# => R_LBA <= s_axi_wdata;
              when 16#008# => R_FDIV <= s_axi_wdata(7 downto 0);
              when others => null;
            end case;
          end if;
        elsif s_axi_awvalid = '1' and s_axi_wvalid = '1' and R_BVALID = '0' then
          R_AWREADY <= '1';
          R_WREADY <= '1';
        end if;
      end if;
    end if;
  end process proc_write;

  -- AXI4-Lite read: raise ARREADY, then present the data on R
  proc_read: process (s_axi_aclk)
    variable a : unsigned(15 downto 0);
    variable i : natural range 0 to 127;
  begin
    if rising_edge(s_axi_aclk) then
      if s_axi_aresetn = '0' then
        R_ARREADY <= '0';
        R_RVALID <= '0';
      else
        if R_RVALID = '1' and s_axi_rready = '1' then
          R_RVALID <= '0';
        end if;
        if R_ARREADY = '1' then           -- AR handshake happens now
          R_ARREADY <= '0';
          R_RVALID <= '1';
          a := unsigned(s_axi_araddr);
          i := to_integer(a(8 downto 2));
          if a(11 downto 9) = "010" then                 -- 0x400-0x5FF rbuf
            R_RDATA <= RBUF_HI(i) & RBUF_LO(i);
          elsif a(11 downto 9) = "011" then              -- 0x600-0x7FF wbuf
            R_RDATA <= WBUF(i);
          else
            case to_integer(a(11 downto 0)) is
              when 16#000# =>
                R_RDATA <= TOKEN & R1 & ERR & "000" & (not R_CD_2) &
                           HC & V2 & INITOK & (BUSY or R_GO);
              when 16#004# => R_RDATA <= R_LBA;
              when 16#008# => R_RDATA <= x"000000" & R_FDIV;
              when 16#00C# => R_RDATA <= RESP;
              when others  => R_RDATA <= (others => '0');
            end case;
          end if;
        elsif s_axi_arvalid = '1' and R_RVALID = '0' then
          R_ARREADY <= '1';
        end if;
      end if;
    end if;
  end process proc_read;

  s_axi_awready <= R_AWREADY;
  s_axi_wready  <= R_WREADY;
  s_axi_bresp   <= "00";
  s_axi_bvalid  <= R_BVALID;
  s_axi_arready <= R_ARREADY;
  s_axi_rdata   <= R_RDATA;
  s_axi_rresp   <= "00";
  s_axi_rvalid  <= R_RVALID;

end syn;
