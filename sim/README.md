# AIRDOS03 firmware simulation (simavr)

A virtual **AIRDOS03B** board (TFUNIPAYLOAD01 + USTSIPIN03). The **unmodified firmware** from
[`fw/`](../fw) runs on a simulated ATmega1284P (8 MHz) with models of the digital peripherals
attached. The analog chain is not simulated: the detector model delivers ADC values directly.

## How it works

The simulator is made of three parts:

1. **[simavr](https://github.com/buserror/simavr)** (git submodule `sim/simavr`) is a library that
   emulates the AVR CPU instruction by instruction, together with its on-chip peripherals
   (timers, interrupts, UART, SPI, TWI/I²C, GPIO). It knows nothing about AIRDOS03.
2. **`build/airdos03_sim`** (sources in `src/`) is a C program linked against simavr. It loads the
   firmware and connects models of the external chips on the board to the CPU pins.
3. **`build/fw/*.elf`** is the firmware, compiled by PlatformIO from `fw/` exactly as for the real board.

`make` builds simavr first (`simavr/simavr/obj-*/libsimavr.a`), then `airdos03_sim`.

## Quick start

Prerequisites (Debian/Ubuntu):

```bash
sudo apt install build-essential pkg-config python3-venv libelf-dev
```

Then, from the repository root:

```bash
git checkout simtest
cd sim
./setup.sh      # submodules, .venv (PlatformIO, pymavlink), builds simulator and firmware
make test       # 6 scenarios, ~30 s
```

`setup.sh` initialises only the submodules the simulator needs (`sim/simavr`,
`hw/modules/TFUNIPAYLOAD01`, `fw/AIRDOS03_MAVLink/src/mavlink`). The AIRDOS03 submodules use
SSH URLs; without a GitHub SSH key, run the command printed by `setup.sh` (HTTPS instead of SSH).

Run the firmware for 60 simulated seconds, UART0 output on stdout:

```bash
./build/airdos03_sim -t 60 build/fw/AIRDOS03_USTDFF.elf
```

The simulation runs about **14× faster than real time** (one hour of flight in ~4.5 minutes).
After changing the firmware, `make test` rebuilds it automatically (or run `make firmware`).

## What is simulated

| Board part | Connection | Model / options |
|---|---|---|
| EEPROM, analog board serial number | I²C 0x5B, 16 B at 0x0800 | `--sn HEX32` |
| EEPROM, ADC configuration | I²C 0x53, 2 B at 0x0000 | `--adc-conf HEX4` |
| SHT31-DIS | I²C 0x45 | command 0x2400, CRC-8, NACK while measuring (`--sht-meas-ms`), `--no-sht` |
| USTSIPIN03 peak detector + ADC | CONV = PB0, DRESET = PC2, SPI | Poisson particle hits, 16-bit value MSB first |
| GNSS receiver | 1PPS = PD4, NMEA = UART1 38400 Bd | `$GNRMC` + `$GNGGA` after each PPS, `--fix-at`, `--fix-loss A:B`, `--no-gnss` |
| Data output | UART0 115200 Bd | stdout, `-o` raw file, `--timed`, `--pty` |
| LEDs red / blue / green | PC5 / PC6 / PC7 | in the VCD trace |

The environment (temperature, humidity, particle rate) is either constant (`--temp`, `--rh`,
`--rate`) or read from a time profile `--scenario file.csv` with columns
`t_s,temp_c,rh_pct,rate_cps` (linear interpolation). See `scenarios/balloon_flight.csv`.

Run `./build/airdos03_sim --help` for all options.

### Model assumptions (not facts about the hardware)

- The spectrum is synthetic: a fraction `--noise-frac` of hits below channel 64 (normal
  distribution), the rest a power law with index `--alpha` up to 65535.
- A hit arriving while CONV still holds an unread value is lost (pile-up). A hit arriving while
  DRESET is low is lost too. The real timing of the analog chain is not modelled.
- SHT31 converts temperature as −45 + 175·raw/65535, so the model returns −45 °C below −45 °C
  (the sensor is specified from −40 °C). The balloon profile (−60 °C) therefore saturates at −45.
- PPS pulses are generated only with a GNSS fix (default behaviour of e.g. u-blox M8).

## Outputs for analysis

```bash
mkdir -p build/out
./build/airdos03_sim -q -t 300 --scenario scenarios/balloon_flight.csv \
    -o build/out/raw.txt --timed build/out/timed.txt --truth build/out/truth.csv \
    --vcd build/out/trace.vcd build/fw/AIRDOS03_USTDFF.elf
.venv/bin/python tools/check_ustdff.py build/out/timed.txt build/out/truth.csv
```

| Option | Content |
|---|---|
| `-o` | exact UART0 byte stream |
| `--timed` | text lines with simulated time of the first and last byte |
| `--truth` | what the virtual hardware really did: hits, ADC reads, SHT31 measurements, PPS |
| `--vcd` | CONV, DRESET, PPS, LEDs, UART, SPI, TWI waveforms for GTKWave |

`check_ustdff.py` compares the firmware output with the truth log: serial number, ADC
configuration, `$ENV` values, `$TIME` and `$STOP` time stamps, and that every ADC value read
inside an integration window appears in the histogram or as a `$E` line.

### MAVLink variant

```bash
./build/airdos03_sim -q -t 120 -o build/out/mav.bin --truth build/out/mav.csv build/fw/AIRDOS03_MAVLink.elf
.venv/bin/python tools/decode_mavlink_file.py build/out/mav.bin > build/out/mav.txt
.venv/bin/python tools/check_mavlink.py build/out/mav.txt build/out/mav.csv
```

Decoding reuses `process_message()` from `fw/AIRDOS03_MAVLink/tools/mavlink_to_airdos.py`.

### Virtual serial port (live data into existing tools)

```bash
./build/airdos03_sim -q -t 0 --realtime --pty build/fw/AIRDOS03_MAVLink.elf &
.venv/bin/python ../fw/AIRDOS03_MAVLink/tools/mavlink_to_airdos.py --port /tmp/simavr-uart0
```

`--realtime=4` runs 4× faster than real time, `-t 0` runs until Ctrl+C.

### Debugging the firmware with gdb

```bash
sudo apt install gdb-avr
./build/airdos03_sim -q -t 100 --gdb 1234 build/fw/AIRDOS03_USTDFF.elf &
avr-gdb build/fw/AIRDOS03_USTDFF.elf -ex "target remote :1234" -ex "break StatusOut" -ex "continue"
(gdb) print *(unsigned short*)&count
(gdb) print *(unsigned long*)&rtc_seconds
```

The firmware is built with LTO, which drops debug information in avr-gcc 7.3, so variables have
to be cast to their type. Breakpoints on functions work normally.

## Firmware findings from the tests

1. **Hundredths of a second without zero padding** (USTDFF): `Serial.print(tm_s100)` prints
   3 hundredths as `…020.3`, i.e. as 0.3 s, in `$STOP` and `$ENV`. The error is up to 0.81 s.
   The MAVLink decoder formats it correctly (`{tm_s100:02d}`).
2. **ADC configuration without zero padding** (USTDFF): `Serial.print(ADCconf1, HEX)` prints
   bytes `03 2C` as `32C`, so the two bytes cannot be separated reliably.
3. **Time stops without PPS**: when the GNSS fix is lost (no PPS pulses), `rtc_seconds` stops
   and `$STOP`/`$ENV` keep the time of the last PPS (up to 20 s error in the test). After the fix
   returns, the time is corrected and `$TIME` is printed.
4. **Values read while a cycle is printed are lost**: `serviceADC()` called inside `DataOut()`
   writes into the histogram and event buffer, which are cleared after the output. With a
   profile of up to 200 hits/s this was about 1 % of read values; at 5 hits/s none.
5. `mavlink_to_airdos.py --stdin` fails with current pymavlink (`file:/dev/stdin` is parsed as a
   UDP address). `tools/decode_mavlink_file.py` works around it.

## Layout

```
sim/
├── README.md
├── setup.sh             one-time preparation
├── Makefile             make / make firmware / make test / make clean
├── build_fw.sh          builds ../fw variants out of tree (PlatformIO), nothing written to ../fw
├── requirements.txt     Python packages for .venv
├── simavr/              git submodule, pinned simavr version
├── src/
│   ├── airdos03_sim.c   main program: MCU, options, UART capture, VCD, pty, gdb
│   ├── board.h          shared declarations of the peripheral models
│   ├── i2c_devices.c    EEPROM (16-bit addressing) and SHT31-DIS
│   ├── ustsipin03.c     detector: CONV / DRESET / SPI
│   └── gnss.c           1PPS + NMEA on UART1
├── tools/               output checks and MAVLink file decoder
├── tests/run_tests.sh   default, high rate, fix loss, no sensors, profile, MAVLink
├── scenarios/           environment time profiles
├── build/               (generated) simulator, firmware ELFs, test outputs
└── .venv/               (generated) Python environment
```
