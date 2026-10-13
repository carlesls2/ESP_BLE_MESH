# Raspberry Pi ↔ ESP32 mesh-node link

Connects a Raspberry Pi to the ESP32 gateway and gives it the full host command
set (`MODE`, `ID`, `ASK`, `TELEM`, `NODES`, `NEIGHBORS`, `SEND`, `HELP`) over
**USB**, **UART**, or **SPI**.

Three programs:

| | |
|---|---|
| `meshctl.py` | the link itself — one-shot commands, an interactive REPL, and a `MeshLink` class other scripts import |
| `mesh_survey.py` | asks every node in the mesh how it is doing, as a table, JSON, or CSV |
| `mesh_neighbors.py` | asks every node which boards it hears directly, and prints the links with RSSI |

## Quick start

Plug the gateway board into the Pi over USB and:

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

Wired to the GPIO header instead — see **Links** — add `--uart` for
`/dev/ttyAMA0` or `--spi` for `/dev/spidev0.0`:

```sh
sudo apt install python3-serial python3-spidev
python3 mesh_survey.py --uart
```

## Links

### Power

The board can run off the Pi's 5 V rail, which is what you want for a permanent
install — no USB cable, and the gateway comes up when the Pi does.

| Pi header pin | Pi name | Dir | ESP32 pin                             |
|---------------|---------|-----|---------------------------------------|
| 2             | 5V      | →   | `5V` (bottom pin of the left-hand row) |
| 6             | GND     | —   | any `GND`                             |

**One supply at a time.** Espressif's DevKitC guide allows exactly one power
source: USB, the `5V` pin, or the `3V3` pin. A genuine V4 has a Schottky diode
(D3, BAT760) from USB `VBUS` to the `5V` pin, so the Pi's 5 V cannot flow back
into a PC's USB port — but the reverse path is open: with USB plugged in and
the Pi's supply off, the PC tries to power the Pi through the 5 V wire. Many
clones leave the diode out and tie the two supplies straight together. Feed the
board from the Pi *or* from USB.

To check a board, power it from the Pi only, plug a USB cable into it with the
far end loose, and measure between the two outer pins of the USB-A plug: about
0 V means the diode is there, about 5 V means it is not.

**Flashing still needs USB.** There is no OTA in this firmware. The routine is:
pull the 5 V jumper, plug USB, `pio run -t upload`, unplug USB, reconnect 5 V.
The UART2 and SPI jumpers can stay connected throughout — neither bus is driven
while flashing.

**Do not wire Pi 3V3 (pin 1) to the DevKit `3V3` pin.** That backfeeds the
on-board regulator's output.

The ESP32 idles around 80 mA and peaks at 250–500 mA on a BLE transmit —
comfortable for a Pi 5 on the 27 W supply. On an undersized PSU it is the *Pi*
that browns out first, not the ESP32. A sagging feed is visible rather than
mysterious: the brownout detector runs at its most sensitive setting and `RESET`
reports `BROWNOUT`, so it lands in the `reset` column of `mesh_survey.py`.

### Everything wired at once

Power, commands over UART2, and SPI as a second channel — nine wires:

| Pi header pin | Pi name       | Dir | ESP32 pin    |
|---------------|---------------|-----|--------------|
| 2             | 5V            | →   | `5V`         |
| 6             | GND           | —   | GND          |
| 8             | GPIO14 / TXD  | →   | GPIO16 (RX2) |
| 10            | GPIO15 / RXD  | ←   | GPIO17 (TX2) |
| 19            | GPIO10 / MOSI | →   | GPIO23       |
| 21            | GPIO9 / MISO  | ←   | GPIO19       |
| 23            | GPIO11 / SCLK | →   | GPIO18       |
| 24            | GPIO8 / CE0   | →   | GPIO5 (CS)   |
| 20            | GND           | —   | GND          |

Pi header pin 1 is the corner nearest the USB-C socket, and the odd-numbered
pins are the row closest to the board edge.

