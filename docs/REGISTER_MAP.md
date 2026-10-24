# INA219 register map and scaling

The INA219 uses 16-bit registers transmitted most-significant byte first. The
driver uses SMBus word helpers with byte swapping to account for SMBus word
helpers' low-byte-first convention.

| Address | Register | Meaning |
| --- | --- | --- |
| `0x00` | Configuration | Reset value `0x399F`; reset bit is `0x8000`. |
| `0x01` | Shunt voltage | Signed two's-complement value, 10 uV per bit. |
| `0x02` | Bus voltage | Bits 15:3, 4 mV per bit; bit 1 CNVR, bit 0 OVF. |
| `0x03` | Power | Unsigned value, 20 times Current LSB per bit. |
| `0x04` | Current | Signed value, configured Current LSB per bit. |
| `0x05` | Calibration | Calibration written by the driver and checked at probe. |

The default current LSB is 100 uA. Calibration uses integer micro-units:

`Cal = trunc(40960000000 / (current_lsb_ua * shunt_uohm))`

For 100 uA and a 100000 uOhm (0.1 Ohm) shunt, `Cal = 4096`.

Conversions: `shunt_voltage_uv = signed_raw * 10`; `voltage_mv =
(bus_raw >> 3) * 4`; `current_ua = signed_current_raw * current_lsb_ua`;
`power_uw = power_raw * 20 * current_lsb_ua`.
