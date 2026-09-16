#!/usr/bin/env python3
"""
Compare AIRDOS03 USTDFF output produced in simulation with the ground
truth logged by the virtual hardware.

    check_ustdff.py timed.txt truth.csv [--strict]

timed.txt  - from airdos03_sim --timed   (t_start, t_end, line)
truth.csv  - from airdos03_sim --truth

Exit code 0 when all hard checks pass, 1 otherwise. Findings about the
firmware output format are reported as WARN (FAIL with --strict).
"""
import argparse
import bisect
import collections
import math
import sys

THRESHOLD = 64
MAX_EVENTS = 300


class Report:
    def __init__(self, strict):
        self.strict = strict
        self.failed = False

    def ok(self, name, msg=""):
        print(f"  OK    {name}" + (f" - {msg}" if msg else ""))

    def fail(self, name, msg):
        print(f"  FAIL  {name} - {msg}")
        self.failed = True

    def warn(self, name, msg):
        print(f"  WARN  {name} - {msg}")
        if self.strict:
            self.failed = True

    def info(self, msg):
        print(f"        {msg}")


def load_timed(path):
    lines = []
    with open(path) as f:
        for raw in f:
            t0, t1, text = raw.rstrip("\n").split("\t", 2)
            lines.append((float(t0), float(t1), text))
    return lines


