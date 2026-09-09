#!/usr/bin/env python3
"""meshctl.py - Raspberry Pi client for the ESP32 mesh-node command channel.

Talks the same line protocol on three links:
  USB:  the board's own USB-serial bridge (CP210x -> /dev/ttyUSB0), 115200 8N1.
        This is UART0, which is also the ESP-IDF console, so log output shares
        the line; it is filtered out here (see LOG_LINE_RE).
  UART: ESP32 UART2 (TX GPIO17 -> Pi RXD, RX GPIO16 <- Pi TXD), 115200 8N1.
  SPI:  ESP32 SPI slave on VSPI (MOSI 23, MISO 19, SCLK 18, CS 5), mode 0,
        fixed 64-byte frames; a reply frame starting with 0x00 is empty.

One-shot:     python3 meshctl.py --usb MODE?
              python3 meshctl.py --spi SEND ALL hello
Interactive:  python3 meshctl.py --usb      (type HELP for the command menu)

As a library, for scripts that want answers rather than a line stream:

    from meshctl import MeshLink
    with MeshLink(usb=True) as m:
        m.ensure_gateway()
        nodes = m.nodes()
        telem, missing = m.telem("ALL")

Exit code is nonzero when any reply line starts with "ERR ".
"""

import argparse
import glob
import json
import os
import re
import sys
import threading
import time

SPI_FRAME_LEN = 64
ASYNC_PREFIXES = ("JOINED", "READY", "MSG", "ATTR", "ATTRSET", "TEXT",
                  "NODE", "NODES", "TELEM")

# Reply collection: a command's reply is done after this much silence.
QUIET_WINDOW_S = 0.3
REPLY_CAP_S = 3.0

# A group-addressed query staggers replies by up to 500 ms in the firmware
# (STAGGER_MAX_MS in mesh_attr_model.c), and a segmented reply takes longer
# still, so a broadcast sweep needs a window well past that.
BROADCAST_WINDOW_S = 3.0

# ESP-IDF log lines: "I (12345) TAG: text", optionally ANSI-coloured. They share
# UART0 with the protocol, so they are dropped rather than parsed. This is a
# heuristic by design -- see tools/pi/README.md for when it can misfire.
ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
LOG_LINE_RE = re.compile(r"^[IWEDV] \(\d+\) [\w.\-/ ]{1,40}: ")


def _clean_line(raw):
    """Decode a received line, or None if it is empty, garbage, or log noise."""
    try:
        text = raw.decode("ascii", errors="strict").strip()
    except UnicodeDecodeError:
        return None
    text = ANSI_RE.sub("", text).strip()
    if not text or not text.isprintable():
        return None
    if LOG_LINE_RE.match(text):
        return None
    return text


class _SerialTransport:
    """Shared line handling for the two UART-shaped links."""

    def __init__(self, port, baud):
        import serial  # pyserial

        # Opening a CP210x asserts DTR/RTS by default, which on most ESP32
        # boards is wired to EN/BOOT and reboots (or bootloader-straps) the
        # chip. Holding them off keeps the running firmware running.
        self.ser = serial.Serial(baudrate=baud, timeout=0.15)
        self.ser.port = port
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.name = port
        self._rxbuf = b""

    def send(self, cmd):
        self.ser.write(cmd.encode("ascii") + b"\n")

    def read_lines(self, max_polls=1):
        """Drain whatever is buffered, return complete lines."""
        data = self.ser.read(4096)
        if data:
            self._rxbuf += data
        lines = []
        while b"\n" in self._rxbuf:
            raw, self._rxbuf = self._rxbuf.split(b"\n", 1)
            line = _clean_line(raw)
            if line is not None:
                lines.append(line)
        return lines

    def command(self, cmd):
        """Send one command and collect its reply lines (quiet-window based)."""
        self.read_lines()  # discard stale input
        self.send(cmd)
        replies = []
        deadline = time.monotonic() + REPLY_CAP_S
        last_rx = time.monotonic()
        while time.monotonic() < deadline:
            lines = self.read_lines()
            if lines:
                replies.extend(lines)
                last_rx = time.monotonic()
            elif time.monotonic() - last_rx > QUIET_WINDOW_S:
                break
        return replies

    def close(self):
        self.ser.close()


