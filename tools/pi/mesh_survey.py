#!/usr/bin/env python3
"""mesh_survey.py - Ask every node in the mesh how it is doing.

Discovers which nodes the gateway has provisioned, polls each for live vitals,
and prints them as a table, JSON, or CSV.

    python3 mesh_survey.py                  # discover + poll once, table
    python3 mesh_survey.py --json           # machine-readable
    python3 mesh_survey.py --watch 10       # re-poll every 10 seconds
    python3 mesh_survey.py --csv log.csv    # append timestamped rows
    python3 mesh_survey.py --addr 0xC005    # just one node
    python3 mesh_survey.py --no-gateway     # remote nodes only
    python3 mesh_survey.py --uart           # over UART2 instead of USB

A node that is in the gateway's table but does not answer is still listed, with
"-" in every column and marked stale. Silence is a finding, not a reason to
hide the row.

Requires the gateway board on the other end of the link; the script promotes it
to GATEWAY if it is currently a plain NODE.
"""

import argparse
import csv
import datetime
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from meshctl import MeshLink  # noqa: E402

COLUMNS = ("addr", "name", "uptime", "heap", "battery", "rssi", "reset", "role")


def fmt_uptime(seconds):
    if seconds is None:
        return "-"
    d, rem = divmod(int(seconds), 86400)
    h, rem = divmod(rem, 3600)
    m, s = divmod(rem, 60)
    if d:
        return "%dd %02dh" % (d, h)
    if h:
        return "%dh %02dm" % (h, m)
    if m:
        return "%dm %02ds" % (m, s)
    return "%ds" % s


def fmt_heap(n):
    if n is None:
        return "-"
    if n >= 1024 * 1024:
        return "%.1fM" % (n / (1024.0 * 1024.0))
    if n >= 1024:
        return "%.1fK" % (n / 1024.0)
    return str(n)


def fmt_batt(mv, pct):
    if mv is None:
        return "-"
    return "%.2fV %d%%" % (mv / 1000.0, pct if pct is not None else 0)


def _row(addr, rec, name=None, uuid="", elems=None, gateway=False):
    """One table row. `rec` is a telem-shaped record, or None for no reply."""
    return {
        "addr": "0x%04x" % addr,
        "addr_int": addr,
        "name": name or (rec or {}).get("name") or "-",
        "uuid": uuid,
        "elems": elems,
        "gateway": gateway,
        "stale": rec is None,
        "uptime_s": (rec or {}).get("uptime_s"),
        "heap": (rec or {}).get("heap"),
        "heap_min": (rec or {}).get("heap_min"),
        "batt_mv": (rec or {}).get("batt_mv"),
        "batt_pct": (rec or {}).get("batt_pct"),
        "rssi": (rec or {}).get("rssi"),
        "reset": (rec or {}).get("reset"),
        "role": (rec or {}).get("role"),
    }


def collect(link, addr=None, names=True, broadcast=False, name_cache=None,
            gateway=True):
    """One survey pass. Returns (rows, missing), one row per known node.

    The gateway is included by default, read locally with ID? rather than
    polled over the mesh -- it cannot answer its own TELEM.
    """
    if addr is None:
        nodes = link.nodes()
        expect = [n["addr"] for n in nodes]
    else:
        nodes = [{"addr": addr, "elems": None, "uuid": ""}]
        expect = [addr]

    if broadcast:
        # Opt-in, and measured to be lossy: the nodes do receive a group query
        # and do reply, but the gateway's client model drops a status with no
        # matching transaction. Kept because it is one packet instead of N.
        telem, missing = link.telem("ALL", expect=expect)
    else:
        telem, missing = link.telem_sweep(expect)

    # NAME is stored identity, not telemetry, so it needs its own ASK. Cached
    # across --watch passes: a name changes about never, and skipping it halves
    # the round trips on a steady-state poll.
    labels = dict(name_cache) if name_cache else {}
    if names:
        todo = [a for a in expect if a not in labels]
        if todo:
            for a, v in link.ask_sweep(todo, attrs=("NAME",)).items():
                labels[a] = v.get("NAME", "")

    rows = [_row(n["addr"], telem.get(n["addr"]), name=labels.get(n["addr"]),
                 uuid=n.get("uuid", ""), elems=n.get("elems"))
            for n in nodes]

    if gateway and addr is None:
        me = link.local()
        if me and me.get("addr") is not None:
            rows.append(_row(me["addr"], me, gateway=True))

    rows.sort(key=lambda r: r["addr_int"])
    return rows, missing


