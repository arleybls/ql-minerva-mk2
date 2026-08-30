// PCF8583 go/no-go tester
//
// Copyright (c) 2026 Arley Silveira
// All rights reserved.
//
// Continuous mode: insert a chip in the socket, the test runs by itself,
// shows a progress bar, then a checklist and a verdict; remove the chip and
// insert the next one.
//
// RAM passes over 0x08..0xFF (alarm registers + 240 bytes free RAM):
//   0x00 0xFF 0x55 0xAA  uniform patterns, stuck bits
//   walk1 walk0          one bit set / cleared per byte, rotating: bit coupling
//   addr ~addr           cell = its address / inverse: address-line aliasing
//   hash                 pseudo-random pattern: pattern-sensitive cells
//   hold                 hash pattern, wait HOLD_MS, re-read: leaky cells
// Extra checks:
//   clkreg               clock registers 0x01..0x07 as RAM, counter stopped
//   wrap                 address counter wraps 0xFF -> 0x00
//   date                 set 30/08 y+2 12:34:56 with the counter running, read back
// With HAS_OSC_DRIVE = 1 (Nano pin OSC_PIN wired to OSCI, chip in 50 Hz mode,
// driven at OSC_HZ so the clock runs OSC_HZ/50 times faster than real time).
// The drive is only on during these tests, never while a chip is inserted:
//   stop                 control bit 7 halts the counter
//   roll                 7 rollover cases: minute, hour, 30/31-day month,
//                        February non-leap and leap, year-end + year counter
//   alarm                daily alarm flag sets at the alarm time, not at another time
//   timer                timer register counts seconds, wraps 99 -> 00, sets its flag
//   TIME                 only with HAS_CRYSTAL = 1: set 12:34:56, check seconds advance
//   clean                counter stopped, 0x01..0xFF zeroed and read back, control 0x00
//
// Board notes: A0 must be tied to VSS (bodge pin 3 -> pin 4 on the socket).
// The chip answers at 0x50 with A0 low, 0x51 with A0 high; both are scanned.
// OSCI drive: wire Nano D9 (OSC_PIN) to socket pin 1 (OSCI).

#include <Wire.h>

// The AVR core has had Wire.setWireTimeout() since 1.8.3 but only newer cores
// announce it with WIRE_HAS_TIMEOUT.
#if defined(WIRE_HAS_TIMEOUT) || defined(ARDUINO_ARCH_AVR)
#define HAVE_WIRE_TIMEOUT 1
#endif

// ---------------------------------------------------------------- configuration

#define HAS_CRYSTAL     0        // 1 on a board with a 32.768 kHz crystal
#define HAS_OSC_DRIVE   1        // 1 with a wire from OSC_PIN to OSCI (pin 1)
#define OSC_PIN         9
#define OSC_HZ          5000UL   // 50 Hz mode: 50 pulses = 1 chip second -> 100x real time
#define CHIP_MS(tenths) ((tenths) * 5000UL / OSC_HZ)   // real ms for N tenths of a chip second

#define CHUNK           31       // data bytes per I2C transaction (Wire buffer 32 = reg + 31)
#define HOLD_MS         2000     // wait for the hold pass
#define DATE_SETTLE_MS  300      // wait between setting and reading back the date
#define POLL_MS         500      // idle poll interval
#define SETTLE_MS       200      // socket contacts settle after insertion
#define HEARTBEAT_POLLS 10       // one '.' every 10 polls = 5 s while idle
#define WIRE_TIMEOUT_US 25000UL  // a stuck bus returns an error instead of hanging

// ---------------------------------------------------------------- PCF8583

#define ADDR_A0_LOW   0x50
#define ADDR_A0_HIGH  0x51

#define REG_CONTROL   0x00
#define REG_HUNDREDTH 0x01
#define REG_SECONDS   0x02
#define REG_MINUTES   0x03
#define REG_HOURS     0x04
#define REG_YEARDATE  0x05
#define REG_WDMONTH   0x06
#define REG_TIMER     0x07
#define REG_ALARM_CTL 0x08
#define RAM_START     REG_ALARM_CTL
#define RAM_END       0xFF