def find_usb_port():
    """Pick the board's USB-serial device, preferring the stable by-id path."""
    for pattern in ("/dev/serial/by-id/*CP210*",
                    "/dev/serial/by-id/*Silicon_Labs*",
                    "/dev/serial/by-id/*CH340*",
                    "/dev/serial/by-id/*usb*"):
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[0]
    matches = sorted(glob.glob("/dev/ttyUSB*")) + sorted(glob.glob("/dev/ttyACM*"))
    if matches:
        return matches[0]
    return None


class UsbTransport(_SerialTransport):
    """UART0 over the board's USB bridge. Shares the line with ESP_LOG."""

    def __init__(self, port=None, baud=115200):
        if not port:
            port = find_usb_port()
            if port is None:
                raise SystemExit(
                    "no USB serial device found (looked in /dev/serial/by-id "
                    "and /dev/ttyUSB*). Is the ESP32 plugged in, and are you "
                    "in the 'dialout' group?")
        super().__init__(port, baud)


class UartTransport(_SerialTransport):
    """ESP32 UART2 on the Pi's header pins. No log noise on this one."""

    def __init__(self, port=None, baud=115200):
        if port is None:
            port = next((p for p in ("/dev/ttyAMA0", "/dev/serial0")
                         if os.path.exists(p)), "/dev/ttyAMA0")
        super().__init__(port, baud)


class SpiTransport:
    # Conservative clock: the ESP32 SPI slave routes these pins through the
    # GPIO matrix, which limits usable speed well below the Pi's maximum.
    def __init__(self, dev="0.0", speed=1_000_000):
        import spidev

        bus, _, cs = dev.partition(".")
        self.spi = spidev.SpiDev()
        self.spi.open(int(bus), int(cs or 0))
        self.spi.mode = 0
        self.spi.max_speed_hz = speed
        self.name = "/dev/spidev%d.%s" % (int(bus), cs or 0)

    def _xfer(self, payload=b""):
        """Clock one fixed-size frame; return the received line or None."""
        frame = list(payload.ljust(SPI_FRAME_LEN, b"\x00")[:SPI_FRAME_LEN])
        rx = bytes(self.spi.xfer2(frame))
        # Give the slave time to re-queue its next transaction; clocking into
        # that gap loses bytes on both directions.
        time.sleep(0.002)
        if rx[0] == 0:
            return None
        return _clean_line(rx.split(b"\x00", 1)[0])

    def send(self, cmd):
        payload = cmd.encode("ascii") + b"\n"
        if len(payload) > SPI_FRAME_LEN - 1:
            raise ValueError("command too long for SPI (max %d chars)"
                             % (SPI_FRAME_LEN - 2))
        # A queued line (e.g. async bridge output) rides out on this same
        # transaction; don't lose it.
        return self._xfer(payload)

    def read_lines(self, max_polls=1):
        """Poll for queued lines with empty frames."""
        lines = []
        for _ in range(max_polls):
            line = self._xfer()
            if line is None:
                break
            lines.append(line)
        return lines

    def command(self, cmd):
        """Send one command, poll until the reply queue stays empty."""
        replies = []
        ride_along = self.send(cmd)
        if ride_along is not None:
            replies.append(ride_along)
        empties = 0
        deadline = time.monotonic() + REPLY_CAP_S
        while empties < 3 and time.monotonic() < deadline:
            line = self._xfer()
            if line is None:
                empties += 1
                time.sleep(0.02)
            else:
                replies.append(line)
                empties = 0
        return replies

    def close(self):
        self.spi.close()


# --- Reply parsing --------------------------------------------------------

def _kv(text):
    """Parse trailing "k=v k=v" pairs into a dict."""
    out = {}
    for token in text.split():
        if "=" in token:
            k, _, v = token.partition("=")
            out[k] = v
    return out


def parse_node_line(line):
    """NODE idx=0 unicast=0xc005 elems=3 uuid=dddd2c3ae8109f4c"""
    if not line.startswith("NODE "):
        return None
    f = _kv(line)
    if "unicast" not in f:
        return None
    try:
        return {
            "addr": int(f["unicast"], 16),
            "idx": int(f.get("idx", "0")),
            "elems": int(f.get("elems", "0")),
            "uuid": f.get("uuid", ""),
        }
    except ValueError:
        return None


