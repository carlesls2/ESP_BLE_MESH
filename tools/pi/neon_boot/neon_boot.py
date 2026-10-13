#!/usr/bin/env python3
"""Record each boot of this machine as a row in a Neon Postgres table.

Started once per boot by neon-boot.service. It retries until the network and
the database answer, inserts one row, and exits. Rows are keyed by the
kernel's boot_id, so running it again during the same boot adds nothing.

The connection string is read from ~/.config/neon-boot/neon.env:
    DATABASE_URL=postgresql://user:password@ep-....neon.tech/neondb?sslmode=require
"""

import ipaddress
import platform
import socket
import sys
import time
import uuid
from pathlib import Path

import psycopg

ENV_FILE = Path.home() / ".config/neon-boot/neon.env"
GIVE_UP_AFTER_S = 3600
MAX_BACKOFF_S = 60

SCHEMA = """
CREATE TABLE IF NOT EXISTS rpi_boots (
    id        bigserial   PRIMARY KEY,
    boot_id   uuid        NOT NULL UNIQUE,
    hostname  text        NOT NULL,
    booted_at timestamptz NOT NULL,
    logged_at timestamptz NOT NULL DEFAULT now(),
    lan_ip    inet,
    kernel    text,
    os        text
)
"""

# booted_at is the server clock minus our uptime, so it is right even when
# the Pi's own clock has not been set yet.
INSERT = """
INSERT INTO rpi_boots (boot_id, hostname, booted_at, lan_ip, kernel, os)
VALUES (%(boot_id)s, %(hostname)s, now() - make_interval(secs => %(uptime)s),
        %(lan_ip)s, %(kernel)s, %(os)s)
ON CONFLICT (boot_id) DO NOTHING
RETURNING id, booted_at
"""


def read_database_url():
    for line in ENV_FILE.read_text().splitlines():
        key, sep, value = line.strip().partition("=")
        if key == "DATABASE_URL" and sep:
            return value.strip().strip("'\"")
    raise KeyError("DATABASE_URL")


def lan_ip():
    """Address of the interface that routes to the internet, or None."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("1.1.1.1", 53))  # a UDP connect sends no packets
            return ipaddress.ip_address(s.getsockname()[0])
    except OSError:
        return None


def boot_facts():
    boot_id = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
    return {
        "boot_id": uuid.UUID(boot_id),
        "hostname": socket.gethostname(),
        "lan_ip": lan_ip(),
        "kernel": platform.release(),
        "os": platform.freedesktop_os_release().get("PRETTY_NAME"),
        "uptime": float(Path("/proc/uptime").read_text().split()[0]),
    }


def record(url):
    with psycopg.connect(url, connect_timeout=10,
                         application_name="neon-boot") as conn:
        conn.execute(SCHEMA)
        return conn.execute(INSERT, boot_facts()).fetchone()


def main():
    try:
        url = read_database_url()
    except (OSError, KeyError) as e:
        print(f"no DATABASE_URL in {ENV_FILE}: {e}", file=sys.stderr)
        return 2

    deadline = time.monotonic() + GIVE_UP_AFTER_S
    delay = 2
    while True:
        try:
            row = record(url)
            break
        except psycopg.OperationalError as e:
            reason = str(e).splitlines()[0]
            if time.monotonic() + delay > deadline:
                print(f"giving up after {GIVE_UP_AFTER_S}s: {reason}",
                      file=sys.stderr)
                return 1
            print(f"database not reachable, retrying in {delay}s: {reason}")
            time.sleep(delay)
            delay = min(delay * 2, MAX_BACKOFF_S)

    if row:
        booted = row[1].astimezone()  # Neon answers in UTC; log local time
        print(f"logged boot as row {row[0]}, booted_at {booted:%Y-%m-%d %H:%M:%S %Z}")
    else:
        print("this boot was already logged")
    return 0


if __name__ == "__main__":
    sys.exit(main())
