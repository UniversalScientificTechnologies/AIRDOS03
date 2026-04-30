# AIRDOS03 MAVLink Firmware

MAVLink tunnel-based firmware for AIRDOS03B on TFUNIPAYLOAD01 (ATmega1284P, 8 MHz). Functionally equivalent to `fw/AIRDOS03_USTDFF` but transmits data as binary MAVLink TUNNEL packets instead of ASCII text, suitable for integration with [TF-ATMON](https://docs.thunderfly.cz/instruments/TF-ATMON) and PX4/ArduPilot autopilots.

## Build and upload

```
source ~/.venv/bin/activate
pio run -e TFUNIPAYLOAD01_uart -t upload
```

Using avrdude directly:
```
avrdude -v -patmega1284p -carduino -P/dev/ttyUSB0 -b57600 -D -Uflash:w:firmware.hex:i
```

## Decode to text format

```
pip install pymavlink
python tools/mavlink_to_airdos.py --port /dev/ttyUSB0 --baud 115200
python tools/mavlink_to_airdos.py --stdin < capture.bin
```

Output is AIRDOS04-compatible ASCII format (same as `fw/AIRDOS03_USTDFF`).


## MAVLink Packet Protocol

All packets use TUNNEL message (`payload_type = 4`, sensor ID for AIRDOS03B).
The message sub-type is encoded as **byte 0** of the 128-byte payload.

```cpp
SendTunnelData(buf, len, 4, 1, 1);  // same payload_type for all AIRDOS03B messages
```

All multi-byte integers are **little-endian**.

### Message sub-types

| Byte 0 | Name            | Corresponds to    | Payload size |
|--------|-----------------|-------------------|--------------|
| 0x01   | AIRDOS_STARTUP  | `$DOS` + `$ADC`   | 29 B         |
| 0x02   | AIRDOS_TIMESYNC | `$TIME`           | 18 B         |
| 0x03   | AIRDOS_START    | `$START`          | 5 B          |
| 0x04   | AIRDOS_STOP     | `$STOP` header    | 14 B         |
| 0x05   | AIRDOS_HIST_LO  | histogram[0..31]  | 69 B         |
| 0x06   | AIRDOS_HIST_HI  | histogram[32..63] | 67 B         |
| 0x07   | AIRDOS_EVENTS   | `$E` events       | ≤121 B       |
| 0x08   | AIRDOS_ENV      | `$ENV`            | 17 B         |
| 0x09   | AIRDOS_ALIVE    | heartbeat (broadcast) | 20 B     |

---

### 0x01 — STARTUP (29 bytes)

Sent once at startup. Provides device identification (`$DOS`, `$ADC`).

```
Offset  Size  Field
  0      1    sub_type        = 0x01
  1      1    threshold       // THRESHOLD constant (64)
  2      2    max_events      // MAX_EVENTS constant (300)
  4     16    serial_number   // raw bytes from EEPROM 0x5B offset 0x0800
 20      1    adc_conf1       // from EEPROM 0x53 offset 0x0000
 21      1    adc_conf2       // from EEPROM 0x53 offset 0x0001
 22      1    fw_major        // MAJOR version
 23      1    fw_minor        // MINOR version
 24      4    reserved        // zero
```

Python: `struct.unpack_from('<BBH16sBBBB4s', payload)`

---

### 0x02 — TIMESYNC (18 bytes)

Sent on first GNSS fix and on re-sync after fix loss (`$TIME`).

```
Offset  Size  Field
  0      1    sub_type        = 0x02
  1      1    gnss_synced     // 1 = valid GNSS fix, 0 = not synced
  2      4    rtc_seconds     // 1PPS-derived seconds counter
  6      4    gnss_sync_unix  // Unix time from last valid $GNRMC
 10      4    current_unix    // gnss_sync_unix + (rtc_seconds - sync_rtc_seconds)
 14      4    sync_age        // seconds since last GNSS fix
```

Python: `struct.unpack_from('<BB4I', payload)`

---

### 0x03 — START (5 bytes)

Sent at start of each 10 s integration window (`$START`).

```
Offset  Size  Field
  0      1    sub_type        = 0x03
  1      2    count           // measurement cycle index
  3      2    start_systime   // TCNT1 at start of window (128 µs/tick)
```

Python: `struct.unpack_from('<BHH', payload)`

---

### 0x04 — STOP (14 bytes)

Sent after events, before histogram packets (`$STOP` header fields).

```
Offset  Size  Field
  0      1    sub_type        = 0x04
  1      2    count           // same cycle index as START
  3      4    tm              // Unix time at end of window (0 if not synced)
  7      1    tm_s100         // hundredths of second (0–99)
  8      2    stop_systime    // TCNT1 at end of window
 10      2    events_total    // total events_counter (may exceed MAX_EVENTS)
 12      2    events_sent     // min(events_total, MAX_EVENTS)
```

Python: `struct.unpack_from('<B H I B H H H', payload)` (no padding)

---

### 0x05 — HIST_LO (69 bytes) and 0x06 — HIST_HI (67 bytes)

64-channel uint16 histogram split across two packets (lossless). Together they form
the histogram fields of `$STOP`.

```
HIST_LO (0x05):                   HIST_HI (0x06):
Offset  Size  Field               Offset  Size  Field
  0      1    sub_type = 0x05       0      1    sub_type = 0x06
  1      2    count                 1      2    count
  3     64    hist[0..31]           3     64    hist[32..63]
              (32 × uint16)                     (32 × uint16)
```

Python: `struct.unpack_from('<BH32H', payload)`

---

### 0x07 — EVENTS (≤121 bytes, chunked)

Up to 29 events per packet. Sent only if `events_sent > 0`.
Multiple packets with `chunk_index` 0 … `chunk_total − 1`. Each event → one `$E` line.

```
Offset  Size  Field
  0      1    sub_type           = 0x07
  1      2    count              // cycle index
  3      1    chunk_index        // 0-based
  4      1    chunk_total        // total chunks for this cycle
  5      1    events_in_chunk    // events in this packet (1–29)
  6      3    reserved
  9    events_in_chunk × 4:
              uint16  event_time    // TCNT1 (128 µs/tick)
              uint16  event_channel // ADC value ≥ THRESHOLD (64)
```

Max: 9 + 29 × 4 = 125 B. For 300 events: ceil(300/29) = 11 packets.

Python header: `struct.unpack_from('<BHBBBxxx', payload[:9])`
Python events: `struct.unpack_from(f'<{2*n}H', payload[9:9+n*4])` — pairs (time, channel)

---

### 0x08 — ENV (17 bytes)

Sent at startup and every 30 s (`$ENV`).

```
Offset  Size  Field
  0      1    sub_type        = 0x08
  1      2    count           // measurement cycle index
  3      4    tm              // Unix time (0 if not synced)
  7      1    tm_s100         // hundredths of second
  8      1    reserved
  9      4    temperature     // float32, °C (IEEE 754 single, little-endian)
 13      4    humidity        // float32, % RH
```

Python: `struct.unpack_from('<BHIBxff', payload)`

---

### 0x09 — ALIVE (20 bytes, broadcast)

Heartbeat sent once per minute as MAVLink **broadcast** (`target_system = 0`,
`target_component = 0`). Lets any listener confirm the unit is alive and roughly
how much it is seeing, without waiting for the 10 s measurement cycle.

```
Offset  Size  Field
  0      1    sub_type        = 0x09
  1      4    uptime_s        // millis() / 1000
  5      2    count           // current cycle index
  7      4    cum_pulses      // cumulative ADC interrupts since boot
                              //   (events + every histogram increment)
 11      4    events_min      // events ≥ THRESHOLD since last ALIVE
                              //   (counted always, incl. when MAX_EVENTS exceeded)
 15      1    gnss_synced     // 1 = valid GNSS fix at least once
 16      4    sync_age        // seconds since last GNSS fix (0 if never synced)
```

Python: `struct.unpack_from('<BIHIIBxxxI', payload)` — note 3 padding bytes
are not present in the wire format; use explicit slicing instead:

```python
sub, uptime, count, cum_pulses, events_min = struct.unpack_from('<BIHII', payload)
gnss_synced = payload[15]
sync_age = struct.unpack_from('<I', payload, 16)[0]
```

---

## Emission sequence

```
At setup():
  STARTUP  (0x01)
  ENV      (0x08)   ← initial sensor reading

Every 10 s:
  START    (0x03)
  EVENTS   (0x07) × N chunks   (omitted if events_sent == 0)
  STOP     (0x04)
  HIST_LO  (0x05)
  HIST_HI  (0x06)

Every 30 s (in addition to measurement cycle):
  ENV      (0x08)

Every 60 s (broadcast, target_system=0, target_component=0):
  ALIVE    (0x09)

On GNSS fix or re-sync after fix loss:
  TIMESYNC (0x02)
```

---

## Decoder output format

`tools/mavlink_to_airdos.py` reconstructs AIRDOS04-compatible text:

```
$DOS,AIRDOS03B,<major>.<minor>,0,unknown,MAVLink,<sn_hex_32chars>
$ADC,USTSIPIN03C,<sn_hex_32chars>,<conf1hex><conf2hex>
$TIME,<rtc_seconds>,<gnss_sync_unix>,<current_unix>,<sync_age>,<YYYY-MM-DD HH:MM:SS>
$START,<count>,<start_systime>
$E,<event_time>,<event_channel>
$STOP,<count>,<tm>.<tm_s100>,<stop_systime>,<events_total>,<hist_0>,...,<hist_63>
$ENV,<count>,<tm>.<tm_s100>,<tempC>,<humidity>
$ALIVE,<uptime_s>,<count>,<cum_pulses>,<events_min>,<gnss_synced>,<sync_age>
```
