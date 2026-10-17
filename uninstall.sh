#!/bin/bash
# Remove maild, its unit and the mail window. Mail database, cache, accounts.toml and keyring passwords are kept.
set -euo pipefail

systemctl --user stop maild.service 2>/dev/null || true
systemctl --user disable maild.service 2>/dev/null || true
rm -f ~/.config/systemd/user/maild.service ~/.local/bin/maild ~/.local/bin/omarchy-mail \
  ~/.local/share/applications/omarchy-mail.desktop
command -v update-desktop-database >/dev/null && update-desktop-database ~/.local/share/applications || true
systemctl --user daemon-reload

cat <<'MSG'
maild and the window removed. Your data was kept; to delete it too:

  rm -rf ~/.config/omarchy-mail ~/.local/share/omarchy-mail ~/.cache/omarchy-mail
  (drafts: ~/.local/share/omarchy-mail/drafts; window cache: ~/.cache/omarchy-mail/window)
  secret-tool clear service omarchy-mail account <email>    # once per account
MSG