def render_table(rows, out=sys.stdout):
    if not rows:
        out.write("no nodes provisioned yet -- power on a node and wait for "
                  "JOINED/READY, then re-run\n")
        return

    table = []
    for r in rows:
        table.append((
            r["addr"],
            r["name"],
            fmt_uptime(r["uptime_s"]),
            fmt_heap(r["heap"]),
            fmt_batt(r["batt_mv"], r["batt_pct"]),
            "-" if r["rssi"] is None else str(r["rssi"]),
            r["reset"] or "-",
            (r["role"] or "-")
            + (" (this board)" if r.get("gateway") else "")
            + (" (no reply)" if r["stale"] else ""),
        ))

    widths = [max(len(str(row[i])) for row in [COLUMNS] + table)
              for i in range(len(COLUMNS))]
    fmt = "  ".join("%%-%ds" % w for w in widths)
    out.write(fmt % COLUMNS + "\n")
    out.write("  ".join("-" * w for w in widths) + "\n")
    for row in table:
        out.write(fmt % row + "\n")


def append_csv(path, rows):
    new = not os.path.exists(path)
    stamp = datetime.datetime.now().isoformat(timespec="seconds")
    fields = ["timestamp", "addr", "name", "uptime_s", "heap", "heap_min",
              "batt_mv", "batt_pct", "rssi", "reset", "role", "gateway", "stale"]
    with open(path, "a", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=fields, extrasaction="ignore")
        if new:
            w.writeheader()
        for r in rows:
            row = dict(r)
            row["timestamp"] = stamp
            w.writerow(row)


def main():
    ap = argparse.ArgumentParser(
        description="Survey every BLE mesh node's live vitals from a "
                    "Raspberry Pi.")
    link = ap.add_mutually_exclusive_group()
    link.add_argument("--usb", nargs="?", const="", metavar="PORT",
                      help="board USB serial (default: autodetect)")
    link.add_argument("--uart", nargs="?", const="", metavar="PORT",
                      help="ESP32 UART2 on the Pi header pins")
    link.add_argument("--spi", nargs="?", const="0.0", metavar="BUS.CS",
                      help="SPI slave link")
    ap.add_argument("--addr", help="survey one node (e.g. 0xC005) instead of all")
    ap.add_argument("--json", action="store_true", help="emit JSON")
    ap.add_argument("--csv", metavar="FILE", help="append timestamped rows here")
    ap.add_argument("--watch", type=float, metavar="SECONDS",
                    help="re-poll on this interval until interrupted")
    ap.add_argument("--no-names", action="store_true",
                    help="skip the extra ASK round trip that fetches NAME")
    ap.add_argument("--no-gateway", action="store_true",
                    help="list only remote nodes, omitting the attached board")
    ap.add_argument("--broadcast", action="store_true",
                    help="one group query instead of a unicast sweep (faster, "
                         "but replies are unreliably delivered -- see README)")
    args = ap.parse_args()

    addr = None
    if args.addr:
        try:
            addr = int(args.addr, 0)
        except ValueError:
            print("bad --addr %r (try 0xC005)" % args.addr, file=sys.stderr)
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

        name_cache = {}
        while True:
            try:
                rows, missing = collect(m, addr, names=not args.no_names,
                                        broadcast=args.broadcast,
                                        name_cache=name_cache,
                                        gateway=not args.no_gateway)
                for r in rows:
                    if r["name"] not in ("", "-"):
                        name_cache[r["addr_int"]] = r["name"]
            except RuntimeError as e:
                print("error: %s" % e, file=sys.stderr)
                return 1

            if args.json:
                print(json.dumps({"nodes": rows, "no_reply": missing}, indent=2))
            else:
                if args.watch:
                    print("\n%s" % datetime.datetime.now().strftime("%H:%M:%S"))
                render_table(rows)
                if missing:
                    print("\n%d of %d did not answer: %s"
                          % (len(missing), len(rows),
                             ", ".join("0x%04x" % a for a in missing)),
                          file=sys.stderr)

            if args.csv:
                append_csv(args.csv, rows)

            if not args.watch:
                return 0
            try:
                time.sleep(args.watch)
            except KeyboardInterrupt:
                print()
                return 0


if __name__ == "__main__":
    sys.exit(main())
