#!/usr/bin/env python3
"""mesh_neighbors.py - Who hears whom: the mesh's radio topology.

Asks the gateway for its own neighbor table, then has it ask every provisioned
node for theirs (one NEIGHBORS ALL sweep), and prints the result as a list of
links and an RSSI matrix.

    python3 mesh_neighbors.py                  # gateway + every node, table
    python3 mesh_neighbors.py --json           # machine-readable
    python3 mesh_neighbors.py --addr 0x0005    # one node's table only
    python3 mesh_neighbors.py --watch 30       # repeat every 30 seconds
    python3 mesh_neighbors.py --uart           # over UART2 instead of USB

A neighbor is a board heard directly -- one radio hop, not relayed. Each board
learns them from a small HELLO every board sends every ~10 s with TTL 0, so a
freshly booted network needs about 15 s before the tables fill in. Entries not
heard for 60 s drop out.

Requires the gateway board on the other end of the link; the script promotes it
to GATEWAY if it is currently a plain NODE.
"""

import argparse
import datetime
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from meshctl import MeshLink  # noqa: E402


def collect(link, addr=None):
    """One pass. Returns (tables, missing, gateway_addr).

    `tables` maps each reporter's address to its list of neighbor entries.
    """
    me = link.local() or {}
    gateway_addr = me.get("addr")

    if addr is not None:
        got = link.neighbors(addr)
        return ({addr: got} if got is not None else {}), \
               ([] if got is not None else [addr]), gateway_addr

    tables, missing = link.neighbors_all()
    own = link.neighbors()
    if own is not None and gateway_addr is not None:
        tables[gateway_addr] = own
    return tables, missing, gateway_addr


def build_links(tables):
    """Fold per-board tables into undirected links.

    Each link records the average RSSI heard at each end. An end that has no
    entry for the other is either deaf to it (it answered, but does not list
    the peer) or unknown (it was not asked, or did not answer).
    """
    heard = {}
    for reporter, entries in tables.items():
        for e in entries:
            heard[(reporter, e["addr"])] = e["rssi_avg"]

    links = {}
    for (a, b) in heard:
        key = (min(a, b), max(a, b))
        if key in links:
            continue
        lo, hi = key
        links[key] = {
            "a": lo,
            "b": hi,
            "a_hears_b": heard.get((lo, hi)),
            "b_hears_a": heard.get((hi, lo)),
            "a_reported": lo in tables,
            "b_reported": hi in tables,
        }
    for link in links.values():
        both = link["a_hears_b"] is not None and link["b_hears_a"] is not None
        link["one_way"] = (not both) and link["a_reported"] and link["b_reported"]
    return sorted(links.values(), key=lambda l: (l["a"], l["b"]))


def label(addr, names, gateway_addr):
    text = "0x%04x" % addr
    name = names.get(addr)
    if name:
        text += " " + name
    if addr == gateway_addr:
        text += " (gw)"
    return text


def render(tables, missing, links, names, gateway_addr, out=sys.stdout):
    if not tables and not missing:
        out.write("no nodes provisioned yet -- power on a node and wait for "
                  "JOINED/READY, then re-run\n")
        return

    def end(rssi, reported):
        if rssi is not None:
            return "%d" % rssi
        return "deaf" if reported else "?"

    out.write("links (average RSSI in dBm, as heard at each end)\n")
    if not links:
        out.write("  none yet -- tables fill ~15 s after boot\n")
    rows = []
    for l in links:
        note = "one-way" if l["one_way"] else ""
        rows.append((label(l["a"], names, gateway_addr), "<->",
                     label(l["b"], names, gateway_addr),
                     end(l["a_hears_b"], l["a_reported"]),
                     end(l["b_hears_a"], l["b_reported"]), note))
    if rows:
        widths = [max(len(r[i]) for r in rows) for i in range(len(rows[0]))]
        for r in rows:
            out.write("  " + "  ".join(c.ljust(w) for c, w in zip(r, widths)).rstrip()
                      + "\n")

    # Matrix: row = the board listening, column = the board it hears.
    addrs = sorted(set(tables) | {e["addr"] for t in tables.values() for e in t})
    if addrs:
        out.write("\nRSSI matrix (row hears column; . = not heard)\n")
        cols = ["0x%04x" % a for a in addrs]
        w = max(6, max(len(c) for c in cols))
        out.write("  %-8s" % "" + " ".join(c.rjust(w) for c in cols) + "\n")
        for r in addrs:
            if r not in tables:
                continue
            seen = {e["addr"]: e["rssi_avg"] for e in tables[r]}
            cells = []
            for c in addrs:
                if c == r:
                    cells.append("-".rjust(w))
                elif c in seen:
                    cells.append(str(seen[c]).rjust(w))
                else:
                    cells.append(".".rjust(w))
            out.write("  %-8s" % ("0x%04x" % r) + " ".join(cells) + "\n")

    if missing:
        out.write("\nno reply from: %s\n"
                  % ", ".join(label(a, names, gateway_addr) for a in missing))


