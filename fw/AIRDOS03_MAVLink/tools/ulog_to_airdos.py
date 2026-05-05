#!/usr/bin/env python3 -u
"""
ulog_to_airdos.py — Extract AIRDOS03B log from a PX4 ULog file.

Usage:
    python ulog_to_airdos.py log100.ulg
    python ulog_to_airdos.py log100.ulg > output.txt
"""

import argparse
import struct
import sys
from datetime import datetime, timezone

try:
    from pyulog import ULog
except ImportError:
    sys.exit("pyulog not found — install with: pip install pyulog")

AIRDOS_PAYLOAD_TYPE = 4

SUB_STARTUP  = 0x01
SUB_TIMESYNC = 0x02
SUB_START    = 0x03
SUB_STOP     = 0x04
SUB_HIST_LO  = 0x05
SUB_HIST_HI  = 0x06
SUB_EVENTS   = 0x07
SUB_ENV      = 0x08
SUB_ALIVE    = 0x09


def sn_hex(raw_bytes):
    return raw_bytes.hex()


def unix_to_str(t):
    try:
        return datetime.fromtimestamp(t, tz=timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
    except Exception:
        return "0000-00-00 00:00:00"


class Cycle:
    def __init__(self):
        self.start_systime = None
        self.stop = None
        self.hist = [None] * 64
        self.events = {}
        self.chunk_total = None

    def complete(self):
        if self.start_systime is None:
            return False
        if self.stop is None:
            return False
        if any(self.hist[i] is None for i in range(64)):
            return False
        if self.stop[4] > 0:
            if self.chunk_total is None:
                return False
            if len(self.events) < self.chunk_total:
                return False
        return True


def parse_startup(payload):
    fmt = '<BBH16sBBBB4s'
    if len(payload) < struct.calcsize(fmt):
        return
    _, threshold, max_events, sn_raw, conf1, conf2, major, minor, _ = \
        struct.unpack_from(fmt, payload)
    sn = sn_hex(sn_raw)
    print(f"$DOS,AIRDOS03B,{major}.{minor},0,unknown,MAVLink,{sn}")
    print(f"$ADC,USTSIPIN03C,{sn},{conf1:02x}{conf2:02x}")


def parse_timesync(payload):
    fmt = '<BB4I'
    if len(payload) < struct.calcsize(fmt):
        return
    _, synced, rtc_sec, gnss_sync, cur_unix, age = struct.unpack_from(fmt, payload)
    dt_str = unix_to_str(cur_unix) if synced else "0000-00-00 00:00:00"
    print(f"$TIME,{rtc_sec},{gnss_sync},{cur_unix},{age},{dt_str}")


def parse_env(payload):
    fmt = '<BHIBxff'
    if len(payload) < struct.calcsize(fmt):
        return
    _, cnt, tm, tm_s100, temp, hum = struct.unpack_from(fmt, payload)
    print(f"$ENV,{cnt},{tm}.{tm_s100:02d},{temp:.1f},{hum:.1f}")


def parse_alive(payload):
    if len(payload) < 20:
        return
    _, uptime, cnt, cum_pulses, evt_min = struct.unpack_from('<BIHII', payload)
    gnss_synced = payload[15]
    sync_age = struct.unpack_from('<I', payload, 16)[0]
    print(f"$ALIVE,{uptime},{cnt},{cum_pulses},{evt_min},{gnss_synced},{sync_age}")


def flush_cycle(count, cycle):
    tm, tm_s100, stop_systime, events_total, events_sent = cycle.stop

    print(f"$START,{count},{cycle.start_systime}")

    if events_sent > 0:
        all_events = []
        for chunk_idx in sorted(cycle.events):
            all_events.extend(cycle.events[chunk_idx])
        for et, ec in all_events:
            print(f"$E,{et},{ec}")

    hist_str = ",".join(str(v) for v in cycle.hist)
    print(f"$STOP,{count},{tm}.{tm_s100:02d},{stop_systime},{events_total},{hist_str}")


def process_payload(payload, cycles):
    if len(payload) < 1:
        return

    sub = payload[0]

    if sub == SUB_STARTUP:
        parse_startup(payload)

    elif sub == SUB_TIMESYNC:
        parse_timesync(payload)

    elif sub == SUB_START:
        if len(payload) < 5:
            return
        _, count, start_systime = struct.unpack_from('<BHH', payload)
        if count not in cycles:
            cycles[count] = Cycle()
        cycles[count].start_systime = start_systime

    elif sub == SUB_STOP:
        fmt = '<B H I B H H H'
        if len(payload) < struct.calcsize(fmt):
            return
        _, count, tm, tm_s100, stop_systime, events_total, events_sent = \
            struct.unpack_from(fmt, payload)
        if count not in cycles:
            cycles[count] = Cycle()
        cycles[count].stop = (tm, tm_s100, stop_systime, events_total, events_sent)

    elif sub == SUB_HIST_LO:
        fmt = '<BH32H'
        if len(payload) < struct.calcsize(fmt):
            return
        vals = struct.unpack_from(fmt, payload)
        count = vals[1]
        hist_vals = vals[2:]
        if count not in cycles:
            cycles[count] = Cycle()
        for i, v in enumerate(hist_vals):
            cycles[count].hist[i] = v

    elif sub == SUB_HIST_HI:
        fmt = '<BH32H'
        if len(payload) < struct.calcsize(fmt):
            return
        vals = struct.unpack_from(fmt, payload)
        count = vals[1]
        hist_vals = vals[2:]
        if count not in cycles:
            cycles[count] = Cycle()
        for i, v in enumerate(hist_vals):
            cycles[count].hist[32 + i] = v

    elif sub == SUB_EVENTS:
        hdr_fmt = '<BHBBBxxx'
        if len(payload) < struct.calcsize(hdr_fmt):
            return
        _, count, chunk_index, chunk_total, n_events = \
            struct.unpack_from(hdr_fmt, payload)
        if count not in cycles:
            cycles[count] = Cycle()
        cyc = cycles[count]
        cyc.chunk_total = chunk_total
        pairs = struct.unpack_from(f'<{2*n_events}H', payload, 9)
        event_list = [(pairs[i*2], pairs[i*2+1]) for i in range(n_events)]
        cyc.events[chunk_index] = event_list

    elif sub == SUB_ENV:
        parse_env(payload)

    elif sub == SUB_ALIVE:
        parse_alive(payload)

    done = [cnt for cnt, cyc in cycles.items() if cyc.complete()]
    for cnt in sorted(done):
        flush_cycle(cnt, cycles.pop(cnt))


def main():
    parser = argparse.ArgumentParser(
        description="Extract AIRDOS03B log from a PX4 ULog file")
    parser.add_argument("ulog", help="Path to .ulg file")
    args = parser.parse_args()

    try:
        u = ULog(args.ulog)
    except Exception as e:
        sys.exit(f"Cannot open ULog file: {e}")

    tunnel_dataset = next(
        (d for d in u.data_list if d.name == 'mavlink_tunnel'), None)
    if tunnel_dataset is None:
        sys.exit("No 'mavlink_tunnel' topic found in the log file.")

    data = tunnel_dataset.data
    n = len(data['timestamp'])

    # Build sorted index by timestamp (ULog data is usually already sorted)
    payload_keys = [f'payload[{i}]' for i in range(128)]

    cycles = {}

    for i in range(n):
        if int(data['payload_type'][i]) != AIRDOS_PAYLOAD_TYPE:
            continue
        plen = int(data['payload_length'][i])
        payload = bytes(int(data[k][i]) for k in payload_keys[:plen])
        process_payload(payload, cycles)


if __name__ == "__main__":
    main()