#define CTL_STOP      0x80     // bit 7: stop counting and reset the divider
#define CTL_50HZ      0x10     // bits 5-4 = 01: clock mode clocked from OSCI at 50 Hz
#define CTL_ALARM_EN  0x04     // bit 2: enable the alarm function (alarm control + timer)
#define CTL_ALARM_FLG 0x02     // bit 1: alarm flag (when alarm enabled)
#define CTL_TIMER_FLG 0x01     // bit 0: timer flag (when alarm enabled)
#define ALM_DAILY     0x10     // alarm control bits 5-4 = 01: daily alarm
#define ALM_TMR_SEC   0x02     // alarm control bits 2-0 = 010: timer counts seconds

// Register bit layouts
#define YD(y, d)  (((y) << 6) | (d))     // 0x05: year 0-3 (0 = leap), BCD date
#define WM(wd, m) (((wd) << 5) | (m))    // 0x06: weekday 0-6, BCD month
// 0x04: bit 7 = 12 h format (0 = 24 h), bit 6 = AM/PM, bits 5-0 BCD hours

// ---------------------------------------------------------------- tests

enum Test : byte {
  T_I2C,
  T_00, T_FF, T_55, T_AA, T_WALK1, T_WALK0, T_ADDR, T_NADDR, T_HASH, T_HOLD,
  T_CLKREG, T_WRAP, T_DATE,
  T_STOP, T_ROLL, T_ALARM, T_TIMER,     // OSCI-driven
  T_TIME, T_CLEAN, N_TESTS
};
#define IS_RAM_PASS(t) ((t) >= T_00 && (t) <= T_HOLD)
static_assert(T_HOLD == T_HASH + 1, "hold re-reads the hash pattern left by the previous pass");

enum Status : byte { S_PENDING, S_OK, S_FAIL, S_BUS, S_NT, S_SKIP };

struct TestResult {
  Status status;
  byte addr, expect, got;   // first failure
  byte count;               // number of failing bytes
};

static TestResult results[N_TESTS];
static byte dev;            // I2C address of the chip under test
static byte curTest;        // test being run, for bus-error attribution
static bool bothAddr;       // chip answered at 0x50 and 0x51
static bool busDead;        // once a bus error happens, remaining tests are skipped
static byte dateBack[6];    // date test read-back for the checklist row

static const __FlashStringHelper *testName(byte t) {
  switch (t) {
    case T_I2C:    return F("i2c   ");
    case T_00:     return F("0x00  ");
    case T_FF:     return F("0xFF  ");
    case T_55:     return F("0x55  ");
    case T_AA:     return F("0xAA  ");
    case T_WALK1:  return F("walk1 ");
    case T_WALK0:  return F("walk0 ");
    case T_ADDR:   return F("addr  ");
    case T_NADDR:  return F("~addr ");
    case T_HASH:   return F("hash  ");
    case T_HOLD:   return F("hold  ");
    case T_CLKREG: return F("clkreg");
    case T_WRAP:   return F("wrap  ");
    case T_DATE:   return F("date  ");
    case T_STOP:   return F("stop  ");
    case T_ROLL:   return F("roll  ");
    case T_ALARM:  return F("alarm ");
    case T_TIMER:  return F("timer ");
    case T_TIME:   return F("TIME  ");
    case T_CLEAN:  return F("clean ");
    default:       return F("?     ");
  }
}

static const __FlashStringHelper *testDesc(byte t) {
  switch (t) {
    case T_I2C:    return F("bus detection");
    case T_00: case T_FF: case T_55: case T_AA: return F("uniform pattern");
    case T_WALK1:  return F("walking 1, bit coupling");
    case T_WALK0:  return F("walking 0, bit coupling");
    case T_ADDR:   return F("address in cell, aliasing");
    case T_NADDR:  return F("inverse address in cell");
    case T_HASH:   return F("pseudo-random pattern");
    case T_HOLD:   return F("data hold, re-read after wait");
    case T_CLKREG: return F("clock regs 0x01-0x07 as RAM");
    case T_WRAP:   return F("address wrap 0xFF -> 0x00");
    case T_DATE:   return F("set date/time, counter running");
    case T_STOP:   return F("stop bit halts the counter");
    case T_ROLL:   return F("rollovers: min/hour/month/leap/year");
    case T_ALARM:  return F("daily alarm flag, positive + negative");
    case T_TIMER:  return F("timer counts seconds, wraps, flags");
    case T_TIME:   return F("seconds advance");
    case T_CLEAN:  return F("chip zeroed and verified");
    default:       return F("");
  }
}