On the ESP32 side, hold the DevKitC with the USB socket toward you: **every
signal pin is on the right-hand row.** `GPIO23` is second from the top, and
`GND, GPIO19, GPIO18, GPIO5, GPIO17, GPIO16` are a run of six consecutive pins
(positions 7–12). Only `5V` is on the other side, at the bottom of the left-hand
row.

The sections below cover each bus on its own, and the setup each one needs.

### USB (no wiring, and the flashing path)

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

`sudo raspi-config` → Interface Options → SPI → enable, or add
`dtparam=spi=on` to `/boot/firmware/config.txt`. After reboot
`/dev/spidev0.0` exists. `sudo apt install python3-spidev`.

`meshctl.py` clocks at 1 MHz (`--speed` to change). Measured on a Pi 5 with
jumper wires and firmware 0.5.0: clean from 125 kHz to 8 MHz, including 150
back-to-back `ID?` at 8 MHz without an error. Firmware before 0.5.0 lost frames
at 8 MHz. The default leaves plenty of margin for longer wiring.

The three buses are independent channels — wire as many as you want, as in
**Everything wired at once** above. Both sides are 3.3 V, so wire directly,
**no level shifter**, but a **common GND is mandatory**.

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

# neighbors (radio topology)
python3 meshctl.py NEIGHBORS                # the attached board's own table
python3 meshctl.py NEIGHBORS 0x0005         # ask one node, via the gateway
python3 meshctl.py NEIGHBORS ALL            # ask every provisioned node
python3 meshctl.py --json NEIGHBORS ALL
python3 mesh_neighbors.py                   # links + RSSI matrix
python3 mesh_neighbors.py --watch 30
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
    tables, silent = m.neighbors_all()           # who hears whom
    mine = m.neighbors()                         # the gateway's own table
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
NEIGHBORS                        nodes this board hears directly (RSSI, age)
NEIGHBORS <addr|ALL>             a remote node's neighbors, or every node's (gateway)
RTC [addr|ALL]                   clock, wake cause and duty cycle
TIME SET [addr|ALL] <epoch|ISO>  set the clock (UTC)
TIME SYNC <addr|ALL>             push this board's clock to nodes (gateway)
SLEEP [addr] <LIGHT|DEEP> <s>    sleep for s seconds (not on a gateway)
SLEEP [addr] <mode> UNTIL <time> sleep until a UTC time (epoch/ISO/HH:MM)
DUTY [addr] <mode> <on_s> <off_s> [ALIGN]   repeat awake/asleep
DUTY [addr|ALL] OFF              stop the duty cycle
HELP [command]                   show the menu
```

Errors start with `ERR `. Unsolicited lines arrive at any time once the device
is a gateway (or receives a SEND): `JOINED`, `READY`, `MSG`, `ATTR`, `ATTRSET`,
`TEXT`, `TELEM`, `NEIGH`, `NEIGHBORS`, `RTC`.

### SPI framing

Every transaction is 64 bytes in both directions. The Pi sends a command as
newline-terminated ASCII padded with zeros, then polls with all-zero frames;
each poll returns one queued reply frame:

| First byte | Meaning |
|------------|---------|
| `0x00` | nothing queued |
| printable | a whole line of up to 63 chars, NUL-terminated |
| `0x01` | up to 62 chars of a longer line; more fragments follow |
| `0x02` | the last fragment of a longer line |

A reply starts a couple of frames after its command, so poll until the queue
has been quiet for a while (`meshctl.py` waits 0.3 s) rather than stopping at
the first empty frame. Fragments need firmware 0.5.0 — see **SPI on firmware
older than 0.5.0** below.

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

## Neighbors

Every board (firmware 0.6.0+) keeps a table of the boards it hears
**directly**: one radio hop, not relayed. The Pi can read the table of any
provisioned node through the gateway.

```
> NEIGHBORS ALL
NEIGHBORS ALL started
NEIGH 0x0005 0x0001 rssi=-40 avg=-42 age=3 n=57
NEIGH 0x0005 0x0006 rssi=-70 avg=-71 age=8 n=12
NEIGHBORS 0x0005 count=2
NEIGHBORS 0x0006 count=0
NEIGHBORS 0x0007 noreply
NEIGHBORS ALL done asked=3 replied=2
```

`NEIGH <reporter> <peer> …` means *reporter hears peer*:

| Field | Meaning |
|---|---|
| `rssi` | RSSI of the last packet, in dBm |
| `avg` | running average of the RSSI (1/8 weight per packet) |
| `age` | seconds since the peer was last heard |
| `n` | packets counted |

Entries are sorted strongest `avg` first. A local `NEIGHBORS` uses the same
format, with the board's own address as the reporter.

**How boards find each other.** Every provisioned board sends a one-byte
`HELLO` to all nodes every 8–12 s, with **TTL 0**. A TTL-0 packet is never
relayed, so only boards in radio range hear it.

- Allow about 15 s after boot for the tables to fill.
- A board that goes quiet drops out after 60 s.
- Other traffic also counts, but only when it arrives with the TTL it was
  sent with (7). A relayed packet's RSSI belongs to the relay, not the source,
  so relayed packets are ignored.

The existing OnOff broadcasts can't do this job. A node's OnOff models have no
app key bound, so those packets never reach application code on other nodes.

**`ALL` is a sweep, not a broadcast.** Group-addressed replies are dropped by
the client model (see **Broadcast replies are not delivered** below). So the
gateway asks each provisioned node in turn:

- It waits for each answer, or for the stack's ~4 s timeout, before asking the
  next node.
- It reports `noreply` for silent nodes.
- It ends with the `done` line.

Budget about 6 s per silent node. `meshctl.py` sizes its wait from `NODES`.

**Relay is on** from 0.6.0, so a node out of the gateway's range is reached
through whichever board sits between them. `mesh_neighbors.py` shows which
boards those are.

`mesh_neighbors.py` combines the gateway's own table with the sweep:

```
links (average RSSI in dBm, as heard at each end)
  0x0001 (gw)        <->  0x0005 bench-node  -43  -42
  0x0005 bench-node  <->  0x0006             -71  deaf  one-way
