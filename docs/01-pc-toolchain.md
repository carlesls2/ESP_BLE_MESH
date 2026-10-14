# 01 — PC toolchain (Windows)

This sets up a Windows PC to build the firmware, flash it over UART0, and talk
to it on the serial console. By the end, the ESP32's boot log reports that all
three command channels are listening: UART0, UART2 and SPI.

Written on Windows 10 22H2. PowerShell commands run from the repository folder.

## 1. USB-serial driver

The ESP32-DevKitC's USB socket connects to a Silicon Labs CP2102 bridge, which
is wired to the ESP32's UART0. Windows needs the CP210x driver to present the
bridge as a COM port.

1. Plug the board in. Use a cable that carries data: charge-only cables are
   common, and with one the PC shows nothing at all.
2. Windows Update normally installs the driver on its own within a minute.
   Check:

   ```powershell
   Get-PnpDevice -Class Ports | Select-Object Status, FriendlyName
   ```

   ```
   Status  FriendlyName
   ------  ------------
   OK      Silicon Labs CP210x USB to UART Bridge (COM4)
   ```

   A status of `Unknown` means Windows remembers the board, but it is not
   plugged in right now.
3. If the board appears under **Other devices** in Device Manager instead,
   download the **CP210x Universal Windows Driver** from
   <https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers> and unzip
   it. In Device Manager, right-click the device → **Update driver** → **Browse
   my computer for drivers**, and pick the unzipped folder.

Note the COM number, because every command below needs it. Windows remembers
the number per board, so it stays the same whichever USB socket you use.

Some clone boards use a WCH CH340 bridge instead. It needs WCH's CH34x driver,
and everything else in this guide is the same.

## 2. PlatformIO

PlatformIO downloads and runs everything else: ESP-IDF, the compiler, esptool
and the serial monitor.

**With VS Code (recommended)**

1. Install VS Code from <https://code.visualstudio.com/>.
2. Open the repository folder. VS Code offers the extension the repository
   recommends in `.vscode/extensions.json`. Accept **PlatformIO IDE**, or
   install it from the Extensions view.
3. Wait for the extension to finish installing PlatformIO Core (watch the
   notifications at the bottom right), then restart VS Code when asked.

The extension puts PlatformIO Core and its own Python in
`%USERPROFILE%\.platformio\penv`, so you don't need to install Python
separately.

You can skip two other extensions. `.vscode/extensions.json` marks the C/C++
Extension Pack as unwanted, because PlatformIO IDE brings in the one C/C++
extension it needs. The Espressif ESP-IDF extension isn't needed either. The
`idf.currentSetup` key in `.vscode/settings.json` belongs to that extension and
points to a separate ESP-IDF 5.1 install. PlatformIO ignores it and builds with
its own ESP-IDF.

**Without VS Code**, install PlatformIO Core on its own with the installer
script:
<https://docs.platformio.org/en/latest/core/installation/methods/installer-script.html>.
It installs to the same `penv` folder.

### `pio` on the command line

A terminal opened with **PlatformIO: New Terminal** in VS Code has `pio` on the
PATH. An ordinary PowerShell window does not. There, either type the full path
or add an alias for the session:

```powershell
Set-Alias pio "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
pio --version
```

```
PlatformIO Core, version 6.1.19
```

To make `pio` available in every window, add
`%USERPROFILE%\.platformio\penv\Scripts` to your user PATH.

## 3. Pin the platform version

The firmware is built and tested with the `espressif32` platform **5.2.0**,
which bundles **ESP-IDF 4.4.2**. `platformio.ini` asks for plain
`espressif32`. On a machine with no copy yet, PlatformIO resolves that to the
newest release, which bundles a newer ESP-IDF this firmware has never been
built against. Once a version is installed, PlatformIO keeps using it until
you run `pio pkg update`. That is why the existing bench PC still builds with
5.2.0.

Before the first build, do one of the following:

- **Recommended:** pin the version in `platformio.ini`, so every machine builds
  with the same one:

  ```ini
  [env:esp32dev]
  platform = espressif32@5.2.0
  ```

- Or install that version before PlatformIO picks one for you:

  ```powershell
  pio pkg install -g -p "platformio/espressif32@5.2.0"
  ```

Check:

```powershell
pio pkg list -e esp32dev
```

The output must include `Platform espressif32 @ 5.2.0` and
`framework-espidf @ 3.40402.0`. PlatformIO numbers ESP-IDF 4.4.2 as 3.40402.0.

## 4. Build

```powershell
pio run -e esp32dev
```

The first build downloads about 750 MB, most of it ESP-IDF and the Xtensa
toolchain, into `%USERPROFILE%\.platformio\packages`, and takes several
minutes. Later builds are incremental. A successful build ends with
`[SUCCESS]`.

## 5. Flash over UART0

`platformio.ini` sets `upload_port = COM3` and `monitor_port = COM3` for the
bench PC. For any other board or PC, give the port on the command line:

```powershell
pio run -e esp32dev -t upload --upload-port COM4
```

esptool 4.2.1, which comes with the platform, toggles DTR and RTS to put the
ESP32 into its ROM bootloader, writes the flash at 460800 baud (the `esp32dev`
default), and then resets the board into the new firmware. The output ends with
`Hard resetting via RTS pin...` and `[SUCCESS]`.

- If it stalls on `Connecting........_____` and then reports `Failed to connect
  to ESP32`, run it again and hold the **BOOT** button until the dots stop.
- If writing fails partway through, add `upload_speed = 115200` to
  `platformio.ini`.
- If the board is powered from the Pi's 5 V pin, disconnect that supply before
  plugging in USB. Power the DevKitC from one source at a time. The UART2 and
  SPI wires can stay connected while you flash.

To wipe the whole flash, use the erase target. This includes the NVS partition
that stores the device mode, the identity and the mesh keys, so the board has
to be provisioned again afterwards:

```powershell
pio run -e esp32dev -t erase --upload-port COM4
```

## 6. Serial monitor and the UART0 command channel

```powershell
pio device monitor -p COM4
```

The monitor opens at 115200 baud, taken from `monitor_speed` in
`platformio.ini`. If you run it outside the project folder, add `-b 115200`.

Press **EN** on the board to restart it and see the boot log from the
beginning. These three lines prove that all three command channels started.
They can appear in any order:

```
I (…) UART_CMD: listening on UART0 for MODE commands
I (…) UART2_CMD: listening on UART2 (TX 17, RX 16)
I (…) SPI_CMD: listening on SPI3 (MOSI 23, MISO 19, SCLK 18, CS 5)
```

If a channel fails to start, its driver logs an error, followed by one of these
lines:

```
E (…) MAIN: UART command channel unavailable
E (…) MAIN: UART2 (Pi) command channel unavailable
E (…) MAIN: SPI command channel unavailable
```

Now type `MODE?` and press Enter. The monitor doesn't echo what you type; add
`--echo` if you want to see it. The reply appears twice: first as a log line,
then as the plain protocol line that a host parses:

```
I (…) CMD: [uart] MODE NODE
MODE NODE
```

A gateway answers `MODE GATEWAY` instead. `HELP` lists every command.

Quit the monitor with **Ctrl+C**. Until you do, it holds the COM port and
nothing else can open it.

## 7. Optional: `meshctl.py` from the PC

The Pi client also runs on Windows over USB, using PlatformIO's Python, which
already includes pyserial. It can't find the board by itself on Windows, so
name the COM port. Close the serial monitor first.

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" tools\pi\meshctl.py --usb COM4 MODE?
```

```
MODE NODE
```

That completes the PC side. UART2 and SPI are wired to the Pi's header, so
test them from the Pi (the SPI client's `spidev` module is Linux-only anyway).
Continue with [02 — Raspberry Pi host](02-raspberry-pi-host.md).
