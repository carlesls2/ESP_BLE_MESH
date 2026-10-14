#!/usr/bin/env bash
# Install neon-boot as a systemd user service for the current user.
# Needs no sudo as long as lingering is on for the user.
set -euo pipefail

src=$(cd "$(dirname "$0")" && pwd)
dest=~/neon-boot
conf=~/.config/neon-boot

mkdir -p "$dest" "$conf" ~/.config/systemd/user
chmod 700 "$conf"
install -m 755 "$src/neon_boot.py" "$dest/"
[ -d "$dest/.venv" ] || python3 -m venv "$dest/.venv"
"$dest/.venv/bin/pip" install --quiet --upgrade 'psycopg[binary]>=3.2,<4'

install -m 644 "$src/neon-boot.service" ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable neon-boot.service

if [ "$(loginctl show-user "$USER" -p Linger --value)" != yes ]; then
    echo "Lingering is off, so the service only starts when you log in." >&2
    echo "Turn it on with: sudo loginctl enable-linger $USER" >&2
fi
if [ ! -f "$conf/neon.env" ]; then
    echo "Now put the Neon connection string in $conf/neon.env (see README.md)."
fi
