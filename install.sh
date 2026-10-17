#!/bin/bash
# Build maild, install it and its systemd user unit, (re)start the service.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

command -v cargo >/dev/null || { echo "cargo not found: install Rust (pacman -S rust)" >&2; exit 1; }

(cd maild && cargo build --release)
install -Dm755 maild/target/release/maild ~/.local/bin/maild
install -Dm644 maild/systemd/maild.service ~/.config/systemd/user/maild.service

systemctl --user daemon-reload
systemctl --user enable --now maild.service
systemctl --user restart maild.service  # picks up a new binary on update

cat <<'MSG'
maild is running. Add an account with an app password:

  read -rs P; printf %s "$P" | maild add-account gmail you@example.com "Your Name"

Logs: journalctl --user -u maild -f
MSG