def main():
    ap = argparse.ArgumentParser(
        description="Map which BLE mesh nodes hear each other directly, "
                    "from a Raspberry Pi.")
    link = ap.add_mutually_exclusive_group()
    link.add_argument("--usb", nargs="?", const="", metavar="PORT",
                      help="board USB serial (default: autodetect)")
    link.add_argument("--uart", nargs="?", const="", metavar="PORT",
                      help="ESP32 UART2 on the Pi header pins")
    link.add_argument("--spi", nargs="?", const="0.0", metavar="BUS.CS",
                      help="SPI slave link")
    ap.add_argument("--addr", help="ask one node (e.g. 0x0005) instead of all")
    ap.add_argument("--json", action="store_true", help="emit JSON")
    ap.add_argument("--watch", type=float, metavar="SECONDS",
                    help="repeat on this interval until interrupted")
    ap.add_argument("--no-names", action="store_true",
                    help="skip the extra ASK round trip that fetches NAME")
    args = ap.parse_args()

    addr = None
    if args.addr:
        try:
            addr = int(args.addr, 0)
        except ValueError:
            print("bad --addr %r (try 0x0005)" % args.addr, file=sys.stderr)
            return 2

    kwargs = {}
    if args.spi is not None:
        kwargs["spi"] = args.spi
    elif args.uart is not None:
        kwargs["uart"] = args.uart or True
    else:
        kwargs["usb"] = args.usb or True

    with MeshLink(**kwargs) as m:
        if not args.json:
            print("link: %s" % m.name, file=sys.stderr)

        if not m.ensure_gateway():
            print("could not put the board into GATEWAY mode; only a gateway "
                  "can query other nodes", file=sys.stderr)
            return 1

        names = {}
        while True:
            try:
                tables, missing, gateway_addr = collect(m, addr)
                if not args.no_names:
                    todo = [a for a in tables if a not in names and a != gateway_addr]
                    for a, v in m.ask_sweep(todo, attrs=("NAME",)).items():
                        if v.get("NAME"):
                            names[a] = v["NAME"]
            except RuntimeError as e:
                print("error: %s" % e, file=sys.stderr)
                return 1

            links = build_links(tables)
            if args.json:
                print(json.dumps({
                    "gateway": None if gateway_addr is None else "0x%04x" % gateway_addr,
                    "neighbors": {"0x%04x" % k: v for k, v in sorted(tables.items())},
                    "links": links,
                    "no_reply": ["0x%04x" % a for a in missing],
                    "names": {"0x%04x" % k: v for k, v in names.items()},
                }, indent=2))
            else:
                if args.watch:
                    print("\n%s" % datetime.datetime.now().strftime("%H:%M:%S"))
                render(tables, missing, links, names, gateway_addr)

            if not args.watch:
                return 0
            try:
                time.sleep(args.watch)
            except KeyboardInterrupt:
                print()
                return 0


if __name__ == "__main__":
    sys.exit(main())
