#!/usr/bin/env bash
# Packages the macOS build: signs the app and the plugins, builds the installer, has Apple notarize both, and
# writes G2fresh-<version>-macOS-<universal|arch>.pkg and .zip, and signing-macOS.txt, a line that says how they
# are signed (see docs/RELEASING.md).
#
#   tools/package/macos.sh [--build DIR] [--out DIR]      (default: build, dist)
#
# Without anything in the environment it signs ad-hoc and leaves the installer unsigned: what a fork, a pull
# request or your own machine gets. A release sets:
#
#   MACOS_SIGN_IDENTITY       "Developer ID Application: NAME (TEAMID)", for the bundles and g2bridge
#   MACOS_INSTALLER_IDENTITY  "Developer ID Installer: NAME (TEAMID)", for the .pkg
#   MACOS_KEYCHAIN            the keychain to take them from (optional; it must be on the search list as well)
# and, for the notarization (only of a build signed with a Developer ID), one of:
#   NOTARY_KEY_ID, NOTARY_ISSUER_ID and NOTARY_KEY_P8 (the .p8 file) or NOTARY_KEY_P8_BASE64 (its contents):
#                             an App Store Connect API key (no issuer for an individual key)
#   NOTARY_APPLE_ID, NOTARY_TEAM_ID, NOTARY_PASSWORD: an Apple ID with an app-specific password
#   NOTARY_KEYCHAIN_PROFILE   a profile saved on this Mac with `xcrun notarytool store-credentials`
#
# The signatures (what each needs was tested, docs/RELEASING.md "Entitlements"):
# * everything with the hardened runtime, which the notarization requires, ad-hoc builds too, so that every build
#   runs under the release's restrictions;
# * the app with macos/G2fresh.entitlements: allow-jit (the Emulated G2's DSPs run on generated code) and
#   audio-input (the audio device's inputs are the G2's IN 1-4);
# * g2bridge and the plugins without entitlements: g2bridge needs none, and a plugin lives by its host's.
# Inside out, never --deep: g2bridge first, then the bundle around it.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && cd .. && pwd)
build="$root/build"
out="$root/dist"
while [ $# -gt 0 ]; do
  case "$1" in
    --build) build=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    *) sed -n '2,6p' "$0" >&2; exit 2 ;;
  esac
done

say() { echo "macos.sh: $*"; }
die() { echo "macos.sh: $*" >&2; exit 1; }
# a line in the summary of a GitHub Actions run
notice() { say "$*"; if [ -n "${GITHUB_ACTIONS:-}" ]; then echo "::notice title=macOS package::$*"; fi; }

version=$("$here/version.sh")
artefacts="$build/plugin/G2Editor_artefacts/Release"
bridge="$build/bridge/g2bridge"
for f in "$artefacts/Standalone/G2fresh.app" "$artefacts/VST3/G2fresh.vst3" "$artefacts/AU/G2fresh.component" "$bridge"; do
  [ -e "$f" ] || die "$f is missing: build first (cmake --build $build)"
done

identity=${MACOS_SIGN_IDENTITY:--}
installer_identity=${MACOS_INSTALLER_IDENTITY:-}
keychain=()
if [ -n "${MACOS_KEYCHAIN:-}" ]; then keychain=(--keychain "$MACOS_KEYCHAIN"); fi

stage=$(mktemp -d "${TMPDIR:-/tmp}/g2fresh-package.XXXXXX")
trap 'rm -rf "$stage"' EXIT
mkdir -p "$out"
out=$(cd "$out" && pwd)

# ---- the notary's credentials -------------------------------------------------------------------------------
notary=()
if [ -n "${NOTARY_KEYCHAIN_PROFILE:-}" ]; then
  notary=(--keychain-profile "$NOTARY_KEYCHAIN_PROFILE")
elif [ -n "${NOTARY_KEY_ID:-}" ]; then
  key=${NOTARY_KEY_P8:-}
  if [ -z "$key" ]; then
    if [ -z "${NOTARY_KEY_P8_BASE64:-}" ]; then die "NOTARY_KEY_ID is set: NOTARY_KEY_P8 or NOTARY_KEY_P8_BASE64 must be too"; fi
    key="$stage/notary-key.p8"
    (umask 077 && printf '%s' "$NOTARY_KEY_P8_BASE64" | base64 --decode > "$key")
  fi
  notary=(--key "$key" --key-id "$NOTARY_KEY_ID")
  if [ -n "${NOTARY_ISSUER_ID:-}" ]; then notary+=(--issuer "$NOTARY_ISSUER_ID"); fi