static byte patternByte(byte t, byte a) {
  switch (t) {
    case T_00:    return 0x00;
    case T_FF:    return 0xFF;
    case T_55:    return 0x55;
    case T_AA:    return 0xAA;
    case T_WALK1: return 1 << (a & 7);
    case T_WALK0: return ~(1 << (a & 7));
    case T_ADDR:  return a;
    case T_NADDR: return ~a;
    case T_HASH:
    case T_HOLD:  return (byte)((a * 0x9D + 0x3B) ^ (a << 3) ^ (a >> 5));
    default:      return 0;
  }
}

// ---------------------------------------------------------------- I2C

static void wireSetup() {
  Wire.begin();
  Wire.setClock(100000);   // PCF8583 is 100 kHz max
#ifdef HAVE_WIRE_TIMEOUT
  Wire.setWireTimeout(WIRE_TIMEOUT_US, true);
  Wire.clearWireTimeoutFlag();
#endif
}

// Free a slave that is holding SDA low: clock SCL up to 9 times, then a STOP.
static void busClear() {
  Wire.end();
  pinMode(SDA, INPUT_PULLUP);
  pinMode(SCL, INPUT_PULLUP);
  for (byte i = 0; i < 9 && digitalRead(SDA) == LOW; i++) {
    pinMode(SCL, OUTPUT); digitalWrite(SCL, LOW); delayMicroseconds(5);
    pinMode(SCL, INPUT_PULLUP);                    delayMicroseconds(5);
  }
  pinMode(SDA, OUTPUT); digitalWrite(SDA, LOW); delayMicroseconds(5);
  pinMode(SDA, INPUT_PULLUP);                    delayMicroseconds(5);
  wireSetup();
}

static bool busTimedOut() {
#ifdef HAVE_WIRE_TIMEOUT
  return Wire.getWireTimeoutFlag();
#else
  return false;
#endif
}

static bool chipPresent(byte a) {
  Wire.beginTransmission(a);
  return Wire.endTransmission() == 0;
}

static void setBus(byte reg) {
  results[curTest].status = S_BUS;
  results[curTest].addr = reg;
  busDead = true;
}

// Write n bytes at reg; on failure records a bus error for the current test.
static bool wr(byte reg, const byte *data, byte n) {
  Wire.beginTransmission(dev);
  Wire.write(reg);
  if (n) Wire.write(data, n);
  if (Wire.endTransmission() == 0) return true;
  setBus(reg);
  return false;
}

static bool wr1(byte reg, byte v) { return wr(reg, &v, 1); }

// Read n bytes from reg (repeated start); on failure records a bus error.
static bool rd(byte reg, byte *data, byte n) {
  Wire.beginTransmission(dev);
  Wire.write(reg);
  if (Wire.endTransmission(false) == 0 && Wire.requestFrom(dev, n) == n) {
    for (byte i = 0; i < n; i++) data[i] = Wire.read();
    return true;
  }
  setBus(reg);
  return false;
}

static void setFail(byte reg, byte expect, byte got) {
  TestResult &r = results[curTest];
  if (r.status != S_FAIL) { r.status = S_FAIL; r.addr = reg; r.expect = expect; r.got = got; }
  if (r.count < 255) r.count++;
}

static void printHex2(byte v) {
  if (v < 0x10) Serial.print('0');
  Serial.print(v, HEX);
}

// ---------------------------------------------------------------- OSCI drive

// Only driven during the driven tests: a chip must never be inserted with
// 5 V on OSCI, and a crystal (HAS_CRYSTAL) must be left alone otherwise.
static void oscDrive(bool on) {
#if HAS_OSC_DRIVE
  if (on) {
    tone(OSC_PIN, OSC_HZ);
  } else {
    noTone(OSC_PIN);
    pinMode(OSC_PIN, INPUT);
  }
#else
  (void)on;
#endif
}

// ---------------------------------------------------------------- RAM passes

static bool ramFill(byte t) {
  byte buf[CHUNK];
  for (int addr = RAM_START; addr <= RAM_END; addr += CHUNK) {
    byte n = min((int)CHUNK, RAM_END - addr + 1);
    for (byte i = 0; i < n; i++) buf[i] = patternByte(t, addr + i);
    if (!wr(addr, buf, n)) return false;
  }
  return true;
}

