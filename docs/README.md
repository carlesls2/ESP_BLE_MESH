# Installing the SPI and UART software

These documents set up every piece of software that talks to the ESP32 over
**USB**, **UART** or **SPI**, on each machine involved. Read them in order.
Each one ends with a check that must pass before you move on.

| Document | Machine | Covers |
|----------|---------|--------|
| [01 — PC toolchain](01-pc-toolchain.md) | Windows PC | USB-serial driver, PlatformIO and ESP-IDF, flashing over UART0, the serial monitor |
| [02 — Raspberry Pi host](02-raspberry-pi-host.md) | Raspberry Pi 5 | header UART and SPI, Python packages, permissions, `meshctl.py` and `mesh_survey.py` |
| [03 — Troubleshooting](03-troubleshooting.md) | both | symptom → cause → fix for each link, and loopback tests that leave the ESP32 out |

Wiring, the command protocol, SPI framing and everyday use of the Pi tools are
in [`tools/pi/README.md`](../tools/pi/README.md). These documents link to it
rather than repeat it.

## What talks to what

| Link | ESP32 end | Host end | Host device | Host software |
|------|-----------|----------|-------------|---------------|
| USB  | UART0 through the on-board CP2102 bridge. It shares the line with the ESP-IDF log. `lib/cmd/uart_cmd.c` | USB cable to the PC or the Pi | `COM3`, `COM4`, … on Windows; `/dev/ttyUSB0` on the Pi | PlatformIO (flash, monitor), `meshctl.py` |
| UART | UART2: TX GPIO17, RX GPIO16. `lib/cmd/uart2_cmd.c` | Pi header pins 8 (TXD) and 10 (RXD) | `/dev/ttyAMA0` | `meshctl.py`, `mesh_survey.py`, through pyserial |
| SPI  | SPI3 (VSPI) slave: MOSI 23, MISO 19, SCLK 18, CS 5. `lib/cmd/spi_cmd.c` | Pi SPI0 master on CE0 | `/dev/spidev0.0` | `meshctl.py`, `mesh_survey.py`, through spidev |

All three links feed one parser, `lib/cmd/cmd_proto.c`, so they all take the
same commands. The reply to a command goes back on the link the command came in
on. Asynchronous lines (`TELEM`, `ATTR`, `TEXT`, `JOINED`, …) go out on all
three.

Flashing always uses USB, because esptool talks to the ESP32's ROM bootloader
on UART0. UART2 and SPI carry commands only.

## Settings both ends must agree on

Nothing negotiates these settings. A mismatch shows up as silence or garbage,
never as an error message.

| Setting | ESP32 (fixed in the firmware) | Host default | Host override |
|---------|-------------------------------|--------------|---------------|
| UART0 and UART2 framing | 115200 baud, 8N1, no flow control | 115200 | `meshctl.py --baud`, `pio device monitor -b` |
| Line ending | CR, LF or CRLF | LF (`meshctl.py`), CRLF (`pio device monitor`) | — |
| Longest command | 95 characters (62 over SPI, which `meshctl.py` sends as one frame) | — | — |
| SPI mode | 0 (CPOL 0, CPHA 0) | 0 | — |
| SPI clock | set by the host | 1 MHz | `meshctl.py --speed` |
| SPI transaction | exactly 64 bytes | 64 bytes | — |
| SPI chip select | GPIO5, active low | CE0 → `/dev/spidev0.0` | `meshctl.py --spi 0.1` for CE1 |
| Logic level | 3.3 V | 3.3 V | none. Wire directly, with a common GND |

## Reference versions

These versions were read from the working bench PC and Pi on 2026-09-26.
Newer versions of most of this will probably work but have not been tried. The
exception is ESP-IDF — see
[Pin the platform version](01-pc-toolchain.md#3-pin-the-platform-version).

**Windows 10 PC**

| Software | Version | Role |
|----------|---------|------|
| Silicon Labs CP210x VCP driver | 11.3.0.176 | makes the board's USB bridge a COM port |
| VS Code with the PlatformIO IDE extension | VS Code 1.135.0 | editor front end (optional) |
| PlatformIO Core | 6.1.19 | build, upload, serial monitor |
| Platform `espressif32` | 5.2.0 | bundles the next three rows |
| ESP-IDF (`framework-espidf` 3.40402.0) | 4.4.2 | the UART and SPI-slave drivers the firmware links against |
| Xtensa toolchain | 8.4.0+2021r2-patch3 | compiler |
| esptool (`tool-esptoolpy` 1.40201.0) | 4.2.1 | flashes over UART0 |
| Python and pyserial, inside PlatformIO | 3.11.7 and 3.5 | serial monitor; `meshctl.py` on the PC |

**Raspberry Pi 5**

| Software | Version | Role |
|----------|---------|------|
| Raspberry Pi OS 64-bit (Debian 13 "trixie") | kernel 6.18.34+rpt-rpi-2712 | |
| `dtparam=uart0=on` in `config.txt` | — | header UART → `/dev/ttyAMA0` |
| `dtparam=spi=on` in `config.txt` | — | SPI0 → `/dev/spidev0.0` and `/dev/spidev0.1` |
| Python | 3.13.5 | |
| `python3-serial` (pyserial) | 3.5-2 | USB and UART links |
| `python3-spidev` | 3.6-1+b6 | SPI link |
| `raspi-utils` (`pinctrl`) | 20260601-1 | shows pin functions, for diagnostics only |

**ESP32 firmware**: built from this repository. A running board reports its
version with `ID GET FW_VER`.
