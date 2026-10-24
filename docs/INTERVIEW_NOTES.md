# Interview notes

## Driver lifecycle and design

- **Probe** runs after I2C core matches a client to the device tree compatible
  or I2C ID. It checks adapter support, allocates per-device state, initializes
  registers, verifies calibration readback, and creates sysfs attributes.
- **Remove** switches the chip into power-down mode. Device-managed memory and
  the device-managed sysfs group are released by the driver core.
- **Device tree matching** uses `custom,ina219-demo`; the I2C ID
  `ina219_demo` also supports explicit `new_device` instantiation.
- **Byte swapping** is needed because the INA219 sends MSB first while SMBus
  word-data helpers interpret the wire bytes in little-endian order.
- **Mutex** serializes multistep register reads and calibration changes so two
  sysfs operations cannot interleave and observe a partially updated setup.
- **devm_ helpers** tie state and sysfs-group lifetime to the device, cleaning
  them up automatically after probe failure or unbind.
- **dmesg** reports successful initialization with shunt value, initialization
  errors, overflow warnings, and driver removal. Actual output depends on the
  kernel and hardware; none is fabricated here.

## Likely questions

1. **Why is this an I2C client driver?** The INA219 is a register-mapped I2C
   peripheral enumerated by firmware or explicitly instantiated on a bus.
2. **Why check adapter functionality?** The driver needs SMBus word-data
   transactions, so unsupported adapters must fail early.
3. **Why no chip ID check?** INA219 has no ID register; the driver checks that
   calibration can be written and read back.
4. **Why signed shunt and current values?** Those registers use two's
   complement and current may reverse direction.
5. **What does OVF mean?** The device reports an arithmetic overflow, making
   current and power readings invalid until the condition is addressed.
6. **Why is the bus voltage shifted?** Bits 15:3 hold the 13-bit voltage
   measurement; low bits carry status flags.
7. **Why a mutex?** A calibration update and concurrent measurement must not
   interleave their I2C transactions.
8. **What is `devm_kzalloc`?** It allocates zeroed per-device memory that is
   released automatically when the device unbinds or probe fails.
9. **How does device-tree matching work?** The I2C core compares the node's
   `compatible` string with the driver's OF match table and calls probe.
10. **How would you debug a missing device?** Check wiring and I2C enablement,
    scan the bus, inspect the bound device and kernel log, then verify adapter
    capabilities and register transactions.