static void ramVerify(byte t) {
  byte buf[CHUNK];
  for (int addr = RAM_START; addr <= RAM_END; addr += CHUNK) {
    byte n = min((int)CHUNK, RAM_END - addr + 1);
    if (!rd(addr, buf, n)) return;
    for (byte i = 0; i < n; i++) {
      byte a = addr + i;
      byte expect = patternByte(t, a);
      if (buf[i] != expect) setFail(a, expect, buf[i]);
    }
  }
}

static void ramPass(byte t) {
  if (t == T_HOLD) {
    delay(HOLD_MS);              // hash pattern is still in RAM from the previous pass
  } else if (!ramFill(t)) {
    return;
  }
  ramVerify(t);
}

// ---------------------------------------------------------------- register tests

// Clock registers 0x01..0x07 are plain RAM cells when the counter is stopped.
static void clkregTest() {
  if (!wr1(REG_CONTROL, CTL_STOP)) return;
  const byte pats[2] = {0x55, 0xAA};
  for (byte p = 0; p < 2; p++) {
    byte w[7], back[7];
    for (byte i = 0; i < 7; i++) w[i] = pats[p] ^ (i & 1 ? 0xFF : 0x00);
    if (!wr(REG_HUNDREDTH, w, 7) || !rd(REG_HUNDREDTH, back, 7)) return;
    for (byte i = 0; i < 7; i++)
      if (back[i] != w[i]) setFail(REG_HUNDREDTH + i, w[i], back[i]);
  }
}

// Reading 2 bytes from 0xFF must wrap to 0x00 (control register).
static void wrapTest() {
  const byte ctl = CTL_STOP | CTL_50HZ;   // distinctive, stopped, no alarm function
  const byte last = 0x3C;
  byte back[2];
  if (!wr1(REG_CONTROL, ctl) || !wr1(RAM_END, last) || !rd(RAM_END, back, 2)) return;
  if (back[0] != last)                          setFail(RAM_END, last, back[0]);
  else if ((back[1] & 0xFC) != (ctl & 0xFC))    setFail(REG_CONTROL, ctl, back[1]);  // bits 1:0 are live flags
}

// Set a real date/time with the counter running (control = 0x00), the way the
// Minerva ROM does, then read it back and check every field.
#define DATE_SEC   0x56
static const byte dateSet[6] = {0x00, DATE_SEC, 0x34, 0x12, YD(2, 0x30), WM(6, 0x08)};  // 12:34:56 Sun 30 Aug y+2

static void dateTest() {
  if (!wr1(REG_CONTROL, 0x00) || !wr(REG_HUNDREDTH, dateSet, 6)) return;
  delay(DATE_SETTLE_MS);
  if (!rd(REG_HUNDREDTH, dateBack, 6)) return;
  // hundredths ignored; seconds may tick if a crystal is fitted
  byte sec = ((dateBack[1] >> 4) * 10) + (dateBack[1] & 0x0F);
  byte secMax = HAS_CRYSTAL ? 59 : 56;
  if (sec < 56 || sec > secMax) setFail(REG_SECONDS, DATE_SEC, dateBack[1]);
  for (byte i = 2; i < 6; i++)
    if (dateBack[i] != dateSet[i]) setFail(REG_HUNDREDTH + i, dateSet[i], dateBack[i]);
}

static void printDateBack() {
  // dd/mm y+N wdW hh:mm:ss from the raw registers
  printHex2(dateBack[4] & 0x3F); Serial.print('/');
  printHex2(dateBack[5] & 0x1F);
  Serial.print(F(" y+")); Serial.print(dateBack[4] >> 6);
  Serial.print(F(" wd")); Serial.print(dateBack[5] >> 5);
  Serial.print(' ');
  printHex2(dateBack[3] & 0x3F); Serial.print(':');
  printHex2(dateBack[2]);        Serial.print(':');
  printHex2(dateBack[1]);
}

// ---------------------------------------------------------------- OSCI-driven tests

// The Nano drives OSCI at OSC_HZ with the chip in 50 Hz mode, so the counter
// chain runs OSC_HZ/50 times faster than real time. Nothing may print between
// a clock write and its read-back: a blocking Serial.print would eat the margin.

#if HAS_OSC_DRIVE