def load_truth(path):
    cfg, reads, sht, pps = {}, [], [], []
    with open(path) as f:
        next(f)
        for raw in f:
            parts = raw.rstrip("\n").split(",")
            t, kind, data = float(parts[0]), parts[1], parts[2:]
            if kind == "config":
                for kv in data:
                    k, v = kv.split("=", 1)
                    cfg[k] = v
            elif kind == "read":
                reads.append((t, int(data[0])))
            elif kind == "sht31":
                sht.append((t, float(data[0]), float(data[1])))
            elif kind == "pps":
                pps.append((t, int(data[0])))
    return cfg, reads, sht, pps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("timed")
    ap.add_argument("truth")
    ap.add_argument("--strict", action="store_true", help="treat format warnings as failures")
    args = ap.parse_args()

    lines = load_timed(args.timed)
    cfg, reads, sht, pps = load_truth(args.truth)
    rep = Report(args.strict)
    utc_start = int(cfg["utc_start"])
    read_t = [r[0] for r in reads]

    def by_prefix(p):
        return [(t0, t1, l) for (t0, t1, l) in lines if l.startswith(p)]

    # ------------------------------------------------------------------
    print("Header")
    dos = by_prefix("$DOS,")
    if len(dos) != 1:
        rep.fail("$DOS", f"expected exactly one line, got {len(dos)}")
    else:
        sn = dos[0][2].split(",")[-1]
        if sn == cfg["sn"]:
            rep.ok("$DOS serial number", sn)
        else:
            rep.fail("$DOS serial number", f"{sn} != EEPROM {cfg['sn']}")

    adc = by_prefix("$ADC,")
    if len(adc) != 1:
        rep.fail("$ADC", f"expected exactly one line, got {len(adc)}")
    else:
        f = adc[0][2].split(",")
        if f[2] != cfg["sn"]:
            rep.fail("$ADC serial number", f"{f[2]} != {cfg['sn']}")
        else:
            rep.ok("$ADC serial number")
        if f[3] == cfg["adc_conf"]:
            rep.ok("$ADC config bytes", f[3])
        elif f[3].upper() == "%X%X" % (int(cfg["adc_conf"][:2], 16), int(cfg["adc_conf"][2:], 16)):
            rep.warn("$ADC config bytes",
                     f"printed '{f[3]}' for EEPROM bytes {cfg['adc_conf'][:2]} {cfg['adc_conf'][2:]} "
                     "- Serial.print(x, HEX) drops the leading zero, the two bytes cannot be separated")
        else:
            rep.fail("$ADC config bytes", f"{f[3]} != {cfg['adc_conf']}")

    # ------------------------------------------------------------------
    print("Environment ($ENV vs SHT31 model)")
    env = by_prefix("$ENV,")
    bad = 0
    worst_t = 0.0
    for (t0, t1, l) in env:
        f = l.split(",")
        temp, rh = float(f[3]), float(f[4])
        prev = [s for s in sht if s[0] <= t0]
        if not prev:
            rep.fail("$ENV", f"line at {t0:.3f}s without SHT31 measurement")
            bad += 1
            continue
        _, tt, trh = prev[-1]
        worst_t = max(worst_t, abs(temp - tt))
        if abs(temp - tt) > 0.051 or abs(rh - trh) > 0.051:
            rep.fail("$ENV", f"at {t0:.3f}s reported {temp}/{rh}, sensor {tt:.3f}/{trh:.3f}")
            bad += 1
    expected_env = 1 + int((lines[-1][0] - (env[0][0] if env else 0)) // 30) if env else 0
    if not bad and env:
        rep.ok("$ENV values", f"{len(env)} records, max temperature error {worst_t:.3f} C")
    if len(env) < expected_env:
        rep.fail("$ENV count", f"{len(env)} records, expected about {expected_env}")

    # ------------------------------------------------------------------
    print("Time keeping")
    fix_at = float(cfg["fix_at"])
    time_lines = by_prefix("$TIME,")
    if cfg.get("gnss") == "1" and fix_at >= 0:
        if not time_lines:
            rep.fail("$TIME", "no $TIME record although GNSS has a fix")
        for (t0, t1, l) in time_lines:
            f = l.split(",")
            cur = int(f[3])
            exp = utc_start + math.floor(t0)
            if cur != exp:
                rep.fail("$TIME", f"at {t0:.3f}s current_unix {cur}, expected {exp}")
            else:
                rep.ok("$TIME", f"at {t0:.3f}s -> {f[5]}")

    stops = by_prefix("$STOP,")
    starts = by_prefix("$START,")
    loss_from, loss_to = map(float, cfg.get("fix_loss", "-1:-1").split(":"))
    err_intended, err_printed, err_holdover = [], [], []
    for (s, st) in zip(starts, stops):
        tm_field = st[2].split(",")[2]
        sec, _, frac = tm_field.partition(".")
        if int(sec) == 0:
            continue                    # not synchronised yet
        true_unix = utc_start + s[0]    # snapshot taken right before $START
        intended = int(sec) + int(frac) / 100.0
        printed = float(tm_field)
        if loss_from <= s[0] < loss_to + 1:
            # no PPS pulses: time is not expected to advance
            err_holdover.append(abs(intended - true_unix))
            continue
        err_intended.append(abs(intended - true_unix))
        err_printed.append(abs(printed - true_unix))
    if err_holdover:
        rep.warn("$STOP time without PPS",
                 f"{len(err_holdover)} cycles during GNSS fix loss, error up to "
                 f"{max(err_holdover):.1f} s - time only advances on PPS pulses (no holdover)")
    if err_intended:
        if max(err_intended) < 0.02:
            rep.ok("$STOP time (sec + tm_s100/100)", f"max error {max(err_intended)*1000:.1f} ms")
        else:
            rep.fail("$STOP time", f"max error {max(err_intended):.3f} s")
        if max(err_printed) > 0.02:
            rep.warn("$STOP time as printed",
                     f"read as a decimal number the error is up to {max(err_printed):.2f} s - "
                     "tm_s100 is printed without zero padding (3 hundredths -> '.3')")

    # ------------------------------------------------------------------
    print("Particle accounting ($START/$E/$STOP vs ADC reads)")
    # W_k: reads between the end of the previous $STOP line and the start of
    #      $START - all of them must be reported in cycle k.
    # A_k, B_k: reads while the previous / this cycle was being printed. They
    #      may or may not be reported (serviceADC() runs inside DataOut()).
    header_end = next((t1 for (t0, t1, l) in lines if l.startswith("$ENV,")), 0.0)
    prev_start, prev_end = header_end, header_end
    total_reported = 0
    channel_mismatch = 0
    events_truncated = 0
    cycles = 0
    for (s, st) in zip(starts, stops):
        cyc = int(s[2].split(",")[1])
        f = st[2].split(",")
        if int(f[1]) != cyc:
            rep.fail("cycle numbering", f"$START {cyc} closed by $STOP {f[1]}")
            continue
        ev_count = int(f[4])
        hist = list(map(int, f[5:5 + THRESHOLD]))
        if len(hist) != THRESHOLD:
            rep.fail("$STOP", f"cycle {cyc}: {len(hist)} histogram bins")
            continue
        ev_lines = [l for (t0, t1, l) in lines if l.startswith("$E,") and s[0] <= t0 <= st[0]]
        ev_channels = [int(l.split(",")[2]) for l in ev_lines]
        if len(ev_lines) != min(ev_count, MAX_EVENTS):
            rep.fail("$E lines", f"cycle {cyc}: {len(ev_lines)} lines, events_count {ev_count}")
        if ev_count > MAX_EVENTS:
            events_truncated += 1

        def n_reads(a, b):
            return bisect.bisect_left(read_t, b) - bisect.bisect_left(read_t, a)

        iw0, iw1 = bisect.bisect_right(read_t, prev_end), bisect.bisect_left(read_t, s[0])
        window = [v for (_, v) in reads[iw0:iw1]]
        n_a = n_reads(prev_start, prev_end) if cycles else 0
        n_b = n_reads(s[0], st[1])

        reported = ev_count + sum(hist)
        truth_hist = collections.Counter(v for v in window if v < THRESHOLD)
        truth_ev = collections.Counter(v for v in window if v >= THRESHOLD)
        missing_hist = sum(max(0, truth_hist[c] - hist[c]) for c in range(THRESHOLD))
        missing_ev = 0 if ev_count > MAX_EVENTS else sum(
            (truth_ev - collections.Counter(ev_channels)).values())
        if missing_hist or missing_ev:
            channel_mismatch += 1
            rep.fail("cycle content", f"cycle {cyc}: {missing_hist} histogram and {missing_ev} "
                     "event values read by the firmware are not in the output")
        if not (len(window) <= reported <= len(window) + n_a + n_b):
            rep.fail("cycle count", f"cycle {cyc}: reported {reported}, reads in window "
                     f"{len(window)} (+{n_a}/+{n_b} while printing)")
        total_reported += reported
        prev_start, prev_end = s[0], st[1]
        cycles += 1

    total_reads = bisect.bisect_left(read_t, prev_start) - bisect.bisect_left(read_t, header_end) \
        if cycles else 0
    if cycles and not channel_mismatch:
        rep.ok("cycle content", f"{cycles} cycles, every ADC value read inside an integration "
               "window appears in the histogram or as $E")
    if events_truncated:
        rep.info(f"{events_truncated} cycles exceeded MAX_EVENTS={MAX_EVENTS} (by design)")
    if total_reads:
        lost = total_reads - total_reported
        rep.info(f"ADC values read before the last $START: {total_reads}, reported: {total_reported}")
        if lost > 0:
            rep.warn("reads during data output",
                     f"about {lost} values ({100.0*lost/total_reads:.2f} %) were read by serviceADC() while "
                     "DataOut() was printing and then cleared by memset() - they appear in no cycle")

    print("\nRESULT:", "FAIL" if rep.failed else "PASS")
    return 1 if rep.failed else 0


if __name__ == "__main__":
    sys.exit(main())