```

The two numbers are what each end hears. Other values in those columns:

- **`deaf`**: that end answered but does not list the other board. The link
  only works one way.
- **`?`**: that end was not heard from.

## Clock, sleep and wake-up

Every board (firmware 0.6.0+) keeps a UTC clock and can sleep on command or on
a schedule. The code is in `lib/rtc`, in two layers:

- **`rtc_dev`** is the low-level device: one interface with two backends.
  - `INTERNAL` is the ESP32's own RTC. It keeps counting through deep sleep
    but loses the date at power-off. It runs off a ~5 %-accurate RC
    oscillator.
  - `DS3231` is an I2C chip with a backup battery, accurate to ±2 ppm. Its
    alarm pin wakes the ESP32.
- **`rtc_mgr`** is the middleware. It handles time, sleeping for a duration or
  until a time, duty cycles, and the wake cause. Firmware code calls it
  directly; the commands below reach it from the host or over the mesh.

Every answer uses one line shape, whether local or remote:

```
RTC 0x0005 OK time=2026-09-30T12:00:00Z valid=1 backend=INTERNAL wake=TIMER duty=light/5/8
```

| Field | Meaning |
|---|---|
| Result (after the address) | `OK`, `BAD_ARG`, `REFUSED` or `FAILED` |
| `valid` | `0` until the clock has been set |
| `wake` | why the board last woke: `RESET` (power-on or reboot), `TIMER`, or `ALARM` (DS3231) |
| `duty` | `off`, or `mode/awake/asleep`, with `/align` added when aligned |

```sh
python3 meshctl.py --uart TIME SET $(date -u +%s)   # set the gateway from the Pi
python3 meshctl.py --uart TIME SYNC ALL             # ...and push it to every node
python3 meshctl.py --uart RTC 0x0005               # one node's clock and schedule
python3 meshctl.py --uart SLEEP 0x0005 DEEP 600    # deep sleep 10 min
python3 meshctl.py --uart SLEEP 0x0005 LIGHT UNTIL 06:00
python3 meshctl.py --uart DUTY 0x0005 DEEP 30 570 ALIGN   # awake 30 s every 10 min
python3 meshctl.py --uart DUTY 0x0005 OFF
python3 meshctl.py --uart --json RTC 0x0005
```

A remote address must be written as `0x....` or `ALL`. A bare number after
`TIME SET` is an epoch, not an address.

**The two sleep modes:**

| | light | deep |
|---|---|---|
| Current | ~0.8 mA | ~10 µA |
| RAM | kept; resumes where it stopped | lost; the chip reboots |
| Back on the mesh after wake | at once | a few seconds (rejoins from NVS) |

In both modes the radio is off, so a sleeping board cannot relay, answer, or
send HELLOs. It drops out of neighbor tables after 60 s. Plan relay paths
around boards that duty-cycle.

**The gateway never sleeps.** `SLEEP` and `DUTY` are refused on a gateway,
because it is the host's only way into the mesh. Also, a gateway booted from
NVS cannot send until its role is toggled; see **Known behaviours**.

**Sleep starts 1 s after the command,** so the reply (and, over the mesh, the
`RTC … OK` answer) gets out first.

**Duty cycles:**

- The schedule is saved in NVS, so a deep-sleeping node picks it up again on
  every wake.
- The awake window must be at least 5 s, and a single sleep at most 7 days.
- `ALIGN` wakes on wall-clock multiples of awake + asleep. Every aligned node
  with the same schedule then wakes together, which is when to send it
  commands.
- A node only hears commands, `DUTY OFF` included, while it is awake. From
  the Pi, retry until one lands in the awake window.

**Accuracy.** On the internal RC oscillator, a 20 s light or deep sleep
measured within 1 s of the Pi's clock. Over hours, expect the ~5 % RC error,
so for long schedules either `TIME SYNC` now and then, or fit a DS3231:

```ini
; platformio.ini -- then rebuild. If no chip answers, the firmware logs a
; warning and falls back to the internal RTC.
    -DRTC_DS3231_SDA=21
    -DRTC_DS3231_SCL=22
    -DRTC_ALARM_GPIO=33   ; DS3231 INT/SQW; must be an RTC GPIO
