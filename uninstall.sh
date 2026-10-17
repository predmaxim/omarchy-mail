#!/bin/bash
# Remove maild and its unit. Mail database, cache, accounts.toml and keyring passwords are kept.
set -euo pipefail

systemctl --user stop maild.service 2>/dev/null || true
systemctl --user disable maild.service 2>/dev/null || true
rm -f ~/.config/systemd/user/maild.service ~/.local/bin/maild
systemctl --user daemon-reload

cat <<'MSG'
maild removed. Your data was kept; to delete it too:

  rm -rf ~/.config/omarchy-mail ~/.local/share/omarchy-mail ~/.cache/omarchy-mail
  secret-tool clear service omarchy-mail account <email>    # once per account
MSG
