#!/bin/bash
# Build maild and the mail window, install them and the systemd user unit, (re)start the service.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

command -v cargo >/dev/null || { echo "cargo not found: install Rust (pacman -S rust)" >&2; exit 1; }

pkgs=(cmake ninja qt6-base qt6-declarative git)
if ! pacman -Q "${pkgs[@]}" &>/dev/null; then
  if command -v omarchy-pkg-add >/dev/null; then omarchy-pkg-add "${pkgs[@]}"
  else sudo pacman -S --needed --noconfirm "${pkgs[@]}"; fi
fi

(cd maild && cargo build --release)

# The window is built and tested before maild is restarted: a failure here leaves the running service alone.
# Separate lines, not one && chain: under set -e a failure mid-chain does not stop the script.
echo "The first window build downloads qlitehtml and litehtml from code.qt.io (needs network)."
cmake -S app -B app/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build app/build
ctest --test-dir app/build --output-on-failure --no-tests=error

install -Dm755 maild/target/release/maild ~/.local/bin/maild
install -Dm755 app/build/omarchy-mail ~/.local/bin/omarchy-mail
install -Dm644 app/omarchy-mail.desktop ~/.local/share/applications/omarchy-mail.desktop
command -v update-desktop-database >/dev/null && update-desktop-database ~/.local/share/applications || true
install -Dm644 maild/systemd/maild.service ~/.config/systemd/user/maild.service

systemctl --user daemon-reload
systemctl --user enable --now maild.service
systemctl --user restart maild.service  # picks up a new binary on update

cat <<'MSG'
maild is running; the window is `omarchy-mail` (Mail in the app menu). Add an account with an app password:

  read -rs P; printf %s "$P" | maild add-account gmail you@example.com "Your Name"

Logs: journalctl --user -u maild -f
MSG