```

With a DS3231 fitted:

- It is the master clock: it sets the system clock at boot and is written on
  every `TIME SET`.
- Its alarm wakes the board, with the sleep timer as a backstop.
- `valid=0` there means its oscillator stopped (for example, a flat backup
  battery) and the clock needs setting again.

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

### SPI on firmware older than 0.5.0

`ID?` reports `FW_VER`. Before 0.5.0 the SPI link has two faults:

- **Lines are cut at 63 chars.** Every `TELEM` line is longer than that, so
  over SPI it loses `rst` and `role` (and `rssi` once a battery is reported).
  Most `HELP` lines and long `ATTR`/`TEXT` lines are cut too.
- **Polling while a command runs desynchronises the slave.** The old firmware
  runs each command inside its SPI task and arms no transaction until it is
  done — about 60 ms for `ID?`, since every reply line is also logged to the
  console, and longer while UART2 is busy too. A frame clocked in that window
  arrives torn; its newline is lost, later commands pile up, and the link stays
  dead until the parser answers `ERR command too long`.

`meshctl.py` works around the second one: it waits 150 ms after each command
before polling, and sends a lone newline when it opens the port so a half-received
line cannot prefix the first command. That holds from 125 kHz to 4 MHz on its
own, but not while UART2 is running commands at the same time. Firmware 0.5.0
fixes both.

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
