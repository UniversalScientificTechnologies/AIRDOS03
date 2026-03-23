# AIRDOS03B Firmware — Agent Context

This file provides context for AI coding agents (e.g. Claude Code) to continue
work on this project without losing history across sessions.

## Repository
- Path: `fw/AIRDOS03_USTDFF/`
- Active branch: `AIRDOS03B`
- Main source: `fw/AIRDOS03_USTDFF/src/main.cpp`
- Build: PlatformIO inside a venv — `source ~/.venv/bin/activate && pio run -e TFUNIPAYLOAD01_uart`
- `platformio.ini` is a symlink into the submodule at `../../hw/modules/TFUNIPAYLOAD01/fw/`

## Hardware

### MCU
- ATmega1284P with MightyCore, 8 MHz external crystal
- Board definition: `TFUNIPAYLOAD01`

### Pin mapping
| AVR pin | Arduino | Function |
|---------|---------|----------|
| PB0 | D0 | ADC CONV signal — PCINT8 (PCINT1_vect, PCMSK1 bit 0) |
| PC2 | D18 | DRESET — peak-detector reset (active LOW) |
| PD4 | D12 | GNSS 1PPS — PCINT28 (PCINT3_vect, PCMSK3 bit 4) |
| PD7 | D15 | DSET — ADC chip enable |
| PC1 | D17 | SDA (I2C) |
| PC0 | D16 | SCL (I2C) |
| PD0 | D8 | RX0 — Serial (data output to host) |
| PD1 | D9 | TX0 |
| PD2 | D10 | RX1 — Serial1 (GNSS NMEA input, 9600 baud) |
| PD3 | D11 | TX1 |

### I2C peripherals
| Address | Device | Purpose |
|---------|--------|---------|
| 0x5B | EEPROM | Analog board serial number (16 B from offset 0x0800) |
| 0x53 | EEPROM | ADC configuration (2 B from offset 0x0000) |
| 0x45 | SHT31-DIS (ADDR=HIGH) | Temperature + humidity (optional, external I2C) |

### SPI (ADC)
- 500 kHz, MSB first, SPI_MODE0
- `SPI.transfer16(0x0000)` returns 12-bit word; `>> 2` gives 10-bit ADC value
- Raw SPI registers (SPDR) used inside ISR — SPI library not interrupt-safe

### GNSS
- Connected to UART1 (Serial1, 9600 baud)
- NMEA sentences `$GPRMC` / `$GNRMC` provide absolute UTC time
- 1PPS output connected to PD4 for precise per-second synchronisation

## Output data format

Reference specification: `fw/AIRDOS03_USTDFF/OUTPUT_FORMAT.md`
(written for AIRDOS04C; AIRDOS03B implements it with hardware-driven differences)

Reference implementation of another instrument: `fw/AIRDOS03_USTDFF/AIRDOS04.ino`

### Startup header (Serial, once)
```
#Cvak...
$DOS,AIRDOS03B,<ver>,0,<githash>,<buildtype>,<SN_hex_32chars>
$ADC,USTSIPIN03C,<SN_hex_32chars>,<conf1hex><conf2hex>
#Hmmm...
$ENV,<count>,<tm>.<tm_s100>,<tempC>,<humidity>
```
`$TIME` is **not** printed at startup (no RTC/EEPROM sync available).
It is emitted on first GNSS fix and on re-sync after fix loss.

### Measurement cycle (every 3 s)
```
$START,<count>,<startSystime>
\r\n$E,<tcnt1_ticks>,<adcVal>        <- one line per event with adcVal >= THRESHOLD
\r\n$STOP,<count>,<tm>.<tm_s100>,<stopSystime>,<events_count>,<hist_0>,...,<hist_63>
```

### Environmental record (every 30 s, and once at startup)
```
$ENV,<count>,<tm>.<tm_s100>,<tempC>,<humidity>
```

