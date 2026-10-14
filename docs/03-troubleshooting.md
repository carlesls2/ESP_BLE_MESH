# 03 — Troubleshooting

Work up through the layers: driver or device file → permissions → pin
functions → wiring → the firmware's channel → the protocol. The checks in
[01](01-pc-toolchain.md) and [02](02-raspberry-pi-host.md) follow the same
order. The loopback tests at the end separate a Pi problem from an ESP32
problem.

## PC: USB and UART0

| Symptom | Cause | Fix |
|---------|-------|-----|
| No COM port appears, and Device Manager shows nothing new | Charge-only USB cable | Use a cable that carries data |
| The board is listed under **Other devices** with a warning icon | CP210x driver missing | Install it, see [01 §1](01-pc-toolchain.md#1-usb-serial-driver) |
| `could not open port 'COM4': PermissionError(13, 'Access is denied.', None, 5)` | Another program holds the port: a serial monitor, `meshctl.py`, or a MobaXterm serial session | Close that program. Windows lets only one program open a COM port at a time |
| Upload stops with `Failed to connect to ESP32` | The automatic reset into the bootloader didn't happen | Hold **BOOT** while `Connecting....` prints |
| Upload fails partway through | Marginal cable or hub at 460800 baud | Add `upload_speed = 115200` to `platformio.ini` |
| The monitor shows garbage | Wrong baud rate | Use `pio device monitor -b 115200`, or run it from the project folder so `monitor_speed` applies |
| The log scrolls, but typed commands get no reply | The UART0 channel didn't start | Look in the boot log for `UART command channel unavailable` and the error above it |
| `pio` is not recognized | PlatformIO's `Scripts` folder is not on the PATH | See [01 §2](01-pc-toolchain.md#pio-on-the-command-line) |

## Raspberry Pi: UART

| Symptom | Cause | Fix |
|---------|-------|-----|
| `/dev/ttyAMA0` doesn't exist | `dtparam=uart0=on` is missing, sits under a model section such as `[cm5]`, or was added without a reboot | See [02 §1](02-raspberry-pi-host.md#1-enable-the-header-uart-and-spi) |
| `meshctl.py --uart` without a port prints `(no reply)` | It fell back to `/dev/serial0`, which is the debug connector on a Pi 5 | Name the port: `--uart /dev/ttyAMA0` |
| `PermissionError: [Errno 13] Permission denied: '/dev/ttyAMA0'` | Not in the `dialout` group, or not logged in again since being added | See [02 §4](02-raspberry-pi-host.md#4-permissions) |
| `ModuleNotFoundError: No module named 'serial'`, or `module 'serial' has no attribute 'Serial'` | pyserial is missing, or pip's unrelated `serial` package is shadowing it | `sudo apt install python3-serial`, and `pip uninstall serial` if you installed that |
| `(no reply)` on `/dev/ttyAMA0` | TX and RX are not crossed, GND is missing, or UART2 didn't start on the ESP32 | Wire Pi pin 8 (TXD) → ESP32 GPIO16, Pi pin 10 (RXD) ← GPIO17, and GND to GND. Look for `listening on UART2` in the boot log. Run the [UART loopback](#uart-loopback) |
| Replies are garbled | Missing GND, or a baud rate other than 115200 | Both ends are fixed at 115200. Check the GND wire |
| Lines go missing, or turn up as the reply to the next command | Another program has the same port open. Linux doesn't lock serial ports, so two readers split the bytes, and opening a port also flushes its input | Find the other program with `fuser -v /dev/ttyAMA0` and stop it. It is often a `meshctl.py` REPL or `mesh_survey.py --watch` |

## Raspberry Pi: SPI

| Symptom | Cause | Fix |
|---------|-------|-----|
| `/dev/spidev0.0` doesn't exist | `dtparam=spi=on` is missing, or was added without a reboot | See [02 §1](02-raspberry-pi-host.md#1-enable-the-header-uart-and-spi) |
| `PermissionError: [Errno 13] Permission denied` when opening SPI | Not in the `spi` group | See [02 §4](02-raspberry-pi-host.md#4-permissions) |
| `ModuleNotFoundError: No module named 'spidev'` | The package is missing, or you are in a virtual environment without system packages | `sudo apt install python3-spidev`. Recreate the environment with `--system-site-packages` |
| `(no reply)` | One of the five wires is wrong: MOSI → 23, MISO ← 19, SCLK → 18, CE0 → 5, GND. Or the wire goes to CE1 (pin 26) while you talk to `0.0`. Or the SPI channel didn't start on the ESP32 | Check each wire. Use `--spi 0.1` if you are wired to CE1. Look for `listening on SPI3` in the boot log. Run the [SPI loopback](#spi-loopback) |
| Reply lines stop at 63 characters (most of `HELP`, every `TELEM`) | Firmware 0.4.0 and earlier send each reply line as one 64-byte frame and cut longer lines | This is a firmware limit, not an installation fault. Check the version with `ID GET FW_VER`. Read long replies over UART, and see the SPI framing in [`tools/pi/README.md`](../tools/pi/README.md) |
| A valid command gets `ERR unknown command`, `ERR command too long`, or nothing, after earlier commands worked | On firmware 0.4.0 and earlier, the ESP32 is not ready for a transfer while it runs a command. A frame clocked in that gap arrives torn, and its leftover bytes stay in front of the next command | Press **EN** on the ESP32 to clear it. Use `meshctl.py` and firmware from the same revision; see [`tools/pi/README.md`](../tools/pi/README.md) |
| Garbled or partial replies | Clock too fast for jumper wires | Stay at the default 1 MHz, or lower it with `--speed 500000` |

## ESP32

| Symptom | Cause | Fix |
|---------|-------|-----|
| The boot log shows `MAIN: … command channel unavailable` | That channel's driver failed to start | The `UART_CMD:`, `UART2_CMD:` or `SPI_CMD:` error just above it names the failing call |
| UART2 is silent on a WROVER board | GPIO16 and GPIO17 are wired to the module's PSRAM | Use a WROOM-based board, or move the UART2 pins in `lib/cmd/uart2_cmd.c` |
| The board resets whenever a host opens the USB port | The client asserts DTR/RTS, which drive EN and BOOT on the DevKitC | `meshctl.py` holds both lines off. Your own clients must do the same (see `tools/pi/README.md`) |

## Loopback tests

These tests check the Pi on its own: that the interface is enabled, the
permissions are right, the Python package works and the header pin is correct.
**Unplug the ESP32's wires from the two pins you bridge first.** Otherwise two
outputs drive the same wire.

If a loopback passes but the ESP32 still doesn't answer, the fault is in the
wiring to the ESP32 or in the ESP32 itself. Check its boot log
([01 §6](01-pc-toolchain.md#6-serial-monitor-and-the-uart0-command-channel)).

### UART loopback

Bridge header pin 8 (TXD) to pin 10 (RXD):

```sh
python3 - <<'EOF'
import serial
with serial.Serial("/dev/ttyAMA0", 115200, timeout=1) as s:
    s.write(b"loopback\n")
    print(s.readline())
EOF
```

Expect `b'loopback\n'`. `b''` means nothing came back. Either the bridge is on
the wrong pins, or the UART is not on GPIO14/15; check with `pinctrl get 14,15`.

### SPI loopback

Bridge header pin 19 (MOSI) to pin 21 (MISO):

```sh
python3 - <<'EOF'
import spidev
spi = spidev.SpiDev()
spi.open(0, 0)
spi.mode = 0
spi.max_speed_hz = 1_000_000
print(spi.xfer2([0x12, 0x34, 0x56, 0x78]))
spi.close()
EOF
```

Expect `[18, 52, 86, 120]`, the same bytes that went out. All zeros or all 255
means MISO isn't receiving what MOSI sends.
