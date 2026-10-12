#!/bin/sh
# Installs G2fresh for the current user, without root:
#   the app and g2bridge (which it starts when a G2 is connected) in ~/.local/bin,
#   a menu entry in ~/.local/share/applications,
#   the VST3 plugin in ~/.vst3.
# Run it from the unpacked archive: ./install.sh        (./uninstall.sh removes these again)
set -eu

here=$(cd "$(dirname "$0")" && pwd)
bin="$HOME/.local/bin"
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
vst3="$HOME/.vst3"

for f in G2fresh g2bridge G2fresh.vst3; do
  if [ ! -e "$here/$f" ]; then
    echo "install.sh: $f is not next to this script: run it from the unpacked archive" >&2
    exit 1
  fi
done

mkdir -p "$bin" "$apps" "$vst3"
cp "$here/G2fresh" "$here/g2bridge" "$bin/"
chmod 755 "$bin/G2fresh" "$bin/g2bridge"
# the whole bundle anew: a file an older version had and this one has not must not stay
rm -rf "$vst3/G2fresh.vst3"
cp -R "$here/G2fresh.vst3" "$vst3/"

cat > "$apps/g2fresh.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=G2fresh
GenericName=Nord Modular G2 editor
Comment=Editor for the Clavia Nord Modular G2
Exec="$bin/G2fresh" %f
Terminal=false
Categories=AudioVideo;Audio;Midi;
EOF
if command -v update-desktop-database > /dev/null 2>&1; then
  update-desktop-database "$apps" > /dev/null 2>&1 || true
fi

cat <<EOF
G2fresh is installed:
  $bin/G2fresh (and g2bridge)
  $apps/g2fresh.desktop
  $vst3/G2fresh.vst3
EOF
case ":$PATH:" in
  *":$bin:"*) ;;
  *) echo "($bin is not on your PATH: start G2fresh from the applications menu, or as $bin/G2fresh.)" ;;
esac
cat <<'EOF'

To connect a G2 by USB, your user needs access to the device. That takes root, so this script leaves it to you:
create /etc/udev/rules.d/50-nord-g2.rules with the line

  SUBSYSTEM=="usb", ATTRS{idVendor}=="0ffc", ATTRS{idProduct}=="0002", MODE="0666"

then run `sudo udevadm control --reload` and plug the G2 in again.

The Emulated G2 and the Classic look need your own copy of Nord's free "Nord Modular G2 OS v1.62 Update" for the
Mac (the .dmg, from nordkeyboards.com): G2fresh asks for it the first time. G2fresh is not affiliated with Clavia.
EOF