def parse_telem_line(line):
    """TELEM 0xc005 up=3821 heap=142314 min=131002 batt=4012mV/87% rssi=-63
    rst=POWERON role=NODE"""
    parts = line.split(None, 2)
    if len(parts) < 3 or parts[0] != "TELEM":
        return None
    try:
        addr = int(parts[1], 16)
    except ValueError:
        return None

    f = _kv(parts[2])

    # "TELEM 0x0005 sent" is the command acknowledgement, not a node's answer,
    # and it shares the prefix and the address with the real thing. Without
    # this it parses into a record whose every field is empty, and a node that
    # never replied looks like one reporting nothing.
    if "up" not in f:
        return None

    def num(key, cast=int):
        try:
            return cast(f[key])
        except (KeyError, ValueError):
            return None

    batt_mv = batt_pct = None
    raw_batt = f.get("batt", "-")
    if raw_batt != "-":
        m = re.match(r"^(\d+)mV/(\d+)%$", raw_batt)
        if m:
            batt_mv, batt_pct = int(m.group(1)), int(m.group(2))

    rssi = None
    if f.get("rssi", "-") != "-":
        try:
            rssi = int(f["rssi"])
        except ValueError:
            pass

    return {
        "addr": addr,
        "uptime_s": num("up"),
        "heap": num("heap"),
        "heap_min": num("min"),
        "batt_mv": batt_mv,
        "batt_pct": batt_pct,
        "rssi": rssi,
        "reset": f.get("rst"),
        "role": f.get("role"),
    }


# "ID <ATTR> <value>" maps onto the same record shape parse_telem_line
# produces, so the attached board can sit in the same table as remote nodes.
ID_FIELD_MAP = {
    "UNICAST":  ("addr", lambda v: int(v, 16)),
    "NAME":     ("name", str),
    "UPTIME":   ("uptime_s", int),
    "HEAP":     ("heap", int),
    "HEAP_MIN": ("heap_min", int),
    "BATT_MV":  ("batt_mv", int),
    "BATT_PCT": ("batt_pct", int),
    "RSSI":     ("rssi", int),
    "RESET":    ("reset", str),
    "ROLE":     ("role", str),
}

TELEM_RECORD_KEYS = ("addr", "name", "uptime_s", "heap", "heap_min",
                     "batt_mv", "batt_pct", "rssi", "reset", "role")


def parse_id_lines(lines):
    """Fold the reply to ID? into one telem-shaped record, or None."""
    rec = dict.fromkeys(TELEM_RECORD_KEYS)
    seen = False

    for line in lines:
        parts = line.split(None, 2)
        if len(parts) < 3 or parts[0] != "ID":
            continue
        attr, raw = parts[1].upper(), parts[2].strip()
        if attr not in ID_FIELD_MAP:
            continue
        seen = True
        # "-" is a dynamic attribute the board cannot supply; "<unreadable>" a
        # stored one that is malformed. Both mean "leave the field empty".
        if raw in ("-", "<unreadable>"):
            continue
        key, cast = ID_FIELD_MAP[attr]
        try:
            rec[key] = cast(raw)
        except (ValueError, TypeError):
            pass

    return rec if seen else None


# --- High-level link ------------------------------------------------------

