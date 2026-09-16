#!/bin/bash
# Automated checks: run the unmodified firmware in simulation and compare
# its output with what the virtual hardware really did.
# Expects build/airdos03_sim and build/fw/*.elf (use: make test).
set -u
cd "$(dirname "$0")/.."
SIM=build/airdos03_sim
FW=build/fw
OUT=build/tests
if [ -x .venv/bin/python ]; then PY=.venv/bin/python; else PY=python3; fi
mkdir -p "$OUT"
fail=0

run_ustdff() {          # name, extra simulator arguments...
	local name=$1; shift
	echo "=== USTDFF: $name"
	$SIM -q --timed "$OUT/$name.txt" --truth "$OUT/$name.csv" "$@" $FW/AIRDOS03_USTDFF.elf \
		2> "$OUT/$name.log" || { echo "simulator failed"; cat "$OUT/$name.log"; fail=1; return; }
	$PY tools/check_ustdff.py "$OUT/$name.txt" "$OUT/$name.csv" > "$OUT/$name.check"
	local rc=$?
	grep -E "FAIL|WARN" "$OUT/$name.check"
	tail -n 1 "$OUT/$name.check"
	[ $rc -eq 0 ] || fail=1
}

run_ustdff default     -t 65
run_ustdff high_rate   -t 65 --rate 2000
run_ustdff fix_loss    -t 75 --fix-loss 20:40
run_ustdff no_sensors  -t 25 --no-sht --no-gnss
run_ustdff scenario    -t 95 --scenario scenarios/short_test.csv --seed 7

if "$PY" -c "import pymavlink" 2>/dev/null; then
	echo "=== MAVLink: default"
	$SIM -q -t 65 --rate 20 -o "$OUT/mavlink.bin" --truth "$OUT/mavlink.csv" \
		$FW/AIRDOS03_MAVLink.elf 2> "$OUT/mavlink.log" || fail=1
	"$PY" tools/decode_mavlink_file.py "$OUT/mavlink.bin" > "$OUT/mavlink.txt" || fail=1
	"$PY" tools/check_mavlink.py "$OUT/mavlink.txt" "$OUT/mavlink.csv" > "$OUT/mavlink.check" || fail=1
	grep -E "FAIL|WARN" "$OUT/mavlink.check"
	tail -n 1 "$OUT/mavlink.check"
else
	echo "=== MAVLink: skipped (pymavlink not installed, run ./setup.sh)"
fi

echo
echo "Detailed reports: $OUT/*.check"
[ $fail -eq 0 ] && echo "ALL TESTS PASSED" || echo "SOME TESTS FAILED"
exit $fail
