# neon-boot: log every Pi boot to Neon Postgres

A systemd user service that runs at each boot and inserts one row into the
`rpi_boots` table of a Neon database. It creates the table the first time.

| column      | value                                                        |
|-------------|--------------------------------------------------------------|
| `boot_id`   | the kernel's per-boot UUID, unique, so reruns add no row     |
| `hostname`  | `rpi`                                                        |
| `booted_at` | Neon's clock minus the Pi's uptime, so right even before NTP |
| `logged_at` | when the row reached Neon                                    |
| `lan_ip`    | the Pi's address on the LAN                                  |
| `kernel`    | `uname -r`                                                   |
| `os`        | `PRETTY_NAME` from `/etc/os-release`                         |

If the network or Neon is not up yet, it retries with backoff (2 s doubling
to 60 s) for up to an hour, then exits. It is not a long-running process: once
the row is in, it exits until the next boot.

## Install

On the Pi, from a copy of this folder (`~/mesh/neon_boot`):

```bash
~/mesh/neon_boot/install.sh
```

This makes a venv with psycopg in `~/neon-boot`, installs
`~/.config/systemd/user/neon-boot.service` and enables it. No sudo is needed,
because lingering is on for `carlesls`. Lingering lets user services start at
boot without anyone logging in.

Then store the connection string. Copy it from the Neon console under
Connect, using the form that starts with `postgresql://`. This prompt keeps it
out of the shell history and makes the file readable only by you:

```bash
( umask 077; read -rsp 'Neon URL: ' u && echo && printf 'DATABASE_URL=%s\n' "$u" > ~/.config/neon-boot/neon.env )
```

## Check it

```bash
systemctl --user start neon-boot      # log now, without rebooting
journalctl --user-unit neon-boot -b   # what it printed this boot
```

On this Pi the lines are in the system journal, so `journalctl --user -u`
shows nothing. Use `--user-unit`.

In Neon's SQL editor:

```sql
SELECT id, booted_at, logged_at, lan_ip FROM rpi_boots ORDER BY id DESC;
```

## Remove

```bash
systemctl --user disable neon-boot
rm -r ~/neon-boot ~/.config/neon-boot ~/.config/systemd/user/neon-boot.service
```
