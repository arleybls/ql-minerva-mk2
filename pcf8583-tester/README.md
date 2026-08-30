# PCF8583 tester

Go/no-go tester for PCF8583 real-time-clock/RAM chips, built to screen chips
before fitting one to the QL Minerva MK2 board.

Insert a chip in the socket, the test runs by itself and prints a checklist and
a verdict on the serial port. Remove the chip, insert the next one.

## Contents

| Path | What |
|---|---|
| `RTC Tester.kicad_sch` / `.kicad_pcb` / `.kicad_pro` | KiCad 9 project: Arduino Nano, DIP-8 socket for the PCF8583, two 2.2 kΩ I2C pull-ups |
| `gerbers/` | Fabrication files for the board above |
| `pcf8583-tester-sketch/pcf8583-tester-sketch.ino` | Arduino sketch |

## Hardware

Board as designed:

| PCF8583 pin | Net |
|---|---|
| 1 OSCI | not connected |
| 2 OSCO | not connected |
| 3 A0 | not connected |
| 4 VSS | GND |
| 5 SDA | Nano A4, 2.2 kΩ to +5 V |
| 6 SCL | Nano A5, 2.2 kΩ to +5 V |
| 7 INT | not connected |
| 8 VDD | +5 V |

Two bodge wires are needed on the current board revision:

1. **A0 to VSS** — socket pin 3 to pin 4. Without it A0 floats, the chip's
   I2C address flickers between 0x50 and 0x51 and results are unreliable.
2. **Nano D9 to OSCI** — socket pin 1. The Nano generates the clock that
   drives the chip's counter chain (see *Driven tests* below). Optional: without
   it set `HAS_OSC_DRIVE` to 0 and the driven tests are skipped.

There is no crystal and no battery on this board, so the chip's own oscillator
and battery retention are not tested. See *Limits*.

## Flashing the Nano

Arduino IDE 2:

1. Open `pcf8583-tester-sketch/pcf8583-tester-sketch.ino`.
2. Tools → Board → Arduino AVR Boards → **Arduino Nano**.
3. Tools → Processor → **ATmega328P (Old Bootloader)** for a clone Nano
   (plain **ATmega328P** for a genuine one; switch if upload reports "not in sync").
4. Tools → Port → the Nano's COM port.
5. Upload.

Or with `arduino-cli`:

```
arduino-cli compile --upload --fqbn arduino:avr:nano:cpu=atmega328old -p COMx pcf8583-tester-sketch
```

Only the built-in `Wire` library is used.

## Using it

1. Open the Serial Monitor at **115200 baud**. You should see:

   ```
   PCF8583 tester - insert chip
   (HAS_CRYSTAL=0: oscillator test disabled on this board)
   (OSCI driven from D9 at 5000 Hz = 100x real time)
   ```

   While no chip is present a `.` is printed every 5 s as a heartbeat.

2. Insert a chip, pin 1 towards the socket notch. The test starts within
   0.5 s and takes about 3 s. Output:

   ```
   === chip detected at 0x50 ===
   progress [######################################] done

     [✓] i2c     bus detection  -> ACK at 0x50 (A0 low)
     [✓] 0x00    uniform pattern
     [✓] 0xFF    uniform pattern
     [✓] 0x55    uniform pattern
     [✓] 0xAA    uniform pattern
     [✓] walk1   walking 1, bit coupling
     [✓] walk0   walking 0, bit coupling
     [✓] addr    address in cell, aliasing
     [✓] ~addr   inverse address in cell
     [✓] hash    pseudo-random pattern
     [✓] hold    data hold 2 s
     [✓] clkreg  clock regs 0x01-0x07 as RAM
     [✓] wrap    address wrap 0xFF -> 0x00
     [✓] date    set date/time, counter running  -> read back 30/08 y+2 wd6 12:34:56
     [✓] roll    rollovers: min/hour/month/leap/year
     [✓] alarm   daily alarm flag
     [✓] timer   timer counts seconds
     [-] TIME    seconds advance  -> not tested (no crystal on this board)
     [✓] clean   chip zeroed

   CHIP @0x50: PASS (18 tests, oscillator not tested)
   ```

   followed by a large "passed!" banner. A failing chip prints
   `CHIP @0x50: FAIL (n of 18 tests failed)` instead, with details on the
   failing rows (see *Reading failures*).

3. Remove the chip. The tester prints `insert next chip` and re-arms.

The chip is hot-plugged with the Nano powered. That is normally harmless for
this CMOS part; if you prefer, power the Nano off between chips — the test
runs on power-up the same way.

## What is tested

The chip's memory map: 0x00 control/status, 0x01–0x07 clock counters,
0x08–0x0F alarm registers, 0x10–0xFF 240 bytes free RAM. Everything from 0x08
up is tested as RAM (the Minerva ROM may use the alarm area as storage).

### I2C

| Row | Check |
|---|---|
| `i2c` | Chip ACKs at 0x50 (A0 low) or 0x51 (A0 high). Answering at **both** is a fault: A0 floating or the chip's A0 input broken. The address found is reported. |

Every read and write in every test checks the I2C result. A NAK or short read
marks that test `BUS ERROR @0x..`, and the remaining tests are skipped, so a
bus problem can never be mistaken for a memory fault.

### RAM march, 0x08–0xFF (248 bytes)

Each pass writes the whole range then reads it all back.

