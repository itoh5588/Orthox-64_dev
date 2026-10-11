-- SPDX-License-Identifier: MIT
-- Copyright (c) 2026 Akio Itoh
--
-- Ported from the author's Arty S7 standalone w11 (artys7_standalone/sdspi_ctl.vhd),
-- with the w11 slvtypes subtypes replaced by plain std_logic(_vector).
--
-- SD card SPI command controller: card initialization, block reads and
-- single or multi-block writes.  The card is only written by op_write and
-- op_mwrite.
--
-- Operations (OP, started by GO while not BUSY):
--   op_init  power-up clocks, CMD0, CMD8, CMD55+ACMD41, CMD58, CMD16 (SDSC)
--   op_cid   CMD10, 16 bytes to the buffer
--   op_csd   CMD9,  16 bytes to the buffer
--   op_read  CMD17, 512 bytes of block LBA to the buffer
--   op_mread CMD18, NBLK blocks from LBA (0 means 1), stopped by CMD12
--   op_write CMD24, 512 bytes from WBUF (word WBUF_ADDR, little-endian) to
--            block LBA; data response and busy end are checked, then the
--            card status by CMD13
--   op_mwrite CMD25, NBLK blocks (0 means 1) to LBA on; WBLK tells which
--            block of the caller's buffer is being sent.  Each block is
--            checked like op_write, then the stop token and its busy end,
--            then CMD13 (2026-10-11, Orthox).  On an error the controller
--            only releases CS; the caller should initialize the card again.
-- The data words leave on BUF_* as little-endian 16-bit words (PDP-11 byte
-- order), BUF_ADDR is the word index inside the block.  While HOLD is set
-- no new data byte is clocked in, so a slow consumer never loses data.
-- Commands carry a valid CRC7; received data blocks are checked by CRC16.
--
-- ERR codes: 0 ok, 1 no R1 response, 2 CMD0 did not enter idle, 3 CMD8 bad
-- echo, 4 ACMD41 timeout, 5 R1 error (see R1), 6 data token timeout, 7 data
-- error token (see TOKEN), 8 data CRC error, 9 not initialized, 10 CMD12
-- busy timeout, 11 aborted, 12 write data CRC rejected, 13 write error
-- (data response), 14 write busy timeout, 15 CMD13 status error.  A failing or aborted multi-block read is still
-- stopped by CMD12.  ABORT ends a read while waiting for or receiving data.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;


entity sdspi_ctl is
  generic (
    SLOWDIV : natural := 99);           -- init clock: 75 MHz/200 = 375 kHz
  port (
    CLK : in std_logic;
    RESET : in std_logic;
    CE_MSEC : in std_logic;
    OP : in std_logic_vector(2 downto 0);
    GO : in std_logic;
    LBA : in std_logic_vector(31 downto 0);
    NBLK : in std_logic_vector(15 downto 0);
    HOLD : in std_logic;
    ABORT : in std_logic;
    FASTDIV : in std_logic_vector(7 downto 0);
    BUSY : out std_logic;
    INITOK : out std_logic;
    V2 : out std_logic;                     -- CMD8 accepted (SD v2)
    HC : out std_logic;                     -- block addressing (SDHC/SDXC)
    ERR : out std_logic_vector(7 downto 0);
    R1 : out std_logic_vector(7 downto 0);
    TOKEN : out std_logic_vector(7 downto 0);
    RESP : out std_logic_vector(31 downto 0);
    BUF_WE : out std_logic;
    BUF_ADDR : out std_logic_vector(7 downto 0);
    BUF_DI : out std_logic_vector(15 downto 0);
    WBUF_ADDR : out std_logic_vector(7 downto 0);               -- write data: word index
    WBUF_DATA : in std_logic_vector(15 downto 0);               -- write data: word (async read)
    WBLK : out std_logic_vector(2 downto 0);                    -- op_mwrite: block being sent
    O_CS_N : out std_logic;
    PHY_DIV : out std_logic_vector(7 downto 0);
    PHY_START : out std_logic;
    PHY_TXD : out std_logic_vector(7 downto 0);
    PHY_RXD : in std_logic_vector(7 downto 0);
    PHY_DONE : in std_logic
  );