elif [ -n "${NOTARY_APPLE_ID:-}" ]; then
  if [ -z "${NOTARY_TEAM_ID:-}" ] || [ -z "${NOTARY_PASSWORD:-}" ]; then
    die "NOTARY_APPLE_ID is set: NOTARY_TEAM_ID and NOTARY_PASSWORD must be too"
  fi
  notary=(--apple-id "$NOTARY_APPLE_ID" --team-id "$NOTARY_TEAM_ID" --password "@env:NOTARY_PASSWORD")
fi
if [ "$identity" = - ] && [ ${#notary[@]} -gt 0 ]; then
  say "no MACOS_SIGN_IDENTITY: an ad-hoc build cannot be notarized, the notary's credentials are not used"
  notary=()
fi

# notarize <zip or pkg>: submits it and waits for Apple's answer; prints Apple's log when it is not accepted
notarize() {
  local file=$1 result id status
  result="$stage/notary-$(basename "$file").json"
  say "notarizing $(basename "$file") (a few minutes)"
  xcrun notarytool submit "$file" "${notary[@]}" --wait --timeout 45m --output-format json > "$result" || true
  id=$(plutil -extract id raw -o - "$result" 2>/dev/null || true)
  status=$(plutil -extract status raw -o - "$result" 2>/dev/null || true)
  if [ "$status" != Accepted ]; then
    cat "$result" >&2 || true
    if [ -n "$id" ]; then xcrun notarytool log "$id" "${notary[@]}" >&2 || true; fi
    die "Apple did not notarize $(basename "$file") (status: ${status:-none})"
  fi
  say "notarized $(basename "$file") (submission $id)"
}

# ---- the bundles, signed ------------------------------------------------------------------------------------
bundles="$stage/bundles"
mkdir -p "$bundles"
ditto --norsrc --noextattr "$artefacts/Standalone/G2fresh.app" "$bundles/G2fresh.app"
ditto --norsrc --noextattr "$artefacts/VST3/G2fresh.vst3" "$bundles/G2fresh.vst3"
ditto --norsrc --noextattr "$artefacts/AU/G2fresh.component" "$bundles/G2fresh.component"
# g2bridge ships next to the app's executable and in each plugin's Resources, where the editors look for it
bridges=("$bundles/G2fresh.app/Contents/MacOS/g2bridge" "$bundles/G2fresh.vst3/Contents/Resources/g2bridge"
         "$bundles/G2fresh.component/Contents/Resources/g2bridge")
for b in "${bridges[@]}"; do
  mkdir -p "$(dirname "$b")"
  cp "$bridge" "$b"
done
# No extended attributes (quarantine, provenance, Finder's): codesign refuses some, and the installer and the zip
# would carry the rest onto every Mac as ._ files. A bundle's signature is in files, not in attributes.
xattr -cr "$bundles"

archs=$(lipo -archs "$bundles/G2fresh.app/Contents/MacOS/G2fresh")
flavour=${archs// /-}
if [ "$flavour" = x86_64-arm64 ] || [ "$flavour" = arm64-x86_64 ]; then flavour=universal; fi
name="G2fresh-$version-macOS-$flavour"
minos=$(vtool -show-build "$bundles/G2fresh.app/Contents/MacOS/G2fresh" | awk '$1 == "minos" { print $2 }' | sort -V | tail -1)
[ -n "$minos" ] || die "cannot read the oldest macOS the build runs on (vtool -show-build)"

# sign <code> [codesign options...]
sign() {
  local code=$1
  shift
  local args=(--force --sign "$identity" --options runtime)
  if [ "$identity" != - ]; then args+=(--timestamp); fi
  codesign "${args[@]}" ${keychain[@]+"${keychain[@]}"} "$@" "$code"
}
for b in "${bridges[@]}"; do
  sign "$b" --identifier org.g2fresh.g2bridge
done
sign "$bundles/G2fresh.app" --entitlements "$here/macos/G2fresh.entitlements"
sign "$bundles/G2fresh.vst3"
sign "$bundles/G2fresh.component"
for code in "${bridges[@]}" "$bundles/G2fresh.app" "$bundles/G2fresh.vst3" "$bundles/G2fresh.component"; do
  codesign --verify --strict "$code" || die "the signature of $code does not verify"
done

if [ ${#notary[@]} -gt 0 ]; then
  # A ticket cannot be stapled to a zip: Apple notarizes the zip, the tickets go onto the bundles in it, and the
  # zip people download is made from those (below), as is the installer.
  (cd "$bundles" && ditto -c -k --norsrc --noextattr . "$stage/bundles.zip")
  notarize "$stage/bundles.zip"
  for b in G2fresh.app G2fresh.vst3 G2fresh.component; do
    xcrun stapler staple "$bundles/$b"
    xcrun stapler validate "$bundles/$b"
  done
  # Gatekeeper's own verdict on the app, for the log (the runner may have Gatekeeper switched off)
  spctl --assess --type execute -vv "$bundles/G2fresh.app" || say "(spctl does not accept the app here)"
fi

# ---- the installer ------------------------------------------------------------------------------------------
# One package per choice. Each bundle is fixed where it is installed: the Installer otherwise "relocates" a
# bundle to wherever it finds one with the same identifier (a developer's build folder, a copy on the desktop),
# and all three share theirs (org.g2fresh.editor). No version check: an older installer puts the older version back.
mkdir -p "$stage/packages"
component() {  # component <id> <bundle> <install location>
  local id=$1 bundle=$2 location=$3
  mkdir -p "$stage/root-$id"
  ditto --norsrc --noextattr "$bundles/$bundle" "$stage/root-$id/$bundle"
  xattr -cr "$stage/root-$id"
  cat > "$stage/$id.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<array>
	<dict>
		<key>RootRelativeBundlePath</key>
		<string>$bundle</string>
		<key>BundleIsRelocatable</key>
		<false/>
		<key>BundleIsVersionChecked</key>
		<false/>
		<key>BundleHasStrictIdentifier</key>
		<true/>
		<key>BundleOverwriteAction</key>
		<string>upgrade</string>
	</dict>
</array>
</plist>
EOF
  pkgbuild --quiet --root "$stage/root-$id" --install-location "$location" --identifier "org.g2fresh.pkg.$id" \
    --version "$version" --component-plist "$stage/$id.plist" --ownership recommended "$stage/packages/G2fresh-$id.pkg"
}
component app G2fresh.app /Applications
component vst3 G2fresh.vst3 /Library/Audio/Plug-Ins/VST3
component au G2fresh.component /Library/Audio/Plug-Ins/Components

mkdir -p "$stage/resources"
fill() { sed -e "s/@VERSION@/$version/g" -e "s/@MINOS@/$minos/g" -e "s/@ARCHS@/${archs// /,}/g" "$1"; }
fill "$here/macos/distribution.xml" > "$stage/distribution.xml"
fill "$here/macos/welcome.html" > "$stage/resources/welcome.html"
cp "$root/LICENSE" "$stage/resources/LICENSE.txt"
product=(--distribution "$stage/distribution.xml" --package-path "$stage/packages" --resources "$stage/resources")
if [ -n "$installer_identity" ]; then
  product+=(--sign "$installer_identity" --timestamp ${keychain[@]+"${keychain[@]}"})
fi
rm -f "$out/$name.pkg" "$out/$name.zip"
productbuild --quiet "${product[@]}" "$out/$name.pkg"

# What the installer holds, taken apart again: the three bundles, their signatures intact.
pkgutil --expand-full "$out/$name.pkg" "$stage/expanded"
for b in G2fresh-app.pkg/Payload/G2fresh.app G2fresh-vst3.pkg/Payload/G2fresh.vst3 G2fresh-au.pkg/Payload/G2fresh.component; do
  codesign --verify --strict "$stage/expanded/$b" || die "$b in the installer: its signature does not verify"
done
if [ -n "$installer_identity" ]; then
  pkgutil --check-signature "$out/$name.pkg" > "$stage/pkg-signature.txt" || { cat "$stage/pkg-signature.txt" >&2; die "the installer's signature does not verify"; }
fi
if [ ${#notary[@]} -gt 0 ] && [ -n "$installer_identity" ]; then
  notarize "$out/$name.pkg"
  xcrun stapler staple "$out/$name.pkg"
  xcrun stapler validate "$out/$name.pkg"
  spctl --assess --type install -vv "$out/$name.pkg" || say "(spctl does not accept the installer here)"
fi

# ---- the zip: the bundles as they are in the installer, with the licence -------------------------------------
cp "$root/LICENSE" "$root/NOTICE" "$bundles/"
xattr -cr "$bundles"
(cd "$bundles" && ditto -c -k --norsrc --noextattr . "$out/$name.zip")

# What was done, in one line: in the run's summary, and in signing-macOS.txt for the release's notes (ci.yml).
if [ "$identity" = - ]; then
  state="not signed (ad-hoc only) and not notarized: macOS refuses to open it until told to (README, Download)"
elif [ ${#notary[@]} -eq 0 ]; then
  state="signed with a Developer ID but NOT notarized: macOS refuses to open it until told to (README, Download)"
elif [ -z "$installer_identity" ]; then
  state="app and plugins signed with a Developer ID and notarized by Apple; the installer itself is NOT signed"
else
  state="signed with a Developer ID and notarized by Apple"
fi
echo "macOS $minos or later (${archs// /, }): $state" > "$out/signing-macOS.txt"
notice "$name: $state"
ls -l "$out/$name.pkg" "$out/$name.zip"
