#!/bin/sh
# Requires root, i2c-stub, i2c-tools, a built module, and a Linux test host.
set -eu

base=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
driver="$base/driver/ina219_demo.ko"
device=
bus=
passed=0
failed=0

cleanup() {
	if [ -n "$device" ] && [ -e "$device/driver/unbind" ]; then
		printf '%s' "${device##*-}" > "$device/driver/unbind" 2>/dev/null || true
	fi
	rmmod ina219_demo 2>/dev/null || true
	if [ -n "$bus" ]; then
		printf '0x40' > "/sys/bus/i2c/devices/i2c-$bus/delete_device" 2>/dev/null || true
	fi
	modprobe -r i2c-stub 2>/dev/null || true
}
trap cleanup EXIT INT TERM

check() {
	name=$1
	shift
	if "$@"; then
		printf 'PASS: %s\n' "$name"
		passed=$((passed + 1))
	else
		printf 'FAIL: %s\n' "$name"
		failed=$((failed + 1))
	fi
}
expect() { [ "$(cat "$1")" = "$2" ]; }
swap16() {
	value=$(( $1 & 65535 ))
	printf '0x%04x' "$(( ((value & 255) << 8) | (value >> 8) ))"
}
setreg() { i2cset -y "$bus" 0x40 "$1" "$(swap16 "$2")" w; }

if [ "$(id -u)" -ne 0 ]; then
	echo 'Run with sudo: i2c-stub tests need root.' >&2
	exit 2
fi
if ! modprobe i2c-stub chip_addr=0x40; then
	echo 'SKIP: i2c-stub is unavailable in this kernel.'
	echo 'Use the documented hardware procedure instead.'
	exit 77
fi
if [ ! -f "$driver" ]; then
	echo "Build $driver before running this test." >&2
	exit 2
fi
bus=$(i2cdetect -l | awk '/stub/ { for (i = 1; i <= NF; i++) if ($i ~ /^i2c-[0-9]+$/) { sub("i2c-", "", $i); print $i; exit }')
if [ -z "$bus" ]; then
	echo 'FAIL: could not find the i2c-stub bus'
	exit 1
fi
printf 'ina219_demo 0x40\n' > "/sys/bus/i2c/devices/i2c-$bus/new_device"
device="/sys/bus/i2c/devices/${bus}-0040"
# A 0x399F config, bus raw 0x1230 (2.328 V), shunt +0x0010,
# current +0x0019, power +0x0005; i2cset word data is byte swapped.
setreg 0x00 0x399f
setreg 0x01 0x0010
setreg 0x02 0x1230
setreg 0x03 0x0005
setreg 0x04 0x0019
insmod "$driver"
sleep 1
check 'bus voltage converts to millivolts' expect "$device/voltage_mv" 2328
check 'shunt voltage converts to microvolts' expect "$device/shunt_voltage_uv" 160
check 'current converts to microamps' expect "$device/current_ua" 2500
check 'power converts to microwatts' expect "$device/power_uw" 10000

setreg 0x02 0x1231
check 'OVF is reported for current' sh -c "cat '$device/current_ua' >/dev/null 2>&1; test \$? -ne 0"
check 'OVF is reported for power' sh -c "cat '$device/power_uw' >/dev/null 2>&1; test \$? -ne 0"
setreg 0x02 0x1230
printf '200000\n' > "$device/shunt_resistor_uohm"
cal=$(i2cget -y "$bus" 0x40 0x05 w)
expected=$(swap16 2048)
check 'shunt update writes recalculated calibration' test "$cal" = "$expected"
if printf '0\n' > "$device/shunt_resistor_uohm" 2>/dev/null; then
	echo 'FAIL: zero shunt input was accepted'
	failed=$((failed + 1))
else
	echo 'PASS: invalid shunt input rejected'
	passed=$((passed + 1))
fi
rmmod ina219_demo
check 'module unbind/removal completes cleanly' test ! -d "$device/driver"
printf 'Results: %s passed, %s failed\n' "$passed" "$failed"
[ "$failed" -eq 0 ]