### Time sync record (on GNSS fix / re-sync)
```
$TIME,<rtc_seconds>,<gnss_sync_unix>,<current_unix>,<sync_age>,<YYYY-MM-DD HH:MM:SS>
```

## Key constants (src/main.cpp)
| Constant | Value | Meaning |
|----------|-------|---------|
| `THRESHOLD` | 64 | ADC values < 64 go to histogram; >= 64 recorded as `$E` event |
| `MAX_EVENTS` | 300 | Maximum individual events stored per integration window |
| Integration interval | 10000 ms | Controlled by `lastDataOutMs` + `millis()` |
| Timer1 prescaler | 1024 | 128 us / tick at 8 MHz |
| `MAJOR.MINOR` | 1.3 | First release with new output format |
| GNSS baud rate | 38400 | Serial1 to GNSS receiver |

## Time-keeping architecture

```
rtc_seconds      - 32-bit seconds counter, incremented by ISR(PCINT1_vect) on 1PPS
last_pps_tcnt1   - TCNT1 captured at the last 1PPS rising edge
gnss_sync_unix   - Unix timestamp from the last valid $GPRMC sentence
sync_rtc_seconds - rtc_seconds at the moment of GNSS sync

current Unix time  = gnss_sync_unix + (rtc_seconds - sync_rtc_seconds)
sub-second [1/100] = (TCNT1 - last_pps_tcnt1) * 128 / 10000
```

TCNT1 is **never reset** — it runs freely for both event timestamps and systime.
The 1PPS ISR only captures its value; it does not reset it.

Without GNSS sync: `tm = rtc_seconds` (small value => clearly invalid Unix time;
existing AIRDOS04 parsers recognise this as an unsynced state).

## ISR layout
| Vector | Pin | Trigger | Action |
|--------|-----|---------|--------|
| `PCINT0_vect` | PB0 (CONV) | Rising edge | Read ADC via raw SPI (SPDR), classify into histogram or event buffer |
| loop() polling | PD4 (1PPS) | Rising/falling edge | Rising: increment `rtc_seconds`, capture `last_pps_tcnt1`, LED3 ON (PC7); Falling: LED3 OFF |

**ATmega1284P PCINT port mapping** (differs from most AVRs):
| Port | PCINT range | PCMSK | PCIE | Vector |
|------|-------------|-------|------|--------|
| PA | PCINT0–7   | PCMSK0 | PCIE0 | PCINT0_vect |
| PB | PCINT8–15  | PCMSK1 | PCIE1 | PCINT1_vect |
| PC | PCINT16–23 | PCMSK2 | PCIE2 | PCINT2_vect |
| PD | PCINT24–31 | PCMSK3 | PCIE3 | PCINT3_vect |

Enable registers:
```cpp
PCICR  |= (1 << PCIE1);  PCMSK1 |= (1 << 0);  // PB0 = PCINT8, bit 0 of PCMSK1
PCICR  |= (1 << PCIE3);  PCMSK3 |= (1 << 4);  // PD4 = PCINT28, bit 4 of PCMSK3
```

## Differences from OUTPUT_FORMAT.md (AIRDOS04C)
| Feature | AIRDOS04C | AIRDOS03B |
|---------|-----------|-----------|
| Histogram channels | 4 | 64 (THRESHOLD) — 10-bit ADC hardware |
| `$E` threshold | adcVal >= 4 | adcVal >= 64 |
| `$DIG`, `$BATP`, `$BATT` | present | absent (hardware not populated) |
| `$TIME` timing | printed at startup from EEPROM | printed on GNSS fix / re-sync |
| `$ENV` sensors | 2x SHT31 + MS5611 | 1x SHT31-DIS ADDR=H (0x45), temp + humidity only |

## Known compiler warnings
Line 12 (`FWversion` macro expansion) produces 4x `invalid suffix on literal` —
inherited from the original codebase; build succeeds without errors.

## Last successful build
- RAM:   ~13.4 % (2201 B / 16384 B)
- Flash: ~8.5 %  (11104 B / 130048 B)
