#!/usr/bin/env bash
# Packages the Linux build: G2fresh-<version>-Linux-x64.tar.gz, one folder with the app, g2bridge, the VST3 plugin,
# install.sh / uninstall.sh (linux/, for the current user, no root) and the licence.
#
#   tools/package/linux.sh [--build DIR] [--out DIR]      (default: build, dist)
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && cd .. && pwd)
build="$root/build"
out="$root/dist"
while [ $# -gt 0 ]; do
  case "$1" in
    --build) build=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    *) sed -n '2,5p' "$0" >&2; exit 2 ;;
  esac
done

version=$("$here/version.sh")
artefacts="$build/plugin/G2Editor_artefacts/Release"
bridge="$build/bridge/g2bridge"
for f in "$artefacts/Standalone/G2fresh" "$artefacts/VST3/G2fresh.vst3" "$bridge"; do
  [ -e "$f" ] || { echo "linux.sh: $f is missing: build first (cmake --build $build)" >&2; exit 1; }
done

name="G2fresh-$version-Linux-x64"
stage=$(mktemp -d "${TMPDIR:-/tmp}/g2fresh-package.XXXXXX")
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/$name" "$out"
cp -R "$artefacts/Standalone/G2fresh" "$bridge" "$artefacts/VST3/G2fresh.vst3" "$stage/$name/"
# g2bridge also goes in the plugin's Resources, where the plugin looks for it
mkdir -p "$stage/$name/G2fresh.vst3/Contents/Resources"
cp "$bridge" "$stage/$name/G2fresh.vst3/Contents/Resources/"
cp "$here/linux/install.sh" "$here/linux/uninstall.sh" "$root/LICENSE" "$root/NOTICE" "$stage/$name/"
chmod 755 "$stage/$name/G2fresh" "$stage/$name/g2bridge" "$stage/$name/G2fresh.vst3/Contents/Resources/g2bridge" \
  "$stage/$name/install.sh" "$stage/$name/uninstall.sh"
tar -C "$stage" -czf "$out/$name.tar.gz" "$name"
ls -l "$out/$name.tar.gz"