// Stopped and driven for 3.5 chip seconds, the clock must not move.
static void stopTest() {
  const byte set[2] = {0x00, 0x58};
  byte back[2];
  if (!wr1(REG_CONTROL, CTL_STOP | CTL_50HZ) || !wr(REG_HUNDREDTH, set, 2)) return;
  delay(CHIP_MS(35));
  if (!rd(REG_HUNDREDTH, back, 2)) return;
  if (back[1] != 0x58) setFail(REG_SECONDS, 0x58, back[1]);
  if (back[0] != 0x00) setFail(REG_HUNDREDTH, 0x00, back[0]);
}

// Each case is loaded with the counter stopped and released, which resets the
// divider, so after 2.5 chip seconds the seconds read exactly 00.
struct RollCase { byte sec, min, hour, yd, wm, eMin, eHour, eYd, eWm; };
static const RollCase rollCases[] PROGMEM = {
  {0x58, 0x34, 0x12, YD(2, 0x30), WM(0, 0x08), 0x35, 0x12, YD(2, 0x30), WM(0, 0x08)}, // 1 minute
  {0x58, 0x59, 0x12, YD(2, 0x30), WM(0, 0x08), 0x00, 0x13, YD(2, 0x30), WM(0, 0x08)}, // 2 hour
  {0x58, 0x59, 0x23, YD(2, 0x30), WM(0, 0x04), 0x00, 0x00, YD(2, 0x01), WM(1, 0x05)}, // 3 30-day month
  {0x58, 0x59, 0x23, YD(2, 0x31), WM(0, 0x05), 0x00, 0x00, YD(2, 0x01), WM(1, 0x06)}, // 4 31-day month
  {0x58, 0x59, 0x23, YD(1, 0x28), WM(0, 0x02), 0x00, 0x00, YD(1, 0x01), WM(1, 0x03)}, // 5 Feb non-leap
  {0x58, 0x59, 0x23, YD(0, 0x28), WM(0, 0x02), 0x00, 0x00, YD(0, 0x29), WM(1, 0x02)}, // 6 Feb leap
  {0x58, 0x59, 0x23, YD(3, 0x31), WM(0, 0x12), 0x00, 0x00, YD(0, 0x01), WM(1, 0x01)}, // 7 year end
};
#define N_ROLL (sizeof(rollCases) / sizeof(rollCases[0]))

// Failure address for roll: (case number << 4) | register
static void rollFail(byte c, byte reg, byte expect, byte got) {
  setFail(((c + 1) << 4) | reg, expect, got);
}

static void rollTest() {
  for (byte c = 0; c < N_ROLL; c++) {
    RollCase rc;
    memcpy_P(&rc, &rollCases[c], sizeof(rc));
    byte set[6] = {0x00, rc.sec, rc.min, rc.hour, rc.yd, rc.wm};
    if (!wr1(REG_CONTROL, CTL_STOP | CTL_50HZ) || !wr(REG_HUNDREDTH, set, 6)) return;
    if (!wr1(REG_CONTROL, CTL_50HZ)) return;   // release: divider starts from zero
    delay(CHIP_MS(25));
    byte back[5];   // sec, min, hour, yd, wm
    if (!rd(REG_SECONDS, back, 5)) return;
    if (back[0] != 0x00)     rollFail(c, REG_SECONDS,  0x00,     back[0]);
    if (back[1] != rc.eMin)  rollFail(c, REG_MINUTES,  rc.eMin,  back[1]);
    if (back[2] != rc.eHour) rollFail(c, REG_HOURS,    rc.eHour, back[2]);   // full byte: format bits too
    if (back[3] != rc.eYd)   rollFail(c, REG_YEARDATE, rc.eYd,   back[3]);
    if (back[4] != rc.eWm)   rollFail(c, REG_WDMONTH,  rc.eWm,   back[4]);
  }
}

