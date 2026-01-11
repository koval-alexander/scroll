# Copilot Instructions for Bluetooth Scroll Wheel Project

## Project Overview
This is a **Nordic nRF52840 Zephyr RTOS firmware** for a Bluetooth Low Energy HID scroll wheel device. It uses an **AMS AS5600 magnetic rotary encoder** (via I2C) to detect rotation and sends scroll events as a BLE HID mouse to paired computers.

**Key Architecture:**
- **Magnetometer thread** ([src/magnetometer.c](../src/magnetometer.c)): Polls AS5600 sensor every 20ms, converts rotation deltas to scroll steps, queues scroll events
- **HID service** ([src/main.c](../src/main.c)): Processes scroll queue, sends HID input reports (ID 1: wheel+buttons, ID 2: motion)
- **Pairing/advertising** ([src/pairing.c](../src/pairing.c)): Manages multi-peer bonding with directed advertising to bonded devices

## Build System (Zephyr/West)
This is a **Nordic Connect SDK (NCS)** project using Zephyr RTOS.

**Standard build commands:**
```bash
# Build for Xiao BLE nRF52840
west build -b xiao_ble/nrf52840/nrf52840

# Build with Bluetooth RPC (network core isolation)
west build -b xiao_ble/nrf52840/nrf52840 -S nordic-bt-rpc -- -DFILE_SUFFIX=bt_rpc

# Flash
west flash
```

**Configuration files:**
- [prj.conf](../prj.conf): Main Kconfig settings (BT device name, security, sensor enable)
- [xiao_ble_nrf52840.overlay](../xiao_ble_nrf52840.overlay): Device tree overlay for AS5600 sensor on I2C1 and button on GPIO0.18
- [Kconfig](../Kconfig): Custom options for security and directed advertising
- [Kconfig.sysbuild](../Kconfig.sysbuild): Sysbuild settings for IPC radio core

## Critical Patterns

### HID Report Descriptor
The device exposes **two input reports** (see [main.c#L109-L149](../src/main.c#L109-L149)):
- **Report ID 1** (wheel+buttons): 2 bytes - button bitmap (5 bits) + padding + wheel delta (int8)
- **Report ID 2** (motion): 3 bytes - X/Y motion as 12-bit signed integers

Scroll sending: `buffer[0] = buttons`, `buffer[1] = scroll_delta` → sent via `bt_hids_inp_rep_send()`

### Sensor Data Flow (Smooth Scrolling)
1. **AS5600** sampled at 50Hz → rotation angle (0-360°)
2. **Wraparound handling** ([magnetometer.c#L62-L67](../src/magnetometer.c#L62-L67)): Delta clamped to ±180° to handle 0/360 crossing
3. **High-res scaling**: `scroll_units = -(angle_delta * 120) / SCROLL_DEGREES_PER_NOTCH` → 120 units per notch
4. **Fractional accumulation**: Sub-unit values preserved in `scroll_accumulator` for smooth motion
5. **Queue**: Integer scroll steps pushed to `scroll_queue` msgq → triggers `hids_work` work item
6. **HID Resolution Multiplier**: Feature report declares 120:1 multiplier for OS smooth scrolling support

### Multi-Peer Bonding with Directed Advertising
- Supports **2 simultaneous connections** (`CONFIG_BT_MAX_CONN=2`)
- On disconnect: cycles through bonded peers with **directed advertising** before falling back to undirected
- `bond_find()` ([pairing.c#L75](../src/pairing.c#L75)): Iterates stored bonds, skips already-connected peers
- To clear all bonds: **press button** → calls `bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY)`

### UICR Reset Pin Configuration
**Critical startup code** ([main.c#L323-L324](../src/main.c#L323-L324)): Disables RESET pin functionality via UICR register writes
```c
write_word_to_uicr(&NRF_UICR->PSELRESET[0], 0);
write_word_to_uicr(&NRF_UICR->PSELRESET[1], 0);
```
This allows GPIO pins normally reserved for RESET to be used as regular I/O. **Device resets after UICR write.**

## Device Tree Specifics
[xiao_ble_nrf52840.overlay](../xiao_ble_nrf52840.overlay) defines:
- **AS5600** at I2C address `0x36` on `&i2c1`
- **Button** on GPIO0.18 (active low) for unpairing
- **UICR** configuration: `gpio-as-nreset` property deleted to enable GPIO use

Access sensor via: `DEVICE_DT_GET_ONE(ams_as5600)` (no DT node label needed)

## Common Modifications
scroll.h](../inc/scroll.h) constants:
```c
#define SCROLL_RESOLUTION_MULTIPLIER 120  // Standard is 120 units per notch
#define SCROLL_DEGREES_PER_NOTCH 6        // Increase for less sensitive, decrease for more
```
Formula: `scroll_units = -(angle_delta * 120) / SCROLL_DEGREES_PER_NOTCH`
- **6° per notch** = very sensitive (default)
- **12° per notch** = moderate sensitivity
- **20° per notch** = low sensitivityc
scroll_delta = -(int8_t)(angle_delta / 3);  // Increase divisor = less sensitive
```

### Adjusting device name
Edit [prj.conf#L13](../prj.conf#L13):
```
CONFIG_BT_DEVICE_NAME="BLE Scroll Wheel"
```

### Disabling security (for testing)
Edit [Kconfig#L12](../Kconfig#L12) or build with `-DCONFIG_BT_HIDS_SECURITY_ENABLED=n`

## Debugging
- **Serial output**: Connect to COM port (see terminals) - device prints scroll deltas, connection events
- **HID reports**: Use nRF Connect Desktop BLE app → "Play" on HID characteristics to view raw bytes
- **Sensor values**: Uncomment [magnetometer.c#L58](../src/magnetometer.c#L58) to print live rotation angles
- **Build artifacts**: Check `build/scroll/` for compile_commands.json and logs

## Dependencies
- **Nordic SDK libraries**: `bt_hids` (NCS HID service), `dk_buttons_and_leds`
- **Zephyr subsystems**: Bluetooth stack, Settings (bond storage), Sensor API
- **Hardware**: Xiao BLE nRF52840, AMS AS5600 magnetic encoder
