# Raspberry Pi ↔ ESP32 mesh-node link

Connects a Raspberry Pi to the ESP32 gateway and gives it the full host command
set (`MODE`, `ID`, `ASK`, `TELEM`, `NODES`, `SEND`, `HELP`) over **USB**,
**UART**, or **SPI**.

Two programs:

| | |
|---|---|
| `meshctl.py` | the link itself — one-shot commands, an interactive REPL, and a `MeshLink` class other scripts import |
| `mesh_survey.py` | asks every node in the mesh how it is doing, as a table, JSON, or CSV |

## Quick start (USB — no wiring needed)

Plug the gateway board into the Pi and:

```sh
sudo apt install python3-serial
python3 mesh_survey.py
```

```
addr    name        uptime  heap    battery  rssi  reset    role
------  ----------  ------  ------  -------  ----  -------  --------------------
0x0001  unnamed     1h 19m  135.6K  -        -25   POWERON  GATEWAY (this board)
0x0005  bench-node  1h 18m  136.2K  -        -23   POWERON  NODE
```

The attached gateway is listed alongside the nodes it provisioned. Its row is
read locally with `ID?`, not polled over the mesh, because a gateway cannot
answer its own `TELEM` — see **Checking the gateway** below. `--no-gateway`
omits it.

`battery` shows `-` until a divider is configured — see **Battery** below.

## Links

### USB (recommended)

The board's own USB-serial bridge appears on the Pi as `/dev/ttyUSB0` (CP210x)
or `/dev/ttyACM0`. `meshctl.py` finds it automatically, preferring the stable
`/dev/serial/by-id/…` path. No wiring, no `config.txt` edits.

The one cost is that USB is **UART0, which is also the ESP-IDF console**, so log
output shares the line. Both sides handle it (see **Log noise** below).

You need to be in the `dialout` group:

```sh
sudo usermod -aG dialout $USER   # then log out and back in
```

### UART (ESP32 UART2 — dedicated, no log noise)

| Pi header pin | Pi BCM name    | Dir | ESP32 pin    |
|---------------|----------------|-----|--------------|
| 8             | GPIO14 / TXD   | →   | GPIO16 (RX2) |
| 10            | GPIO15 / RXD   | ←   | GPIO17 (TX2) |
| 6             | GND            | —   | GND          |

- **Pi 5**: add `dtparam=uart0=on` to `/boot/firmware/config.txt` and reboot.
  The header UART appears as `/dev/ttyAMA0`. No console conflict — the Pi 5
  serial console lives on the dedicated 3-pin debug connector.
- **Pi 3 / 4 / Zero 2 W**: `sudo raspi-config` → Interface Options → Serial
  Port → login shell **No**, serial hardware **Yes**. Use `/dev/serial0`.
  On Pi 3 / Zero 2 W that alias is the mini-UART whose baud tracks the core
  clock — also add `dtoverlay=disable-bt` (or `core_freq=250`) to
  `config.txt` for a stable 115200.
- **Pi 2 / B+**: remove `console=serial0,115200` (or `ttyAMA0`) from
  `/boot/cmdline.txt`, `sudo systemctl disable serial-getty@ttyAMA0.service`,
  reboot. Use `/dev/ttyAMA0`.

### SPI (Pi = master on SPI0/CE0, ESP32 = slave on VSPI)

| Pi header pin | Pi BCM name    | Dir | ESP32 pin |
|---------------|----------------|-----|-----------|
| 19            | GPIO10 / MOSI  | →   | GPIO23    |
| 21            | GPIO9 / MISO   | ←   | GPIO19    |
| 23            | GPIO11 / SCLK  | →   | GPIO18    |
| 24            | GPIO8 / CE0    | →   | GPIO5     |
| 20            | GND            | —   | GND       |

`sudo raspi-config` → Interface Options → SPI → enable. After reboot
`/dev/spidev0.0` exists. `sudo apt install python3-spidev`.

All three buses can be wired at once; they are independent channels. Both sides
are 3.3 V — wire directly, **no level shifter**, but a **common GND is
mandatory**.

## Usage

```sh
# one-shot
python3 meshctl.py MODE?                    # USB is the default link
python3 meshctl.py --usb NODES
python3 meshctl.py --uart TELEM 0xC005
python3 meshctl.py --spi SEND ALL hello from the pi
python3 meshctl.py --usb --json NODES       # parsed JSON

# interactive (async lines print as they arrive)
python3 meshctl.py --usb

# survey
python3 mesh_survey.py                      # table
python3 mesh_survey.py --json               # machine-readable
python3 mesh_survey.py --watch 10           # re-poll every 10 s
python3 mesh_survey.py --csv log.csv        # append timestamped rows
python3 mesh_survey.py --addr 0xC005        # one node
python3 mesh_survey.py --no-gateway         # remote nodes only
```

`--usb PORT` / `--uart PORT` / `--spi BUS.CS` override the defaults. Exit code
is nonzero when the reply is an `ERR` line.

### As a library

```python
from meshctl import MeshLink

with MeshLink(usb=True) as m:
    m.ensure_gateway()
    for node in m.nodes():                       # who is out there
        print(hex(node["addr"]), node["uuid"])
    found, missing = m.telem_sweep([5, 8])       # how are they doing
    names = m.ask_sweep([5, 8], attrs=("NAME",))
```

## Protocol

ASCII lines, newline-terminated, case-insensitive. From the firmware `HELP`:

```
MODE [GATEWAY|NODE]              show or switch the device role
ID? | ID GET <attr>              read local attributes
ID SET <attr> <value>            write a local attribute
ASK <addr|ALL> [attr ...]        query attributes of remote nodes (gateway)
ASK SET <addr> <attr> <value>    write an attribute on a remote node (gateway)
SEND <addr|ALL> <text>           send text to remote nodes (gateway)
TELEM <addr|ALL>                 live vitals, packed (gateway)
NODES                            list nodes this gateway has provisioned
HELP [command]                   show the menu
```