// Arm a daily alarm at almMin:almHour with the clock at 12:34:58, wait 3.5
// chip seconds, return the control register (flag bits included).
static bool alarmRun(byte almHour, byte almMin, byte *ctlBack) {
  const byte clk[6] = {0x00, 0x58, 0x34, 0x12, YD(2, 0x30), WM(0, 0x08)};
  const byte alm[5] = {ALM_DAILY, 0x00, 0x00, almMin, almHour};   // alarm ctl, hund, sec, min, hour
  if (!wr1(REG_CONTROL, CTL_STOP | CTL_50HZ)) return false;
  if (!wr(REG_HUNDREDTH, clk, 6) || !wr(REG_ALARM_CTL, alm, 5)) return false;
  if (!wr1(REG_CONTROL, CTL_50HZ | CTL_ALARM_EN)) return false;   // release, flags cleared
  if (!rd(REG_CONTROL, ctlBack, 1)) return false;
  if (*ctlBack & CTL_ALARM_FLG) return true;                       // set too early: caller fails it
  delay(CHIP_MS(35));
  return rd(REG_CONTROL, ctlBack, 1);
}

static void alarmTest() {
  byte ctl;
  // positive: alarm at 12:35:00, two chip seconds away, must fire
  if (!alarmRun(0x12, 0x35, &ctl)) return;
  if (!(ctl & CTL_ALARM_FLG)) { setFail(REG_CONTROL, CTL_50HZ | CTL_ALARM_EN | CTL_ALARM_FLG, ctl); return; }
  // negative: alarm at 13:35:00, must not fire at 12:35:00
  if (!alarmRun(0x13, 0x35, &ctl)) return;
  if (ctl & CTL_ALARM_FLG) setFail(REG_CONTROL, CTL_50HZ | CTL_ALARM_EN, ctl);
}

// Timer preset to 98 counting seconds: after 3.5 chip seconds it has wrapped
// 99 -> 00 and reads 01 or 02, and the timer flag must be set.
static void timerTest() {
  byte back[1];
  if (!wr1(REG_CONTROL, CTL_STOP | CTL_50HZ)) return;
  if (!wr1(REG_ALARM_CTL, ALM_TMR_SEC) || !wr1(REG_TIMER, 0x98)) return;
  if (!wr1(REG_CONTROL, CTL_50HZ | CTL_ALARM_EN)) return;   // release, flags cleared
  delay(CHIP_MS(35));
  if (!rd(REG_TIMER, back, 1)) return;
  if (back[0] != 0x01 && back[0] != 0x02) setFail(REG_TIMER, 0x01, back[0]);
  if (!rd(REG_CONTROL, back, 1)) return;
  if (!(back[0] & CTL_TIMER_FLG)) setFail(REG_CONTROL, CTL_50HZ | CTL_ALARM_EN | CTL_TIMER_FLG, back[0]);
}

#endif  // HAS_OSC_DRIVE

// ---------------------------------------------------------------- TIME test

static void timeTest() {
#if HAS_CRYSTAL
  const byte t[4] = {0x00, 0x56, 0x34, 0x12};  // hundredths, sec, min, hours (BCD 12:34:56)
  byte back;
  if (!wr1(REG_CONTROL, CTL_STOP) || !wr(REG_HUNDREDTH, t, 4) || !wr1(REG_CONTROL, 0x00)) return;
  delay(2500);
  if (!rd(REG_SECONDS, &back, 1)) return;
  byte sec = ((back >> 4) * 10) + (back & 0x0F);
  if (sec < 58 || sec > 59) setFail(REG_SECONDS, 0x58, back);
#else
  results[T_TIME].status = S_NT;
#endif
}

// ---------------------------------------------------------------- cleanup

// Stop the counter, zero 0x01..0xFF, read it back, then control = 0x00.
static void cleanTest() {
  byte buf[CHUNK];
  if (!wr1(REG_CONTROL, CTL_STOP)) return;
  memset(buf, 0, sizeof buf);
  for (int addr = REG_HUNDREDTH; addr <= RAM_END; addr += CHUNK) {
    byte n = min((int)CHUNK, RAM_END - addr + 1);
    if (!wr(addr, buf, n)) return;
  }
  for (int addr = REG_HUNDREDTH; addr <= RAM_END; addr += CHUNK) {
    byte n = min((int)CHUNK, RAM_END - addr + 1);
    if (!rd(addr, buf, n)) return;
    for (byte i = 0; i < n; i++)
      if (buf[i] != 0x00) setFail(addr + i, 0x00, buf[i]);
  }
  wr1(REG_CONTROL, 0x00);
}

// ---------------------------------------------------------------- report