end sdspi_ctl;

architecture syn of sdspi_ctl is

  constant op_init : std_logic_vector(2 downto 0) := "001";
  constant op_cid : std_logic_vector(2 downto 0) := "010";
  constant op_csd : std_logic_vector(2 downto 0) := "011";
  constant op_read : std_logic_vector(2 downto 0) := "100";
  constant op_mread : std_logic_vector(2 downto 0) := "101";
  constant op_mwrite : std_logic_vector(2 downto 0) := "111";
  constant op_write : std_logic_vector(2 downto 0) := "110";

  constant err_noresp : std_logic_vector(7 downto 0) := x"01";
  constant err_cmd0 : std_logic_vector(7 downto 0) := x"02";
  constant err_cmd8 : std_logic_vector(7 downto 0) := x"03";
  constant err_acmd41 : std_logic_vector(7 downto 0) := x"04";
  constant err_r1 : std_logic_vector(7 downto 0) := x"05";
  constant err_tokto : std_logic_vector(7 downto 0) := x"06";
  constant err_token : std_logic_vector(7 downto 0) := x"07";
  constant err_crc : std_logic_vector(7 downto 0) := x"08";
  constant err_noinit : std_logic_vector(7 downto 0) := x"09";
  constant err_stop : std_logic_vector(7 downto 0) := x"0a";
  constant err_abort : std_logic_vector(7 downto 0) := x"0b";
  constant err_wcrc : std_logic_vector(7 downto 0) := x"0c";
  constant err_werr : std_logic_vector(7 downto 0) := x"0d";
  constant err_wbusy : std_logic_vector(7 downto 0) := x"0e";
  constant err_wstat : std_logic_vector(7 downto 0) := x"0f";

  type state_type is (
    s_idle, s_pwr,
    s_c_pre, s_c_send, s_c_r1, s_c_resp, s_c_tok, s_c_data, s_c_crc,
    s_c_end, s_stop, s_stop_skip, s_stop_r1, s_stop_busy,
    s_i_cmd0, s_i_cmd0_chk, s_i_cmd8_chk, s_i_cmd55_chk, s_i_cmd41_chk,
    s_i_cmd58_chk, s_i_cmd16_chk, s_rd_chk,
    s_w_gap, s_w_tok, s_w_data, s_w_crc, s_w_resp, s_w_busy, s_w_status,
    s_w_chk, s_w_stop, s_w_stop_gap, s_w_stop_busy, s_done);

  type regs_type is record
    state : state_type;
    ret : state_type;                   -- state after s_c_end
    xact : std_logic;                       -- byte transfer running
    cs_n : std_logic;
    fast : std_logic;
    cmd : std_logic_vector(47 downto 0);
    nresp : std_logic;                      -- 4 response bytes follow R1
    ndata : unsigned(9 downto 0);       -- data bytes, 0: no data phase
    cnt : unsigned(9 downto 0);
    tries : unsigned(3 downto 0);
    tmo : unsigned(9 downto 0);         -- ms counter
    crc : std_logic_vector(15 downto 0);
    crcrx : std_logic_vector(15 downto 0);
    lowbyte : std_logic_vector(7 downto 0);
    busy : std_logic;
    initok : std_logic;
    v2 : std_logic;
    hc : std_logic;
    err : std_logic_vector(7 downto 0);
    r1 : std_logic_vector(7 downto 0);
    token : std_logic_vector(7 downto 0);
    resp : std_logic_vector(31 downto 0);
    multi : std_logic;                      -- multi-block read running
    stopping : std_logic;                   -- CMD12 being sent
    nleft : unsigned(15 downto 0);      -- blocks still to read
    wr : std_logic;                         -- CMD24: data phase follows R1
    mw : std_logic;                         -- CMD25 running
    wblk : unsigned(2 downto 0);        -- CMD25: block of WBUF being sent
  end record regs_type;

  constant regs_init : regs_type := (
    s_idle, s_idle, '0', '1', '0', (others => '1'), '0',
    (others => '0'), (others => '0'), (others => '0'), (others => '0'),
    (others => '0'), (others => '0'), (others => '0'),
    '0', '0', '0', '0', (others => '0'), x"ff", x"ff", (others => '0'),
    '0', '0', (others => '0'), '0', '0', (others => '0'));

  signal R_REGS : regs_type := regs_init;
  signal N_REGS : regs_type := regs_init;

  function crc7(d : std_logic_vector(39 downto 0)) return std_logic_vector is
    variable c : std_logic_vector(6 downto 0) := (others => '0');
    variable fb : std_logic;
  begin
    for i in 39 downto 0 loop
      fb := c(6) xor d(i);
      c := c(5 downto 0) & '0';
      if fb = '1' then
        c := c xor "0001001";
      end if;
    end loop;
    return c;
  end function crc7;

  function crc16(c_in : std_logic_vector(15 downto 0); d : std_logic_vector(7 downto 0)) return std_logic_vector is
    variable c : std_logic_vector(15 downto 0) := c_in;
    variable fb : std_logic;
  begin
    for i in 7 downto 0 loop
      fb := c(15) xor d(i);
      c := c(14 downto 0) & '0';
      if fb = '1' then
        c := c xor x"1021";
      end if;
    end loop;
    return c;
  end function crc16;