class MeshLink:
    """A transport plus request/response correlation.

    The firmware answers a query with an immediate "sent" acknowledgement and
    delivers the actual results later as unsolicited bridge lines. The gather
    helpers below wait for those and key them by source address, so callers get
    a result set instead of having to reassemble a line stream themselves.
    """

    def __init__(self, transport=None, usb=None, uart=None, spi=None,
                 baud=115200, speed=1_000_000):
        if transport is not None:
            self.tr = transport
        elif spi is not None:
            self.tr = SpiTransport(spi if spi is not True else "0.0", speed)
        elif uart is not None:
            self.tr = UartTransport(None if uart is True else uart, baud)
        else:
            self.tr = UsbTransport(None if usb in (True, None) else usb, baud)

    # context manager so scripts cannot leak the port
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def close(self):
        self.tr.close()

    @property
    def name(self):
        return self.tr.name

    def command(self, cmd):
        return self.tr.command(cmd)

    def _gather(self, cmd, prefix, timeout, done=None):
        """Send `cmd`, then collect lines starting with `prefix` until `done`
        says the set is complete or `timeout` expires.

        Returns (matched, others). `others` carries ERR lines and anything
        unrelated that arrived meanwhile, so nothing is silently swallowed.
        """
        self.tr.read_lines(max_polls=4)  # drop stale input
        self.tr.send(cmd)

        matched, others = [], []
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            lines = self.tr.read_lines(max_polls=8)
            if not lines:
                time.sleep(0.05)
                continue
            finished = False
            for line in lines:
                # Record first, then test. The line that completes the set is
                # usually part of it -- the single answer to a unicast query --
                # and testing first dropped exactly the reply we were waiting
                # for, making every node look silent.
                (matched if line.startswith(prefix) else others).append(line)
                if done is not None and done(line):
                    finished = True
            if finished:
                break
        return matched, others

    def mode(self):
        for line in self.command("MODE?"):
            if line.startswith("MODE "):
                return line.split(None, 1)[1].strip()
        return None

    def ensure_gateway(self):
        """Promote to GATEWAY if not already. Returns True if the role is now
        GATEWAY. Only a gateway can query other nodes."""
        if self.mode() == "GATEWAY":
            return True
        self.command("MODE GATEWAY")
        return self.mode() == "GATEWAY"

    def nodes(self, timeout=3.0):
        """Every node this gateway has provisioned, from its persistent table.

        The firmware terminates the list with "NODES count=N", so this returns
        as soon as the list is complete rather than waiting out the timeout.
        """
        lines, others = self._gather(
            "NODES", "NODE ", timeout,
            done=lambda l: l.startswith("NODES count="))
        found = [n for n in (parse_node_line(l) for l in lines) if n]
        errors = [l for l in others if l.startswith("ERR")]
        if errors and not found:
            raise RuntimeError("; ".join(errors))
        return found

    def telem(self, target="ALL", timeout=None, expect=None):
        """Live vitals. `target` is "ALL", a group address, or a unicast int.

        Returns (by_addr, missing): a dict keyed by unicast address, and the
        subset of `expect` that did not answer.
        """
        tgt = target if isinstance(target, str) else "0x%04x" % target
        if timeout is None:
            timeout = BROADCAST_WINDOW_S if tgt.upper() == "ALL" else 2.0

        want = set(expect or [])

        def done(line):
            # A unicast query is finished as soon as its one answer lands.
            if want and line.startswith("TELEM "):
                got = parse_telem_line(line)
                if got:
                    want.discard(got["addr"])
                    return not want
            return False

        lines, others = self._gather("TELEM %s" % tgt, "TELEM ", timeout,
                                     done=done if expect else None)
        by_addr = {}
        for line in lines:
            rec = parse_telem_line(line)
            if rec:
                by_addr[rec["addr"]] = rec

        errors = [l for l in others if l.startswith("ERR")]
        if errors and not by_addr:
            raise RuntimeError("; ".join(errors))

        missing = [a for a in (expect or []) if a not in by_addr]
        return by_addr, missing

    def local(self):
        """The attached board's own state, read locally with ID?.

        This is the only way to get the gateway's vitals: TELEM addressed to
        its own unicast goes out over the mesh and nothing comes back through
        the client model, so a gateway genuinely cannot poll itself.
        """
        return parse_id_lines(self.command("ID?"))

    def telem_sweep(self, addrs, timeout=2.0):
        """Poll each address in turn with a unicast TELEM.

        Preferred over telem("ALL"). A group-addressed query does reach every
        node and every node does reply -- you can watch it happen in the node's
        log -- but ESP-BLE-MESH's client model only accepts a status that
        matches a pending transaction, and a group send arms none, so the
        replies are dropped before the application ever sees them. A unicast
        query arms that transaction, which is why this works.

        The sweep also attributes silence correctly: with a broadcast you learn
        only that somebody did not answer.
        """
        found, missing = {}, []
        for addr in addrs:
            got, _ = self.telem(addr, timeout=timeout, expect=[addr])
            if addr in got:
                found[addr] = got[addr]
            else:
                missing.append(addr)
        return found, missing

    def ask_sweep(self, addrs, attrs=(), timeout=2.0):
        """Unicast ASK per address, for the same reason as telem_sweep."""
        found = {}
        for addr in addrs:
            try:
                got = self.ask(addr, attrs=attrs, timeout=timeout)
            except RuntimeError:
                continue
            if addr in got:
                found[addr] = got[addr]
        return found

    def ask(self, target="ALL", attrs=(), timeout=None):
        """Stored identity attributes, as ATTR lines keyed by address."""
        tgt = target if isinstance(target, str) else "0x%04x" % target
        if timeout is None:
            timeout = BROADCAST_WINDOW_S if tgt.upper() == "ALL" else 2.0

        cmd = "ASK %s%s" % (tgt, (" " + " ".join(attrs)) if attrs else "")
        want_addr = None if tgt.upper() == "ALL" else int(tgt, 0)

        def done(line):
            if want_addr is None or not line.startswith("ATTR "):
                return False
            parts = line.split(None, 2)
            try:
                return len(parts) >= 2 and int(parts[1], 16) == want_addr
            except ValueError:
                return False

        lines, others = self._gather(cmd, "ATTR ", timeout,
                                     done=done if want_addr is not None else None)

        by_addr = {}
        for line in lines:
            parts = line.split(None, 2)
            if len(parts) < 2:
                continue
            try:
                addr = int(parts[1], 16)
            except ValueError:
                continue
            by_addr[addr] = _kv(parts[2]) if len(parts) > 2 else {}

        errors = [l for l in others if l.startswith("ERR")]
        if errors and not by_addr:
            raise RuntimeError("; ".join(errors))
        return by_addr