static void printChecklist() {
  for (byte t = 0; t < N_TESTS; t++) {
    const TestResult &r = results[t];
    Serial.print(F("  "));
    switch (r.status) {
      case S_OK:   Serial.print(F("[\xE2\x9C\x93] ")); break;   // ✓
      case S_FAIL:
      case S_BUS:  Serial.print(F("[\xE2\x9C\x97] ")); break;   // ✗
      case S_NT:   Serial.print(F("[-] ")); break;
      default:     Serial.print(F("[ ] ")); break;
    }
    Serial.print(testName(t)); Serial.print(F("  ")); Serial.print(testDesc(t));

    if (t == T_I2C && r.status != S_BUS) {
      Serial.print(F("  -> ACK at 0x")); printHex2(dev);
      if (bothAddr)                  Serial.print(F(" AND 0x51: A0 floating / A0 input bad"));
      else if (dev == ADDR_A0_LOW)   Serial.print(F(" (A0 low)"));
      else                           Serial.print(F(" (A0 high - bodge missing?)"));
      Serial.println();
      continue;
    }
    if (t == T_DATE && (r.status == S_OK || r.status == S_FAIL)) {
      Serial.print(F("  -> read back ")); printDateBack();
      if (r.status == S_OK) { Serial.println(); continue; }
      Serial.print(F(" |"));
    }
    switch (r.status) {
      case S_FAIL:
        if (t == T_ROLL) {
          Serial.print(F("  -> FAIL case ")); Serial.print(r.addr >> 4);
          Serial.print(F(" reg 0x")); printHex2(r.addr & 0x0F);
        } else {
          Serial.print(F("  -> FAIL @0x")); printHex2(r.addr);
        }
        Serial.print(F(" expected 0x")); printHex2(r.expect);
        Serial.print(F(" got 0x")); printHex2(r.got);
        Serial.print(F(" bad bits 0x")); printHex2(r.got ^ r.expect);
        if (r.count > 1) { Serial.print(F(", ")); Serial.print(r.count); Serial.print(F(" mismatches")); }
        break;
      case S_BUS:
        Serial.print(F("  -> BUS ERROR @0x")); printHex2(r.addr);
        break;
      case S_NT:
        Serial.print(t == T_TIME ? F("  -> not tested (no crystal on this board)")
                                 : F("  -> not tested (no OSCI drive wire)"));
        break;
      case S_SKIP:
        Serial.print(F("  -> skipped after bus error"));
        break;
      default:
        break;
    }
    Serial.println();
  }
}

static void printPassedBanner() {
  Serial.println();
  Serial.println(F("                                                        /$$ /$$"));
  Serial.println(F("                                                       | $$| $$"));
  Serial.println(F("  /$$$$$$   /$$$$$$   /$$$$$$$ /$$$$$$$  /$$$$$$   /$$$$$$$| $$"));
  Serial.println(F(" /$$__  $$ |____  $$ /$$_____//$$_____/ /$$__  $$ /$$__  $$| $$"));
  Serial.println(F("| $$  \\ $$  /$$$$$$$|  $$$$$$|  $$$$$$ | $$$$$$$$| $$  | $$|__/"));
  Serial.println(F("| $$  | $$ /$$__  $$ \\____  $$\\____  $$| $$_____/| $$  | $$"));
  Serial.println(F("| $$$$$$$/|  $$$$$$$ /$$$$$$$//$$$$$$$/|  $$$$$$$|  $$$$$$$ /$$"));
  Serial.println(F("| $$____/  \\_______/|_______/|_______/  \\_______/ \\_______/|__/"));
  Serial.println(F("| $$"));
  Serial.println(F("| $$"));
  Serial.println(F("|__/"));
  Serial.println();
}

// ---------------------------------------------------------------- one chip

static void runTest(byte t) {
  curTest = t;
  if (IS_RAM_PASS(t)) { ramPass(t); return; }
  switch (t) {
    case T_I2C:
      // the chip already ACKed to get here; both addresses answering is a fault.
      // Then put the chip in a known state before the RAM passes: it has no
      // reset and powers up with a random control register.
      if (bothAddr) results[T_I2C].status = S_FAIL;
      wr1(REG_CONTROL, CTL_STOP);
      break;
    case T_CLKREG: clkregTest(); break;
    case T_WRAP:   wrapTest();   break;
    case T_DATE:   dateTest();   break;
#if HAS_OSC_DRIVE
    case T_STOP:   stopTest();   break;
    case T_ROLL:   rollTest();   break;
    case T_ALARM:  alarmTest();  break;
    case T_TIMER:  timerTest();  break;
#else
    case T_STOP: case T_ROLL: case T_ALARM: case T_TIMER:
      results[t].status = S_NT;
      break;
#endif
    case T_TIME:   timeTest();   break;
    case T_CLEAN:  cleanTest();  break;
  }
}