| Row | Pattern | Catches |
|---|---|---|
| `0x00` `0xFF` `0x55` `0xAA` | uniform | stuck bits, shorted adjacent bits |
| `walk1` | one bit set per byte, rotating with address | bit-to-bit coupling within a byte |
| `walk0` | one bit cleared per byte, rotating | same, opposite polarity |
| `addr` | each cell holds its own address | address-line faults: two addresses hitting one cell |
| `~addr` | inverse of its address | same, opposite polarity |
| `hash` | pseudo-random function of the address | pattern-sensitive cells |
| `hold` | the `hash` pattern re-read after 2 s | cells that leak |

### Registers

| Row | Check |
|---|---|
| `clkreg` | 0x01–0x07 written with 0x55/0xAA patterns and read back while the counter is stopped (control bit 7). |
| `wrap` | A 2-byte read starting at 0xFF must return 0xFF then 0x00 (control register): the address auto-increment wraps. |
| `date` | Control = 0x00 (clock mode, running), the way the Minerva ROM uses it. Writes 30 August, year+2, Sunday, 12:34:56 in the datasheet register layouts, waits 300 ms, reads everything back and checks every field. The decoded read-back is printed. |

### Driven tests (`HAS_OSC_DRIVE`)

The PCF8583 has a "50 Hz" mode (control bits 5:4 = 01) that clocks the counter
chain from an external signal on OSCI instead of the crystal. The Nano drives
OSCI at 5 kHz with `tone()`, so the clock runs at 100× real time: one chip
second is 10 ms. This exercises every part of the RTC logic except the crystal
oscillator itself.

| Row | Check |
|---|---|
| `roll` | Seven rollover cases. Each sets the clock at hh:mm:58 plus a date, waits 2.5 chip-seconds, and checks minutes, hours, date, month, weekday and year: |

| Case | Set | Expect |
|---|---|---|
| 1 | 12:34:58 | 12:35 |
| 2 | 12:59:58 | 13:00 |
| 3 | 23:59:58 30 Apr | 00:00 01 May, weekday +1 |
| 4 | 23:59:58 31 May | 01 Jun |
| 5 | 23:59:58 28 Feb, year+1 | 01 Mar (non-leap) |
| 6 | 23:59:58 28 Feb, year+0 | 29 Feb (the chip's year 0 is the leap year) |
| 7 | 23:59:58 31 Dec, year+3 | 01 Jan, year+0 (year counter wraps) |

| Row | Check |
|---|---|
| `alarm` | Clock 12:34:58, daily alarm 12:35:00, alarm enabled. The alarm flag must be clear right after arming and set 3.5 chip-seconds later. |
| `timer` | Timer function = seconds, timer register = 00. Must read 03 or 04 after 3.5 chip-seconds. |

### Oscillator (`HAS_CRYSTAL`)

| Row | Check |
|---|---|
| `TIME` | Only with a crystal fitted: set 12:34:56 in normal 32.768 kHz mode, wait 2.5 s, seconds must have advanced. Shown as `[-] not tested` on this board. |

### Cleanup

| Row | Check |
|---|---|
| `clean` | 0x01–0xFF zeroed and control register set to 0x00, so a passing chip goes into a Minerva blank. Counts as a test because it is a final write pass. |

## Reading failures

Each failing row prints the first mismatch:

```
  [✗] addr    address in cell, aliasing  -> FAIL @0x41 expected 0x41 got 0x40 bad bits 0x01, 12 bytes
```

* `@0x41` — register address of the first bad byte.
* `bad bits` — XOR of expected and actual: which bits are wrong.
* `12 bytes` — total bad bytes in that pass.

For `roll` the address is replaced by the case number and register:

```
  [✗] roll    rollovers: min/hour/month/leap/year  -> FAIL case 6 reg 0x05 expected 0x29 got 0x01
```

(case 6 = leap-year February, register 0x05 = year/date; here the chip treated
year 0 as non-leap.)

Rules of thumb:

* Same bit wrong across many addresses → stuck data line or bad output driver.
* `addr` / `~addr` fail while the uniform patterns pass → address decoding fault.
* Only `hold` fails → leaky cells; the chip may work but will not keep data on
  battery.
* `BUS ERROR` mid-run on a chip that was detected → poor socket contact, or the
  A0 bodge is missing and the address flickered. Re-seat and retry before
  blaming the chip.
* `i2c` reports 0x51 → chip works but A0 is high: the bodge is missing.

## Configuration

At the top of the sketch:

| Define | Default | Meaning |
|---|---|---|
| `HAS_CRYSTAL` | 0 | 1 on a board with a 32.768 kHz crystal: enables `TIME` and lets `date` tolerate ticking seconds |
| `HAS_OSC_DRIVE` | 1 | 1 with the D9→OSCI wire: enables `roll`, `alarm`, `timer` |
| `OSC_PIN` | 9 | Nano pin driving OSCI |
| `OSC_HZ` | 5000 | drive frequency; speed-up factor is `OSC_HZ / 50` |
| `HOLD_MS` | 2000 | wait for the `hold` pass |
| `CHUNK` | 16 | bytes per I2C transaction (Wire buffer is 32) |

Serial is 115200 baud, I2C 100 kHz (the PCF8583 maximum).

## Limits

A PASS here means: good RAM, good I2C interface, good counter/calendar/alarm
logic. It does **not** prove:

* the on-chip oscillator amplifier works with a real crystal (`TIME`, needs a
  crystal on the board);
* the chip keeps time and RAM on battery (needs a battery and a retention test).

Both are planned for a board revision with a 32.768 kHz crystal, trimmer, A0
tied to ground and a backup battery with diode.

## History

The first version of this sketch tested a single RAM byte at a hard-coded
address 0x50 without checking I2C results. With A0 floating on the board, the
chip's address drifted between 0x50 and 0x51 and good chips were reported as
bad. If you have chips rejected by that version, re-test them.
