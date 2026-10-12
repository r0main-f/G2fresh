#!/bin/sh
# Removes what install.sh put in place. Your patches and G2fresh's settings (~/.config/G2fresh) stay.
set -eu

bin="$HOME/.local/bin"
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
vst3="$HOME/.vst3"

rm -f "$bin/G2fresh" "$bin/g2bridge" "$apps/g2fresh.desktop"
rm -rf "$vst3/G2fresh.vst3"
if command -v update-desktop-database > /dev/null 2>&1; then
  update-desktop-database "$apps" > /dev/null 2>&1 || true
fi

cat <<'EOF'
G2fresh is removed. Left in place:
  ~/.config/G2fresh (settings, the Emulated G2's memory, the Classic look's cache): delete it too if you want
  /etc/udev/rules.d/50-nord-g2.rules, if you created it: sudo rm /etc/udev/rules.d/50-nord-g2.rules
EOF
