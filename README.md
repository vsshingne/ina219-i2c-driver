# INA219 I2C learning driver

This repository is a from-scratch learning driver for TI's INA219 current and
power monitor. It is an out-of-tree Linux I2C client module named
`ina219_demo`; the mainline `ina2xx` hwmon driver is separate and should not be
bound to the same device at the same time. The module exposes voltage, current,
and power values through a small sysfs ABI.

## Hardware and wiring

Assumptions: Raspberry Pi I2C bus 1, address `0x40`, 0.1 Ohm shunt. Connect:

```text
Raspberry Pi                 INA219 breakout
3.3 V (pin 1)  ------------> VCC
GND (pin 6)    ------------> GND
GPIO 2 / SDA (pin 3) ------> SDA
GPIO 3 / SCL (pin 5) ------> SCL

Supply +  -----------------> VIN+   [shunt]   VIN-  ----> Load +
Supply -  ----------------------------------------------> Load -
```

Use low-voltage loads only. Keep the monitored bus below 26 V and current
below about 3 A for the assumed breakout and shunt. Verify your particular
board's shunt and ratings before connecting a load. Enable I2C on Raspberry Pi
OS with `i2c_arm=on` in the applicable `config.txt` or `raspi-config`, and
confirm `/dev/i2c-*` exists.

## Build

On the target Linux system, first check its release and kernel build headers:

```sh
uname -r
cat /etc/os-release
test -e "/lib/modules/$(uname -r)/build"
```

On Raspberry Pi OS, install prerequisites if needed:

```sh
sudo apt install raspberrypi-kernel-headers build-essential device-tree-compiler i2c-tools
```

Build the module against the running kernel:

```sh
make -C driver
make -C driver clean
```

The current development workspace was Windows without WSL or Linux kernel
headers. The module and Linux-only tests have not been compiled or run here;
the driver uses version guards for the I2C probe and remove API transitions at
Linux 6.3 and 6.1, respectively. Those guards could not be checked against the
running kernel's headers here; confirm the callback signatures in the target
headers before relying on the build. `scripts/checkpatch.pl` was not available
in this workspace. The userspace C source was compiled with the available
MinGW GCC 6.3 compiler.

## Bind the device

### Device tree overlay

Compile and install the overlay:

```sh
dtc -@ -I dts -O dtb -o ina219-demo.dtbo dts/ina219-demo-overlay.dts
sudo cp ina219-demo.dtbo /boot/overlays/
```

Add `dtoverlay=ina219-demo` to `/boot/config.txt` (older images) or
`/boot/firmware/config.txt` (newer images), then reboot. The overlay targets
`&i2c1`, declares `custom,ina219-demo` at `0x40`, and supplies the 100000 uOhm
shunt property.

### Runtime instantiation

This does not require rebooting. Ensure no other driver owns the device:

```sh
echo ina219_demo 0x40 | sudo tee /sys/bus/i2c/devices/i2c-1/new_device
```

Load and unload the module with:

```sh
make -C driver
scripts/load.sh
scripts/unload.sh
```

If runtime instantiation happened after module loading, bind it through the
driver's `bind` file under `/sys/bus/i2c/drivers/ina219_demo/` or reload the
module. Remove the instantiated device by writing `0x40` to the bus's
`delete_device` file after unbinding.

## Sysfs ABI

The files appear in `/sys/bus/i2c/devices/<bus>-0040/` while the device is
bound. Values are decimal integers; the shunt setting is in micro-ohms.

| Attribute | Access | Unit | Meaning |
| --- | --- | --- | --- |
| `voltage_mv` | read | mV | Bus voltage. |
| `shunt_voltage_uv` | read | uV | Signed shunt voltage. |
| `current_ua` | read | uA | Signed current. Returns `EOVERFLOW` if OVF is set. |
| `power_uw` | read | uW | Power. Returns `EOVERFLOW` if OVF is set. |
| `shunt_resistor_uohm` | read/write | uOhm | Shunt value; writing updates calibration. |

Allowed shunt values are 1000 through 10000000 uOhm. The current LSB is fixed
at 100 uA for this learning example. See [docs/REGISTER_MAP.md](docs/REGISTER_MAP.md)
for register details and conversion math.

## Tests

Build the plain C reader with `make -C test`. It discovers a bound device or
accepts an explicit sysfs directory:

```sh
test/ina219_read
test/ina219_read -d /sys/bus/i2c/devices/1-0040 -n 5 -i 500
test/ina219_read -s 200000
```

The `-s` option writes the shunt property and verifies its readback. Run the
stub test as root on a Linux test machine with `i2c-stub` and `i2c-tools`:

```sh
sudo scripts/test_with_i2c_stub.sh
```

The script reports a clear skip if the kernel lacks `i2c-stub`. No stub or
hardware tests have been run in the current Windows workspace.

For hardware testing, first check `i2cdetect -y 1` shows `0x40`; then load and
bind the driver, read the sysfs values, compare bus/shunt voltage against a
multimeter, and inspect `dmesg | tail`. Begin with a low-voltage, current-limited
load. No hardware test was performed for this project in this workspace.

## Limitations and debug notes

This example fixes Current LSB at 100 uA and uses a simple 100000 uOhm default.
It does not implement averaging, alert pins, power management, or a hwmon
interface. It is intended for learning rather than production use. See
[docs/INTERVIEW_NOTES.md](docs/INTERVIEW_NOTES.md) for lifecycle and interview
discussion. Check `dmesg` for probe errors and the successful initialization
message.

## Tested on

The userspace reader was compiled with MinGW GCC 6.3 on Windows. No target
kernel, Raspberry Pi, kernel module, i2c-stub, or real hardware was available
in the current workspace, so driver compilation and Linux runtime behavior
remain unverified.

## Feature map

| Feature | Location |
| --- | --- |
| Probe and remove lifecycle | `driver/ina219_demo.c`: `ina219_probe`, `ina219_remove` |
| Device tree binding | `driver/ina219_demo.c`: `ina219_of_match`; `dts/ina219-demo-overlay.dts` |
| Register access | `driver/ina219_demo.c`: `ina219_read_reg`, `ina219_write_reg` |
| Sysfs readings and calibration | `driver/ina219_demo.c`: `*_show`, `shunt_resistor_uohm_store` |
| User-space test application | `test/ina219_read.c` |
| Error handling and overflow | `driver/ina219_demo.c`: probe and sysfs handlers |
| Mutex locking | `driver/ina219_demo.c`: sysfs handlers and probe/remove sequences |
| Logging and `dmesg` debugging | `driver/ina219_demo.c`: `dev_info`, `dev_err`, `dev_warn_ratelimited` |
| Stub checks | `scripts/test_with_i2c_stub.sh` |

## License

GPL-2.0. See [LICENSE](LICENSE).