begin

  proc_regs: process (CLK)
  begin
    if rising_edge(CLK) then
      if RESET = '1' then
        R_REGS <= regs_init;
      else
        R_REGS <= N_REGS;
      end if;
    end if;
  end process proc_regs;

  proc_next: process (R_REGS, CE_MSEC, OP, GO, LBA, NBLK, HOLD, ABORT,
                      FASTDIV, WBUF_DATA,
                      PHY_RXD, PHY_DONE)
    variable r : regs_type := regs_init;
    variable n : regs_type := regs_init;
    variable istart : std_logic := '0';
    variable itxd : std_logic_vector(7 downto 0) := (others => '1');
    variable ibufwe : std_logic := '0';
    variable idiv : std_logic_vector(7 downto 0) := (others => '0');
    variable rx : std_logic_vector(7 downto 0) := (others => '1');
    variable bdone : boolean := false;  -- byte transfer finished this cycle
    variable wbyte : std_logic_vector(7 downto 0) := (others => '0');

    -- start a command; after it (and its CS release) continue in ret
    procedure command(constant idx : in natural;
                      constant arg : in std_logic_vector(31 downto 0);
                      constant resp4 : in std_logic;
                      constant ndata : in natural;
                      constant ret : in state_type) is
      variable c40 : std_logic_vector(39 downto 0);
    begin
      c40 := "01" & std_logic_vector(to_unsigned(idx, 6)) & arg;
      n.cmd := c40 & crc7(c40) & '1';
      n.nresp := resp4;
      n.ndata := to_unsigned(ndata, 10);
      n.ret := ret;
      n.state := s_c_pre;
    end procedure command;

    -- abort: record the error, stop a multi-block read, release CS
    procedure fail(constant code : in std_logic_vector(7 downto 0)) is
    begin
      if r.err = x"00" then
        n.err := code;
      end if;
      n.ret := s_done;
      if r.multi = '1' and r.stopping = '0' then
        n.state := s_stop;
      else
        n.state := s_c_end;
      end if;
    end procedure fail;

    -- issue one byte, bdone signals completion in a later cycle
    procedure xfer(constant data : in std_logic_vector(7 downto 0)) is
    begin
      if r.xact = '0' then
        istart := '1';
        itxd := data;
        n.xact := '1';
      end if;
    end procedure xfer;

  begin
    r := R_REGS;
    n := R_REGS;

    istart := '0';
    itxd := x"ff";
    ibufwe := '0';
    rx := PHY_RXD;
    bdone := r.xact = '1' and PHY_DONE = '1';
    if bdone then
      n.xact := '0';
    end if;
    if CE_MSEC = '1' and r.tmo /= "1111111111" then
      n.tmo := r.tmo + 1;
    end if;

    case r.state is

      when s_idle =>
        if GO = '1' then
          n.err := (others => '0');
          n.busy := '1';
          n.multi := '0';
          n.stopping := '0';
          n.wr := '0';
          n.mw := '0';
          n.wblk := (others => '0');
          case OP is
            when op_init =>
              n.initok := '0';
              n.v2 := '0';
              n.hc := '0';
              n.fast := '0';
              n.cs_n := '1';
              n.cnt := to_unsigned(10, 10); -- >= 74 clocks with CS high
              n.state := s_pwr;
            when op_write =>
              if r.initok = '0' then
                n.err := err_noinit;
                n.busy := '0';
              else
                n.wr := '1';
                if r.hc = '1' then
                  command(24, LBA, '0', 0, s_done);
                else
                  command(24, LBA(22 downto 0) & "000000000", '0', 0, s_done);
                end if;
              end if;
            when op_mwrite =>
              if r.initok = '0' then
                n.err := err_noinit;
                n.busy := '0';
              else
                n.wr := '1';
                n.mw := '1';
                n.nleft := unsigned(NBLK);
                if NBLK = x"0000" then
                  n.nleft := x"0001";
                end if;
                if r.hc = '1' then
                  command(25, LBA, '0', 0, s_done);
                else
                  command(25, LBA(22 downto 0) & "000000000", '0', 0, s_done);
                end if;
              end if;
            when op_cid | op_csd | op_read | op_mread =>
              if r.initok = '0' then
                n.err := err_noinit;
                n.busy := '0';
              elsif OP = op_cid then
                command(10, x"00000000", '0', 16, s_rd_chk);
              elsif OP = op_csd then
                command(9, x"00000000", '0', 16, s_rd_chk);
              elsif OP = op_mread then
                n.multi := '1';
                n.nleft := unsigned(NBLK);
                if NBLK = x"0000" then
                  n.nleft := x"0001";
                end if;
                if r.hc = '1' then
                  command(18, LBA, '0', 512, s_rd_chk);
                else
                  command(18, LBA(22 downto 0) & "000000000", '0', 512,
                          s_rd_chk);
                end if;
              elsif r.hc = '1' then
                command(17, LBA, '0', 512, s_rd_chk);
              else
                command(17, LBA(22 downto 0) & "000000000", '0', 512,
                        s_rd_chk);
              end if;
            when others =>
              n.busy := '0';
          end case;
        end if;

      when s_pwr =>
        xfer(x"ff");
        if bdone then
          if r.cnt = 1 then
            n.tries := (others => '0');
            n.state := s_i_cmd0;
          else
            n.cnt := r.cnt - 1;
          end if;
        end if;

      -- generic command transaction ----------------------------------------
      when s_c_pre =>                   -- CS low, one idle byte
        n.cs_n := '0';
        xfer(x"ff");
        if bdone then
          n.cnt := (others => '0');
          n.state := s_c_send;
        end if;

      when s_c_send =>
        xfer(r.cmd(47 downto 40));
        if bdone then
          n.cmd := r.cmd(39 downto 0) & x"ff";
          if r.cnt = 5 then
            n.cnt := (others => '0');
            if r.stopping = '1' then
              n.state := s_stop_skip;
            else
              n.state := s_c_r1;
            end if;
          else
            n.cnt := r.cnt + 1;
          end if;
        end if;

      when s_c_r1 =>                    -- NCR: R1 within 8 bytes, allow 16
        xfer(x"ff");
        if bdone then
          if rx(7) = '0' then
            n.r1 := rx;
            n.cnt := (others => '0');
            if r.wr = '1' then            -- CMD24: data block follows
              if rx /= x"00" then
                fail(err_r1);
              else
                n.state := s_w_gap;
              end if;
            elsif r.nresp = '1' then
              n.state := s_c_resp;
            elsif r.ndata /= 0 then
              if rx /= x"00" then
                fail(err_r1);
              else
                n.tmo := (others => '0');
                n.state := s_c_tok;
              end if;
            else
              n.state := s_c_end;
            end if;
          elsif r.cnt = 15 then
            n.r1 := rx;
            fail(err_noresp);
          else
            n.cnt := r.cnt + 1;
          end if;
        end if;

      when s_c_resp =>                  -- R3/R7: 4 more bytes
        xfer(x"ff");
        if bdone then
          n.resp := r.resp(23 downto 0) & rx;
          if r.cnt = 3 then
            n.state := s_c_end;
          else
            n.cnt := r.cnt + 1;
          end if;
        end if;

      when s_c_tok =>                   -- wait for data token, 100 ms
        if ABORT = '1' and r.xact = '0' then
          fail(err_abort);
        else
          xfer(x"ff");
        end if;
        if bdone then
          if rx = x"fe" then
            n.token := rx;
            n.crc := (others => '0');
            n.cnt := (others => '0');
            n.state := s_c_data;
          elsif rx /= x"ff" then
            n.token := rx;
            fail(err_token);
          elsif r.tmo >= 100 then
            fail(err_tokto);
          end if;
        end if;

      when s_c_data =>
        if ABORT = '1' and r.xact = '0' then
          fail(err_abort);
        elsif HOLD = '0' then             -- consumer has room for a word
          xfer(x"ff");
        end if;
        if bdone then
          n.crc := crc16(r.crc, rx);
          if r.cnt(0) = '0' then
            n.lowbyte := rx;
          else
            ibufwe := '1';
          end if;
          if r.cnt = r.ndata - 1 then
            n.cnt := (others => '0');
            n.state := s_c_crc;
          else
            n.cnt := r.cnt + 1;
          end if;
        end if;

      when s_c_crc =>
        xfer(x"ff");
        if bdone then
          n.crcrx := r.crcrx(7 downto 0) & rx;
          if r.cnt = 1 then
            if r.crcrx(7 downto 0) & rx /= r.crc then
              fail(err_crc);
            elsif r.multi = '0' then
              n.state := s_c_end;
            elsif r.nleft > 1 then        -- next block of CMD18
              n.nleft := r.nleft - 1;
              n.cnt := (others => '0');
              n.tmo := (others => '0');
              n.state := s_c_tok;
            else
              n.state := s_stop;
            end if;
          else
            n.cnt := r.cnt + 1;
          end if;
        end if;

      when s_c_end =>                   -- CS high, 8 clocks to release DO
        n.cs_n := '1';
        xfer(x"ff");
        if bdone then
          n.state := r.ret;
        end if;

      -- CMD24 data phase ----------------------------------------------------
      when s_w_gap =>                   -- NWR: at least one byte
        xfer(x"ff");
        if bdone then
          n.state := s_w_tok;
        end if;

      when s_w_tok =>                   -- start block token (CMD25: fc)
        if r.mw = '1' then
          xfer(x"fc");
        else
          xfer(x"fe");
        end if;
        if bdone then
          n.cnt := (others => '0');
          n.crc := (others => '0');
          n.state := s_w_data;
        end if;

      when s_w_data =>                  -- 512 bytes, low byte of word first
        if r.cnt(0) = '0' then
          wbyte := WBUF_DATA(7 downto 0);
        else
          wbyte := WBUF_DATA(15 downto 8);
        end if;
        xfer(wbyte);
        if bdone then
          n.crc := crc16(r.crc, wbyte);
          if r.cnt = 511 then
            n.cnt := (others => '0');
            n.state := s_w_crc;
          else
            n.cnt := r.cnt + 1;
          end if;
        end if;

      when s_w_crc =>
        if r.cnt(0) = '0' then
          xfer(r.crc(15 downto 8));
        else
          xfer(r.crc(7 downto 0));
        end if;
        if bdone then
          if r.cnt(0) = '1' then
            n.cnt := (others => '0');
            n.state := s_w_resp;
          else
            n.cnt := r.cnt + 1;
          end if;
        end if;

      when s_w_resp =>                  -- data response xxx0sss1
        xfer(x"ff");
        if bdone then
          if rx /= x"ff" then
            n.token := rx;
            if rx(4 downto 0) = "00101" then
              n.tmo := (others => '0');
              n.state := s_w_busy;
            elsif rx(4 downto 0) = "01011" then
              fail(err_wcrc);
            else
              fail(err_werr);
            end if;
          elsif r.cnt = 15 then
            fail(err_werr);
          else
            n.cnt := r.cnt + 1;
          end if;
        end if;

      when s_w_busy =>                  -- card holds DO low while writing
        xfer(x"ff");
        if bdone then
          if rx /= x"00" then
            if r.mw = '1' and r.nleft > 1 then  -- next block of CMD25
              n.nleft := r.nleft - 1;
              n.wblk := r.wblk + 1;
              n.cnt := (others => '0');
              n.state := s_w_gap;
            elsif r.mw = '1' then
              n.state := s_w_stop;
            else
              n.ret := s_w_status;
              n.state := s_c_end;
            end if;
          elsif r.tmo >= 1000 then
            fail(err_wbusy);
          end if;
        end if;

      -- CMD25 end: stop token, one byte (Nbr), then busy ---------------------
      when s_w_stop =>
        xfer(x"fd");
        if bdone then
          n.state := s_w_stop_gap;
        end if;

      when s_w_stop_gap =>
        xfer(x"ff");
        if bdone then
          n.tmo := (others => '0');
          n.state := s_w_stop_busy;
        end if;

      when s_w_stop_busy =>
        xfer(x"ff");
        if bdone then
          if rx /= x"00" then
            n.mw := '0';
            n.ret := s_w_status;
            n.state := s_c_end;
          elsif r.tmo >= 1000 then
            fail(err_wbusy);
          end if;
        end if;

      when s_w_status =>                -- CMD13: R2 = R1 + status byte
        n.wr := '0';
        command(13, x"00000000", '1', 0, s_w_chk);

      when s_w_chk =>
        if r.err /= x"00" then
          n.state := s_done;
        elsif r.r1 /= x"00" or r.resp(31 downto 24) /= x"00" then
          fail(err_wstat);
        else
          n.state := s_done;
        end if;

      -- CMD12: stop a multi-block read (R1b) ---------------------------------
      when s_stop =>
        n.stopping := '1';
        n.cmd := "01" & "001100" & x"00000000" &
                 crc7("01" & "001100" & x"00000000") & '1';
        n.cnt := (others => '0');
        n.state := s_c_send;

      when s_stop_skip =>               -- stuff byte after CMD12
        xfer(x"ff");
        if bdone then
          n.cnt := (others => '0');
          n.state := s_stop_r1;
        end if;

      when s_stop_r1 =>
        xfer(x"ff");
        if bdone then
          if rx(7) = '0' then
            n.tmo := (others => '0');
            n.state := s_stop_busy;
          elsif r.cnt = 15 then
            if r.err = x"00" then
              n.err := err_noresp;
            end if;
            n.stopping := '0';
            n.state := s_c_end;
          else
            n.cnt := r.cnt + 1;
          end if;
        end if;

      when s_stop_busy =>               -- card holds DO low while busy
        xfer(x"ff");
        if bdone then
          if rx /= x"00" then
            n.stopping := '0';
            n.multi := '0';
            n.state := s_c_end;
          elsif r.tmo >= 500 then
            if r.err = x"00" then
              n.err := err_stop;
            end if;
            n.stopping := '0';
            n.state := s_c_end;
          end if;
        end if;

      -- initialization -------------------------------------------------------
      when s_i_cmd0 =>
        command(0, x"00000000", '0', 0, s_i_cmd0_chk);

      when s_i_cmd0_chk =>
        if r.err /= x"00" then
          n.state := s_done;
        elsif r.r1 = x"01" then
          command(8, x"000001aa", '1', 0, s_i_cmd8_chk);
        elsif r.tries = 7 then
          fail(err_cmd0);
        else
          n.tries := r.tries + 1;
          n.state := s_i_cmd0;
        end if;

      when s_i_cmd8_chk =>
        if r.err /= x"00" then
          n.state := s_done;
        elsif r.r1 = x"01" then
          if r.resp(11 downto 0) = x"1aa" then
            n.v2 := '1';
            n.tmo := (others => '0');
            command(55, x"00000000", '0', 0, s_i_cmd55_chk);
          else
            fail(err_cmd8);
          end if;
        elsif r.r1(2) = '1' then        -- illegal command: SD v1
          n.v2 := '0';
          n.tmo := (others => '0');
          command(55, x"00000000", '0', 0, s_i_cmd55_chk);
        else
          fail(err_r1);
        end if;

      when s_i_cmd55_chk =>
        if r.err /= x"00" then
          n.state := s_done;
        elsif r.r1(7 downto 1) /= "0000000" then
          fail(err_r1);
        elsif r.v2 = '1' then
          command(41, x"40000000", '0', 0, s_i_cmd41_chk);
        else
          command(41, x"00000000", '0', 0, s_i_cmd41_chk);
        end if;

      when s_i_cmd41_chk =>
        if r.err /= x"00" then
          n.state := s_done;
        elsif r.r1 = x"00" then
          if r.v2 = '1' then
            command(58, x"00000000", '1', 0, s_i_cmd58_chk);
          else
            command(16, x"00000200", '0', 0, s_i_cmd16_chk);
          end if;
        elsif r.r1 /= x"01" then
          fail(err_r1);
        elsif r.tmo >= 1000 then
          fail(err_acmd41);
        else
          command(55, x"00000000", '0', 0, s_i_cmd55_chk);
        end if;

      when s_i_cmd58_chk =>
        if r.err /= x"00" then
          n.state := s_done;
        elsif r.r1 /= x"00" then
          fail(err_r1);
        elsif r.resp(30) = '1' then     -- CCS: block addressing
          n.hc := '1';
          n.fast := '1';
          n.initok := '1';
          n.state := s_done;
        else
          command(16, x"00000200", '0', 0, s_i_cmd16_chk);
        end if;

      when s_i_cmd16_chk =>
        if r.err /= x"00" then
          n.state := s_done;
        elsif r.r1 /= x"00" then
          fail(err_r1);
        else
          n.fast := '1';
          n.initok := '1';
          n.state := s_done;
        end if;

      when s_rd_chk =>
        n.state := s_done;

      when s_done =>
        n.busy := '0';
        n.multi := '0';
        n.mw := '0';
        n.stopping := '0';
        n.state := s_idle;

    end case;

    idiv := std_logic_vector(to_unsigned(SLOWDIV, 8));
    if r.fast = '1' then
      idiv := FASTDIV;
      -- >= 1 (2026-10-11, was >= 2): at DIV = 1 the PHY still samples MISO
      -- at the rising edge, two cycles (40 ns at 50 MHz) after the card
      -- changed it on the falling edge (see sdspi_phy).  DIV = 0 would sample
      -- at the change itself.
      if unsigned(FASTDIV) < 1 then
        idiv := x"01";
      end if;
    end if;

    N_REGS <= n;

    BUSY <= r.busy;
    INITOK <= r.initok;
    V2 <= r.v2;
    HC <= r.hc;
    ERR <= r.err;
    R1 <= r.r1;
    TOKEN <= r.token;
    RESP <= r.resp;
    BUF_WE <= ibufwe;
    BUF_ADDR <= std_logic_vector(r.cnt(8 downto 1));
    BUF_DI <= rx & r.lowbyte;
    WBUF_ADDR <= std_logic_vector(r.cnt(8 downto 1));
    WBLK <= std_logic_vector(r.wblk);
    O_CS_N <= r.cs_n;
    PHY_DIV <= idiv;
    PHY_START <= istart;
    PHY_TXD <= itxd;
  end process proc_next;

end syn;
