# 02 — Raspberry Pi host

This prepares a Raspberry Pi to control the ESP32 over the header UART and SPI.
USB needs no setup beyond the Python package in step 3. The steps are written
for a **Raspberry Pi 5** running Raspberry Pi OS 64-bit (Debian 13 "trixie").
On other models only the UART step changes; see
[Other Raspberry Pi models](#other-raspberry-pi-models).

Before you start:

- The ESP32 is flashed, and its boot log shows all three channels listening.
  See [01 — PC toolchain](01-pc-toolchain.md#6-serial-monitor-and-the-uart0-command-channel).
- It is wired to the Pi as described under **Links** in
  [`tools/pi/README.md`](../tools/pi/README.md#links), with a common GND.

Commands that need root start with `sudo`; run everything else as your normal
user. The examples assume the Pi answers as `rpi.local` and your login name is
`<user>`.

## 1. Enable the header UART and SPI

Both are device-tree switches in `/boot/firmware/config.txt`:

| Line | Effect |
|------|--------|
| `dtparam=uart0=on` | routes UART0 to GPIO14/15 (header pins 8 and 10) → `/dev/ttyAMA0` |
| `dtparam=spi=on` | enables SPI0 on GPIO8–11 → `/dev/spidev0.0` (CE0) and `/dev/spidev0.1` (CE1) |

The stock file ends with an `[all]` section, so lines appended to the end apply
to every model. These commands add each line only if it isn't already there,
so they are safe to run twice:

```sh
grep -qx 'dtparam=uart0=on' /boot/firmware/config.txt || echo 'dtparam=uart0=on' | sudo tee -a /boot/firmware/config.txt
grep -qx 'dtparam=spi=on'   /boot/firmware/config.txt || echo 'dtparam=spi=on'   | sudo tee -a /boot/firmware/config.txt
```

Check that the two lines come after the last section header, and that this
header is `[all]`. Your line numbers may differ from the example; what matters
is the order:

```sh
grep -nE '^\[|^dtparam=(uart0|spi)=' /boot/firmware/config.txt
```

```
42:[cm4]
48:[cm5]
51:[pi5]
54:[all]
55:dtparam=spi=on
56:dtparam=uart0=on
```

A line that lands under `[cm4]`, `[cm5]` or `[pi5]` applies only to that model.
Then reboot:

```sh
sudo reboot
```

For SPI, `sudo raspi-config` → *Interface Options* → *SPI* does the same thing.
For the UART, use the `config.txt` line. raspi-config's *Serial Port* option is
built around `/dev/serial0`, and on a Pi 5 that is the debug connector, not the
header pins (see the next step).

## 2. Leave the serial console where it is

On a Pi 5, the kernel console (`console=serial0,115200` in
`/boot/firmware/cmdline.txt`) and its login prompt (`serial-getty@ttyAMA10`)
both live on the 3-pin debug connector between the two HDMI ports.
`/dev/serial0` points there too:

```sh
ls -l /dev/serial0
```

```
lrwxrwxrwx 1 root root 8 … /dev/serial0 -> ttyAMA10
```

Nothing else uses `/dev/ttyAMA0`, so `cmdline.txt` needs no edit.

The catch is that **`/dev/serial0` is never the right port for the ESP32 on a
Pi 5**. If you run `meshctl.py --uart` without naming a port and
`/dev/ttyAMA0` doesn't exist, it falls back to `/dev/serial0`. On a Pi where
step 1 didn't take effect, your commands then go to the debug console. While
setting up, always name the port (`--uart /dev/ttyAMA0`), so that a missing
UART fails with an error instead.

## 3. Install the Python packages

```sh
sudo apt update
sudo apt install python3-serial python3-spidev
```

| Package | Python module | Used for |
|---------|---------------|----------|
| `python3-serial` | `serial` (pyserial) | USB and UART links |
| `python3-spidev` | `spidev` | SPI link |

Install them with apt, not pip. Raspberry Pi OS marks its Python as externally
managed, so `pip install` outside a virtual environment is refused. If you run
the tools from a virtual environment, create it with `--system-site-packages`
so it can see these two packages. Never run `pip install serial`: that is an
unrelated package that also uses the name `serial`.

Check:

```sh
python3 -c 'import serial, spidev; print("pyserial", serial.VERSION)'
```

```
pyserial 3.5
```

## 4. Permissions

The device files belong to groups, and your user must be in both:

| Device | Owner and mode | Group you need |
|--------|----------------|----------------|
| `/dev/ttyAMA0`, `/dev/ttyUSB0` | `root:dialout`, `0660` | `dialout` |
| `/dev/spidev0.0`, `/dev/spidev0.1` | `root:spi`, `0660` | `spi` |

The first account that Raspberry Pi Imager creates is already in both groups.
Check:

```sh
id -nG
```

The list must include `dialout` and `spi`. If either is missing, add it, then
log out and back in. Group changes only apply to new login sessions.

```sh
sudo usermod -aG dialout,spi "$USER"
```

## 5. Install the tools

`meshctl.py` and `mesh_survey.py` are plain Python 3 scripts that need nothing
beyond step 3. Keep them in the same folder, because `mesh_survey.py` imports
`meshctl.py` from its own directory.

Copy them from the repository on the PC. Windows 10 includes the OpenSSH
client, so this works from PowerShell:

```powershell
ssh <user>@rpi.local "mkdir -p ~/mesh"
scp tools/pi/meshctl.py tools/pi/mesh_survey.py tools/pi/README.md <user>@rpi.local:~/mesh/
```

Run the `scp` again whenever either script changes; there is no other install
step. You can also clone the repository on the Pi and run the scripts from
`tools/pi/` where they are.

Git on Windows may check the scripts out with CRLF line endings (the bench PC
has `core.autocrlf=true`). `python3 meshctl.py` doesn't mind,
but `./meshctl.py` then fails with `/usr/bin/env: 'python3\r': No such file or
directory`. Either always start the scripts with `python3`, or strip the
carriage returns on the Pi:

```sh
sed -i 's/\r$//' ~/mesh/*.py
```

## 6. Verify

Run these checks in order. Each one tests a single layer, so the first failure
tells you where to look in [03 — Troubleshooting](03-troubleshooting.md). Stop
anything else that is using the ports first. Linux doesn't lock serial ports,
so two programs on one port take each other's bytes.

**The device files exist, with the right groups:**

```sh
ls -l /dev/ttyAMA0 /dev/spidev0.0
```

```
crw-rw---- 1 root spi     153,  0 … /dev/spidev0.0
crw-rw---- 1 root dialout 204, 64 … /dev/ttyAMA0
```

**The pins carry UART and SPI:**

```sh
pinctrl get 14,15
pinctrl get 8-11
```

```
14: a4    pn | hi // GPIO14 = TXD0
15: a4    pu | hi // GPIO15 = RXD0
 8: op dh pu | hi // GPIO8 = output
 9: a0    pn | lo // GPIO9 = SPI0_MISO
10: a0    pn | lo // GPIO10 = SPI0_MOSI
11: a0    pn | lo // GPIO11 = SPI0_SCLK
```

GPIO8 (CE0) shows as a plain `output`, and that is correct: on the Pi 5 the SPI
driver works the chip-select line as a GPIO. `hi` is its idle, deselected
level.

**The ESP32 answers on each link:**

```sh
cd ~/mesh
python3 meshctl.py --uart /dev/ttyAMA0 MODE?
python3 meshctl.py --spi 0.0 MODE?
```

Each command prints one line and exits with status 0:

```
MODE NODE
```

A gateway answers `MODE GATEWAY`. `MODE?` is used here because its reply is a
single short line. Long replies over SPI depend on the firmware version; see
[03 — Troubleshooting](03-troubleshooting.md#raspberry-pi-spi).

If the board is also plugged into the Pi over USB, test that link as well:

```sh
python3 meshctl.py MODE?
```

With no link named, `meshctl.py` uses USB and finds the board under
`/dev/serial/by-id/`.

The installation is complete when all of these checks pass. For everyday use,
see [`tools/pi/README.md`](../tools/pi/README.md#usage). Note that
`mesh_survey.py` switches the board to `GATEWAY` if it isn't one already.

## Other Raspberry Pi models

Only the UART step is different. SPI, the packages, the groups and the tools
are the same on every model.

On a Pi 4, Pi 3 or Zero 2 W, enable the header UART with `sudo raspi-config` →
*Interface Options* → *Serial Port*: answer **No** to the login shell and
**Yes** to the serial hardware. It then appears as `/dev/serial0`. On these
models `/dev/serial0` *is* the header UART, so `--uart /dev/serial0` is the
right port. [`tools/pi/README.md`](../tools/pi/README.md) has the
model-specific details under **UART**, including the mini-UART on the Pi 3 and
Zero 2 W and the console on older boards.