Errors start with `ERR `. Unsolicited lines arrive at any time once the device
is a gateway (or receives a SEND): `JOINED`, `READY`, `MSG`, `ATTR`, `ATTRSET`,
`TEXT`, `TELEM`.

### Attributes

Stored (persisted in NVS, some writable):
`NAME`, `FW_VER`, `HW_VER`, `GROUP_NAME`, `GROUP_ADDR`

Live (computed on every read, always read-only):
`UPTIME`, `HEAP`, `HEAP_MIN`, `RESET`, `UNICAST`, `RSSI`, `BATT_MV`,
`BATT_PCT`, `ROLE`

A live attribute that does not exist on this board reports `-` — no battery
divider fitted, nothing heard yet for `RSSI`, no address until provisioned.
That is a normal answer. `<unreadable>` is reserved for a *stored* value that is
genuinely malformed.

### `ASK` vs `TELEM`

Both fetch node state; they differ in how much air time they cost.

- `ASK` returns TLV attributes, 2 header bytes per value. Asking for everything
  runs to ~155 bytes and segments into 13 blocks. Use it when you want a named
  subset, or the stored identity fields.
- `TELEM` returns one packed 21-byte struct with every vital. Two segments
  instead of thirteen. Use it for the routine "how is everyone doing" sweep.

## Checking the gateway

Three commands answer different questions about the board you are plugged into:

```sh
python3 meshctl.py MODE?     # is it actually a gateway?
python3 meshctl.py ID?       # its own vitals
python3 meshctl.py NODES     # what it has provisioned
```

`ID?` is the one that reports uptime, heap, RSSI and role for the gateway
itself. It reads the local attribute registry directly — the same registry the
mesh carries — so no radio round trip is involved.

**`TELEM <the gateway's own address>` does not work.** The query goes out over
the mesh addressed to the sender, and no reply comes back through the client
model:

```
> TELEM 0x0001
TELEM 0x0001 sent          <- and nothing further
```

That is why `mesh_survey.py` builds the gateway row from `ID?` instead. From a
script:

```python
with MeshLink(usb=True) as m:
    print(m.mode())      # 'GATEWAY'
    print(m.local())     # its own vitals, as a telem-shaped record
```

## Known behaviours worth knowing

### Broadcast replies are not delivered — sweep unicast instead

`TELEM ALL` and `ASK ALL` **do** reach every node, and every node **does**
reply — you can watch it in the node's own log:

```
I (39343) ATTR_MDL: group query: replying in 46 ms
I (39383) ATTR_MDL: telemetry sent to 0x0001 (21 bytes)
```

But ESP-BLE-MESH's client model only accepts a status that matches a pending
transaction, and a group send arms none, so the gateway drops those replies
before the application sees them. A unicast query does arm one, and works.

This is why `mesh_survey.py` uses `NODES` to learn the addresses and then polls
each one unicast. `--broadcast` opts into the group path if you want it. The
same limitation applies to `ASK ALL`, and predates the telemetry work.

### A gateway booted from NVS cannot send until its role is toggled

A board that boots straight into `GATEWAY` (because that role was persisted)
fails every mesh send with `-22` / `-EINVAL`:

```
E (22863) ATTR_MDL: send failed for opcode 0xd402e5, err -22
```

`MODE NODE` followed by `MODE GATEWAY` clears it, after which sends work
normally. This affects `SEND` and `ASK` too, so it is not specific to `TELEM`.
`mesh_survey.py` does not work around it automatically — if every node is
silent on a freshly booted gateway, toggle the role and retry.

### Opening the port must not reset the board

pyserial asserts DTR and RTS by default, and on most ESP32 boards those drive
EN/BOOT — so a naive `serial.Serial("/dev/ttyUSB0")` **reboots the chip**, which
loses any mesh state the stack has not yet flushed to NVS. `meshctl.py` holds
both lines deasserted before opening. If you write your own client, do the same.

### Log noise on USB

UART0 carries both the protocol and `ESP_LOG` output. Two things keep that
workable:

- The firmware writes host replies through `stdout` in a single call, so they
  share the lock `ESP_LOG`'s `vprintf` takes. Without this, a log line lands
  *inside* a reply and corrupts both — which no host-side filter can repair.
- `meshctl.py` drops whole lines matching the `I (12345) TAG: …` shape and
  strips ANSI colour.

The filter is a heuristic: a log line whose message happens to begin with
`TELEM`/`ATTR`/`NODE` would slip through. If that ever bites, use UART2 or SPI,
which carry no log traffic.

### Battery

`BATT_MV` and `BATT_PCT` report `-` until you tell the firmware where the
divider is. In `platformio.ini`:

```ini
build_flags =
    -DBATT_ADC_GPIO=34        ; the pin your divider actually feeds
    -DBATT_DIVIDER_NUM=2      ; vbatt_mv = measured_mv * NUM / DEN
    -DBATT_DIVIDER_DEN=1      ; two equal resistors -> 2/1
```

Only ADC1 pins (**32–39**) work — ADC2 is claimed by the radio and reads fail
while the mesh is up. Prefer 34–39: they are input-only and cannot be driven by
mistake.

It ships commented out on purpose. An unconfigured ADC pin floats and returns a
plausible-looking voltage (GPIO34 measured 284 mV on a bench board with no
divider), and a wrong number is worse than an honest `-`. Check the reading
against a multimeter once; a value off by roughly the divider ratio means
`NUM`/`DEN` are backwards.