# --- CLI ------------------------------------------------------------------

def run_oneshot(tr, cmd):
    replies = tr.command(cmd)
    for line in replies:
        print(line)
    if not replies:
        print("(no reply)", file=sys.stderr)
        return 1
    return 1 if any(l.startswith("ERR ") or l == "ERR" for l in replies) else 0


def run_repl(tr, is_spi):
    print("connected to %s -- type HELP for commands, Ctrl-D/exit to quit"
          % tr.name)
    stop = threading.Event()
    # Async bridge lines (JOINED/READY/MSG/ATTR/ATTRSET/TEXT/NODE/TELEM) arrive
    # at any time; a background poller prints them between commands. Commands
    # pause it so replies stay attached to their prompt.
    idle = threading.Event()
    idle.set()

    def poller():
        while not stop.is_set():
            if idle.is_set():
                for line in tr.read_lines():
                    print(line)
            time.sleep(0.1)

    t = threading.Thread(target=poller, daemon=True)
    t.start()

    try:
        while True:
            try:
                cmd = input("> ").strip()
            except EOFError:
                break
            if not cmd:
                continue
            if cmd.lower() in ("exit", "quit"):
                break
            idle.clear()
            try:
                for line in tr.command(cmd):
                    print(line)
            except ValueError as e:
                print("ERR %s" % e)
            finally:
                idle.set()
    except KeyboardInterrupt:
        print()
    finally:
        stop.set()
        t.join(timeout=1)
    return 0


def open_transport(args):
    if args.spi is not None:
        return SpiTransport(args.spi, args.speed)
    if args.uart is not None:
        return UartTransport(args.uart or None, args.baud)
    return UsbTransport(args.usb or None, args.baud)


def main():
    ap = argparse.ArgumentParser(
        description="Send mesh-node host commands from a Raspberry Pi over "
                    "USB, UART or SPI.")
    link = ap.add_mutually_exclusive_group()
    link.add_argument("--usb", nargs="?", const="", metavar="PORT",
                      help="use the board's USB serial (default: autodetect)")
    link.add_argument("--uart", nargs="?", const="", metavar="PORT",
                      help="use UART2 (default /dev/ttyAMA0, else /dev/serial0)")
    link.add_argument("--spi", nargs="?", const="0.0", metavar="BUS.CS",
                      help="use SPI (default 0.0 -> /dev/spidev0.0)")
    ap.add_argument("--baud", type=int, default=115200, help="serial baud rate")
    ap.add_argument("--speed", type=int, default=1_000_000,
                    help="SPI clock in Hz")
    ap.add_argument("--json", action="store_true",
                    help="for NODES and TELEM, print parsed JSON instead of raw lines")
    ap.add_argument("command", nargs=argparse.REMAINDER,
                    help="command to send (omit for interactive mode)")
    args = ap.parse_args()

    # Default to USB when no link is named: it needs no wiring or config.txt
    # edits, which makes it the right first thing to try.
    if args.usb is None and args.uart is None and args.spi is None:
        args.usb = ""

    tr = open_transport(args)

    try:
        if not args.command:
            return run_repl(tr, args.spi is not None)

        cmd = " ".join(args.command)
        if args.json:
            link = MeshLink(transport=tr)
            verb = args.command[0].upper()
            if verb == "NODES":
                print(json.dumps(link.nodes(), indent=2))
                return 0
            if verb == "TELEM":
                target = args.command[1] if len(args.command) > 1 else "ALL"
                found, _ = link.telem(target)
                print(json.dumps(list(found.values()), indent=2))
                return 0
            print("--json only applies to NODES and TELEM", file=sys.stderr)
            return 2

        return run_oneshot(tr, cmd)
    finally:
        tr.close()


if __name__ == "__main__":
    sys.exit(main())
