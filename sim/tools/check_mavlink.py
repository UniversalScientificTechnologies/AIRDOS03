#!/usr/bin/env python3
"""
Check decoded AIRDOS03 MAVLink output (tools/decode_mavlink_file.py)
against the ground truth of the simulation.

    check_mavlink.py decoded.txt truth.csv

The MAVLink stream carries no transmit timestamps, so cycles are placed
in time using their $STOP time stamp (GNSS time) instead.
"""
import collections
import sys

sys.path.insert(0, __import__("os").path.dirname(__file__))
from check_ustdff import Report, load_truth, THRESHOLD, MAX_EVENTS  # noqa: E402


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    text = [l.rstrip("\n") for l in open(sys.argv[1])]
    cfg, reads, sht, pps = load_truth(sys.argv[2])
    rep = Report(strict=False)
    utc_start = int(cfg["utc_start"])

    print("Header")
    dos = [l for l in text if l.startswith("$DOS,")]
    adc = [l for l in text if l.startswith("$ADC,")]
    if dos and dos[0].split(",")[-1].upper() == cfg["sn"]:
        rep.ok("STARTUP serial number")
    else:
        rep.fail("STARTUP serial number", f"{dos} vs {cfg['sn']}")
    if adc and adc[0].split(",")[-1].upper() == cfg["adc_conf"]:
        rep.ok("STARTUP ADC config", adc[0].split(",")[-1])
    else:
        rep.fail("STARTUP ADC config", f"{adc} vs {cfg['adc_conf']}")

    print("Environment")
    env = [l for l in text if l.startswith("$ENV,")]
    if len(env) != len(sht):
        rep.fail("ENV count", f"{len(env)} packets, {len(sht)} sensor measurements")
    for l, (_, tt, trh) in zip(env, sht):
        f = l.split(",")
        if abs(float(f[3]) - tt) > 0.051 or abs(float(f[4]) - trh) > 0.051:
            rep.fail("ENV value", f"{l} vs {tt:.2f}/{trh:.2f}")
            break
    else:
        rep.ok("ENV values", f"{len(env)} packets")

    print("Time keeping")
    for l in (l for l in text if l.startswith("$TIME,")):
        f = l.split(",")
        rep.ok("TIMESYNC", f[5]) if int(f[3]) - utc_start >= 0 else rep.fail("TIMESYNC", l)

    print("Particle accounting")
    stops = [l.split(",") for l in text if l.startswith("$STOP,")]
    counts = [int(s[1]) for s in stops]
    if counts != list(range(len(counts))):
        rep.fail("cycle sequence", f"{counts}")
    else:
        rep.ok("cycle sequence", f"{len(counts)} complete cycles")
    events = [int(l.split(",")[2]) for l in text if l.startswith("$E,")]
    hist = collections.Counter()
    reported = 0
    last_t = None
    for s in stops:
        sec, frac = s[2].split(".")
        if int(sec):
            last_t = int(sec) + int(frac) / 100.0 - utc_start
        ev_total = int(s[4])
        h = list(map(int, s[5:5 + THRESHOLD]))
        for c, n in enumerate(h):
            hist[c] += n
        reported += ev_total + sum(h)
    truth_vals = collections.Counter(v for (t, v) in reads if last_t is None or t <= last_t + 0.5)
    extra_hist = sum(max(0, hist[c] - truth_vals[c]) for c in range(THRESHOLD))
    extra_ev = sum((collections.Counter(events) - collections.Counter(
        {k: v for k, v in truth_vals.items() if k >= THRESHOLD})).values())
    if extra_hist or extra_ev:
        rep.fail("values", f"{extra_hist} histogram / {extra_ev} event values never read from the ADC")
    else:
        rep.ok("values", "every reported value was produced by the detector model")
    if last_t is not None:
        n_truth = sum(1 for (t, v) in reads if t <= last_t)
        lost = n_truth - reported
        msg = f"reads up to last snapshot {n_truth}, reported {reported}"
        if abs(lost) <= max(3, 0.02 * n_truth):
            rep.ok("counts", msg)
        else:
            rep.fail("counts", msg)

    print("\nRESULT:", "FAIL" if rep.failed else "PASS")
    return 1 if rep.failed else 0


if __name__ == "__main__":
    sys.exit(main())