static void runChip() {
  Serial.println();
  Serial.print(F("=== chip detected at 0x")); printHex2(dev); Serial.println(F(" ==="));

  memset(results, 0, sizeof results);   // S_PENDING == 0
  busDead = false;

  Serial.print(F("progress ["));
  for (byte t = 0; t < N_TESTS; t++) {
    if (t == T_STOP) oscDrive(true);
    if (busDead) {
      results[t].status = S_SKIP;
    } else {
      runTest(t);
      if (results[t].status == S_PENDING) results[t].status = S_OK;
    }
    if (t == T_TIMER) oscDrive(false);
    Serial.print(F("##"));
  }
  oscDrive(false);   // in case a bus error skipped past T_TIMER
  Serial.println(F("] done"));
  Serial.println();

  printChecklist();

  byte failed = 0, ok = 0, skipped = 0;
  for (byte t = 0; t < N_TESTS; t++) {
    switch (results[t].status) {
      case S_OK:   ok++;      break;
      case S_SKIP: skipped++; break;
      case S_NT:              break;
      default:     failed++;  break;
    }
  }

  Serial.println();
  Serial.print(F("CHIP @0x")); printHex2(dev); Serial.print(F(": "));
  if (failed == 0) {
    Serial.print(F("PASS (")); Serial.print(ok); Serial.print(F(" tests"));
#if !HAS_CRYSTAL
    Serial.print(F(", oscillator not tested"));
#endif
    Serial.println(F(")"));
    printPassedBanner();
  } else {
    Serial.print(F("FAIL (")); Serial.print(failed); Serial.print(F(" failed"));
    if (skipped) { Serial.print(F(", ")); Serial.print(skipped); Serial.print(F(" skipped")); }
    Serial.print(F(", ")); Serial.print(ok); Serial.println(F(" ok)"));
  }
  Serial.println(F("--- remove chip ---"));
}

// ---------------------------------------------------------------- main

void setup() {
  Serial.begin(115200);
  oscDrive(false);
  busClear();              // also does wireSetup()
  Serial.println();
  Serial.println(F("PCF8583 tester - insert chip"));
#if !HAS_CRYSTAL
  Serial.println(F("(HAS_CRYSTAL=0: oscillator test disabled on this board)"));
#endif
#if HAS_OSC_DRIVE
  Serial.print(F("(OSCI driven from D")); Serial.print(OSC_PIN);
  Serial.print(F(" at ")); Serial.print(OSC_HZ); Serial.print(F(" Hz = "));
  Serial.print(OSC_HZ / 50); Serial.println(F("x real time during the driven tests)"));
#endif
#ifdef HAVE_WIRE_TIMEOUT
  Serial.println(F("(I2C timeout armed: a stuck bus is reported and cleared)"));
#else
  Serial.println(F("(Wire library has no timeout: a stuck bus will hang the tester)"));
#endif
}

void loop() {
  static byte idlePolls = 0;

  if (busTimedOut()) {
    busClear();
    Serial.println(F("! I2C bus timeout - bus cleared"));
  }

  bool low  = chipPresent(ADDR_A0_LOW);
  bool high = chipPresent(ADDR_A0_HIGH);
  if (!low && !high) {
    if (++idlePolls >= HEARTBEAT_POLLS) { idlePolls = 0; Serial.print('.'); }
    delay(POLL_MS);
    return;
  }
  idlePolls = 0;

  // debounce: the chip must still answer after the contacts have settled
  delay(SETTLE_MS);
  low  = chipPresent(ADDR_A0_LOW);
  high = chipPresent(ADDR_A0_HIGH);
  if (!low && !high) return;

  dev = low ? ADDR_A0_LOW : ADDR_A0_HIGH;
  bothAddr = low && high;
  runChip();

  while (chipPresent(ADDR_A0_LOW) || chipPresent(ADDR_A0_HIGH)) delay(POLL_MS);
  delay(POLL_MS);
  Serial.println(F("insert next chip"));
}
