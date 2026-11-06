# Releasing G2fresh

A release is five files, built, packaged, signed and attached to a draft GitHub release by CI (`.github/workflows/ci.yml`) when a tag `v<version>` is pushed:

| File | What | Made by |
|------|------|---------|
| `G2fresh-<version>-macOS-universal.pkg` | Installer: the app to `/Applications`, the VST3 to `/Library/Audio/Plug-Ins/VST3`, the Audio Unit to `/Library/Audio/Plug-Ins/Components`, each optional | `tools/package/macos.sh` |
| `G2fresh-<version>-macOS-universal.zip` | The same three bundles, to copy by hand | `tools/package/macos.sh` |
| `G2fresh-<version>-Windows-x64-setup.exe` | Installer (Inno Setup): the app and `g2bridge.exe` to `Program Files\G2fresh`, the VST3 to `Common Files\VST3`, a Start menu entry, an uninstaller | `tools/package/windows.ps1` |
| `G2fresh-<version>-Windows-x64.zip` | The same files | `tools/package/windows.ps1` |
| `G2fresh-<version>-Linux-x64.tar.gz` | The app, `g2bridge`, the VST3, `install.sh` and `uninstall.sh` (for the current user, no root) | `tools/package/linux.sh` |

With the secrets below in the repository, the macOS files are signed with a Developer ID and notarized by Apple, and the Windows files are signed. Without them (a fork, a pull request, before you have the certificates, your own machine) the same scripts sign the macOS bundles ad-hoc and leave the rest unsigned, as releases were up to 0.10.0. Nothing else changes: the build never fails for a missing secret.

Only a release is signed: a `v*` tag, or a run started by hand with "sign" ticked (Actions > CI > Run workflow), which rehearses the signing without a tag. Other pushes are packaged unsigned, so that an ordinary push does not wait on Apple's notary service. The `SIGN` line at the top of `ci.yml` decides this.

## Cutting a release

1. Set the version in `CMakeLists.txt` (`project(G2fresh VERSION x.y.z ...)`) and in the README's status line; commit and push.
2. Optional, and worth it the first time the secrets are in place: Actions > CI > Run workflow on that commit with "sign" ticked, and look at the artifacts.
3. Tag and push: `git tag v<x.y.z> && git push origin v<x.y.z>`.
4. CI checks that the tag is the version in `CMakeLists.txt` (the Version job; it fails before anything is built when they differ), builds and tests the three systems, packages them, and attaches the five files to a **draft** release named after the tag. Its notes start with how each system's files are signed, as the packaging scripts report it.
5. Check the draft's files (below), write the notes, and publish the draft by hand.

If the tag was wrong: delete it (`git push origin :refs/tags/v<x.y.z>`, `git tag -d v<x.y.z>`), fix, tag again. Running the workflow again on the same tag replaces the files of the draft and leaves its notes; it refuses to touch a release that is already published.

Checks worth doing on the draft's files before publishing:

```sh
# macOS, on a Mac that did not build them (download with a browser, so that they are quarantined)
spctl --assess --type install -vv G2fresh-<version>-macOS-universal.pkg   # "accepted", "source=Notarized Developer ID"
xcrun stapler validate G2fresh-<version>-macOS-universal.pkg
pkgutil --check-signature G2fresh-<version>-macOS-universal.pkg
# then open it: no warning; and start the Emulated G2 in the installed app and in a DAW
```

On Windows: the file's Properties > Digital Signatures, then run it.

## The secrets

You set these yourself with `gh secret set` (or Settings > Secrets and variables > Actions). Never paste a secret's value into a chat, an issue, a commit or a log; the commands below read the values from files or ask for them without showing them.

| Secret | What |
|--------|------|
| `MACOS_CERT_P12_BASE64` | One `.p12` holding both Developer ID identities (Application and Installer) with their private keys, in base64 |
| `MACOS_CERT_PASSWORD` | The password you gave that `.p12` |
| `MACOS_SIGN_IDENTITY` | `Developer ID Application: NAME (TEAMID)`, as `security find-identity -v` prints it |
| `MACOS_INSTALLER_IDENTITY` | `Developer ID Installer: NAME (TEAMID)` |
| `NOTARY_KEY_ID` | The App Store Connect API key's ID (10 characters) |
| `NOTARY_ISSUER_ID` | The issuer ID shown above the list of keys (a UUID); none for an individual key |
| `NOTARY_KEY_P8_BASE64` | The key file `AuthKey_<id>.p8`, in base64 |
| `WINDOWS_CERT_PFX_BASE64` | A code-signing certificate with its key, a `.pfx` in base64 (see "Windows" for whether you can have one) |
| `WINDOWS_CERT_PASSWORD` | The `.pfx`'s password |

Instead of the three `NOTARY_KEY_*` secrets, an Apple ID works too: `NOTARY_APPLE_ID`, `NOTARY_TEAM_ID` and `NOTARY_PASSWORD` (an app-specific password, never the account's). The API key is the better choice: it is not a person's login and can be revoked alone.

Any subset is allowed. Without `MACOS_SIGN_IDENTITY` the macOS build is ad-hoc; with it but without the notary's secrets it is signed and not notarized (macOS still refuses it); without `MACOS_INSTALLER_IDENTITY` the bundles are signed and notarized and the `.pkg` is not signed.

## macOS: the certificates and the notary key

You need a paid Apple Developer Program membership (99 USD a year when this was written: check the current price), as an individual or an organization. The name on the certificates is the membership's: your own name for an individual. Only the Account Holder can create Developer ID certificates.

### 1. The two Developer ID certificates

The short way, with Xcode: Xcode > Settings > Accounts > your Apple ID > Manage Certificates... > "+" > **Developer ID Application**, then "+" > **Developer ID Installer**. Xcode makes the keys and puts both identities in your login keychain.

Without Xcode's help:

1. Keychain Access > Certificate Assistant > Request a Certificate From a Certificate Authority...: your email, a name, "Saved to disk". This makes a private key in your login keychain and a `.certSigningRequest` file.
2. [developer.apple.com/account](https://developer.apple.com/account) > Certificates, IDs & Profiles > Certificates > "+" > Developer ID Application (the newer "G2 Sub-CA" profile when asked), upload the request, download the `.cer`, double-click it. Do the same for Developer ID Installer (the same request file will do).

Then check that both are there with their keys:

```sh
security find-identity -v | grep "Developer ID"
```

It prints the two names for `MACOS_SIGN_IDENTITY` and `MACOS_INSTALLER_IDENTITY`. A Developer ID certificate is valid for five years; what was signed with it and notarized stays valid after that.

### 2. The `.p12` for CI

Keychain Access > login > My Certificates: select both "Developer ID Application: ..." and "Developer ID Installer: ..." (each must unfold to a private key), right-click > Export 2 items... > format Personal Information Exchange (.p12), and give it a strong password that you keep.

```sh
base64 -i DeveloperID.p12 | gh secret set MACOS_CERT_P12_BASE64 --repo r0main-f/G2fresh
gh secret set MACOS_CERT_PASSWORD --repo r0main-f/G2fresh        # asks for the value, does not show it
gh secret set MACOS_SIGN_IDENTITY --repo r0main-f/G2fresh        # Developer ID Application: NAME (TEAMID)
gh secret set MACOS_INSTALLER_IDENTITY --repo r0main-f/G2fresh   # Developer ID Installer: NAME (TEAMID)
```

Then delete the `.p12` or keep it somewhere safe: with its password it is your signature.

### 3. The notary's API key

[appstoreconnect.apple.com](https://appstoreconnect.apple.com) > Users and Access > Integrations > App Store Connect API > Team Keys > "+": a name ("G2fresh notarization"), access "Developer". Download `AuthKey_<id>.p8` (offered once), and note the Key ID in the list and the Issuer ID above it.

```sh
base64 -i AuthKey_XXXXXXXXXX.p8 | gh secret set NOTARY_KEY_P8_BASE64 --repo r0main-f/G2fresh
gh secret set NOTARY_KEY_ID --repo r0main-f/G2fresh
gh secret set NOTARY_ISSUER_ID --repo r0main-f/G2fresh
```

The Apple ID way instead: [account.apple.com](https://account.apple.com) > Sign-In and Security > App-Specific Passwords, then `gh secret set` for `NOTARY_APPLE_ID` (the Apple ID's email), `NOTARY_TEAM_ID` (the ten characters in parentheses in the certificates' names) and `NOTARY_PASSWORD` (the app-specific password).

Apple may ask you to accept a new agreement on developer.apple.com now and then; until you do, the notary answers with an error that says so.

### What CI does with them

The certificates go into a keychain made for the run and deleted at its end (`ci.yml`, "macOS signing certificates into a temporary keychain"). `tools/package/macos.sh` then:

1. copies the three bundles, puts `g2bridge` in each, and signs inside out, never with `--deep`: each `g2bridge`, then the bundle around it, all with the hardened runtime and a secure timestamp;
2. zips the bundles, has Apple notarize the zip (`xcrun notarytool submit --wait`), and staples the tickets onto the bundles (a zip cannot carry one);
3. builds one package per bundle with `pkgbuild` and the installer with `productbuild` (`macos/distribution.xml`: the welcome page, the licence, the three choices), signs it with the Installer identity, has Apple notarize it, and staples it;
4. zips the stapled bundles again: the `.zip` of the release.

Two submissions to Apple, a few minutes each. When Apple refuses one, the script prints Apple's log, which names each file and what is wrong with it.

### On your own Mac

```sh
tools/package/macos.sh                      # ad-hoc: dist/G2fresh-<version>-macOS-<...>.pkg and .zip, unsigned installer
```

With the certificates in your login keychain you can make the signed, notarized files yourself. Save the notary's credentials once in the keychain, then name them:

```sh
xcrun notarytool store-credentials g2fresh --key AuthKey_XXXXXXXXXX.p8 --key-id XXXXXXXXXX --issuer <issuer id>
MACOS_SIGN_IDENTITY="Developer ID Application: NAME (TEAMID)" \
MACOS_INSTALLER_IDENTITY="Developer ID Installer: NAME (TEAMID)" \
NOTARY_KEYCHAIN_PROFILE=g2fresh tools/package/macos.sh
```

### Entitlements

Notarization requires the hardened runtime, which takes away, among other things, generated code and the microphone unless the signature claims them back with entitlements. What G2fresh needs was tested (macOS 26.6 on an M1, both architectures, the x86_64 one under Rosetta; ad-hoc signatures with `codesign -o runtime`, which the system treats alike):

| Code | Entitlements | Why, and what happens without |
|------|--------------|-------------------------------|
| `G2fresh.app` | `com.apple.security.cs.allow-jit` | The Emulated G2's four DSPs run on code generated at run time (dsp56300's JIT, asmjit). Without it the emulator cannot get executable memory: until 0.10.0 it then jumped to address 0 (a crash); now `g2emu` checks first, and the Emulated G2 says why it does not start. The interpreter is no way out: dsp56300 picks its engine when it is compiled, and in a build that has the JIT it runs the interrupts there (calling its interpreter in such a build mixes the two, which it does not support). An interpreter-only build would be another build of the library, and slower. |
| | `com.apple.security.device.audio-input` | The audio device's inputs are the G2's IN 1-4. Without it macOS refuses the microphone at once, without asking the user (`tccd`: "requires entitlement com.apple.security.device.audio-input but it is missing"), so the inputs would be silent. Seen with a small test app that asks for the microphone, not with G2fresh; with the entitlement the system asks the user, which was not run (it puts a question on screen): open Setup > Audio/MIDI Settings once in the first signed build. |
| `g2bridge` | none | It runs the same under the hardened runtime: it serves the virtual G2, and libusb opens and lists the USB devices. Not tested with a G2 plugged in (there was none); USB needs no entitlement outside the App Sandbox. |
| `G2fresh.vst3`, `G2fresh.component` | none (they would be ignored) | A plugin runs under its host's entitlements, not its own. See below. |

Not needed, and so not claimed: `allow-unsigned-executable-memory` and `disable-executable-page-protection` (each also makes the emulator run, but `allow-jit` alone does, and is the narrowest: asmjit uses `MAP_JIT` memory), and `disable-library-validation` (the app loads no code of anyone else's). Reading the user's files needs no entitlement: G2fresh is not sandboxed.

**The plugins.** With a host signed with the hardened runtime (`g2plugintest` signed in each way, loading the signed VST3):

| Host's entitlements | Result |
|---------------------|--------|
| none | The plugin does not load at all (library validation: the host only loads code of its own team). No plugin of any vendor loads in such a host. |
| `disable-library-validation` | The plugin loads; the Emulated G2 does not start and says why. Before the check in `g2emu`: a crash of the host. |
| `disable-library-validation` + `allow-jit` | Everything works. |
| `disable-library-validation` + `allow-unsigned-executable-memory` | Everything works. This is what Live 11, Apple's `auval` and Apple's Audio Unit hosting service have. |

So the Emulated G2 runs in any host that lets plugins generate code, which hosts that load third-party plugins commonly do; G2fresh cannot change it for a host that does not.

The stand-alone app was also started as packaged (out of the release zip, hidden, with a scratch home folder and no audio device): its emulated G2 starts; signed without `allow-jit` it runs without one.

### The oldest macOS

The installer refuses a Mac older than what the build runs on, which it reads from the app (`vtool -show-build`). The build sets no deployment target (`CMAKE_OSX_DEPLOYMENT_TARGET`), so that is the macOS of the machine that built it: the CI runner's for a release. With `-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0` the app builds without complaint (tried on arm64), but it has not been run on an older macOS. Until that is decided, a release needs the macOS its runner had: the macOS line of the draft's notes says which.

### Removing G2fresh

There is no uninstaller. Put these in the Trash:

```
/Applications/G2fresh.app
/Library/Audio/Plug-Ins/VST3/G2fresh.vst3
/Library/Audio/Plug-Ins/Components/G2fresh.component
```

Settings, the Emulated G2's memory and the Classic look's cache are in `~/Library/Application Support/G2fresh`. macOS also keeps a note that the packages were installed; it does no harm, and `sudo pkgutil --forget org.g2fresh.pkg.app` (and `.vst3`, `.au`) removes it.

## Windows

The installer is `tools/package/windows/G2fresh.iss`, compiled by `tools/package/windows.ps1` with Inno Setup 6.3 or later (CI installs it with Chocolatey when the runner has none). It installs for all users, so it asks for administrator rights. Its last page says how to give the G2 the WinUSB driver (Zadig), which G2fresh does not install.

### Signing: what you can get

Unsigned, the installer works and SmartScreen warns ("More info" > "Run anyway"). Signing removes "unknown publisher"; SmartScreen may still warn about a new certificate until enough people have run the file.

Check every price and condition here before acting: they change, and these are from memory.

- **A `.pfx` file**, which the two `WINDOWS_CERT_*` secrets take. Since June 2023 the certificate authorities must keep code-signing keys on certified hardware (a token, an HSM, a cloud service), so a new publicly trusted certificate no longer comes as a file. This path serves a certificate from before that, or a rehearsal with a self-signed one (below).
- **Azure Trusted Signing** (Microsoft; it has been renamed, look for "Trusted Signing" or "Artifact Signing"): about 10 USD a month. Microsoft checks your identity and signs in the cloud, which suits CI. When this was written it was open to organizations in a few regions and to individuals in fewer: check whether you qualify.
- **SignPath Foundation**: free for open-source projects that qualify; the signature then names SignPath Foundation as the publisher, and the files must come from CI. G2fresh's licence fits; you apply.
- **Certum's open-source certificate**: tens of euros a year plus a card reader or their cloud signing; it names you, and is awkward in CI (it wants a code from a phone).
- **An OV or EV certificate with cloud signing** (SSL.com, DigiCert, Sectigo...): a few hundred USD a year. EV no longer buys an instant SmartScreen pass.

Every signature goes through `tools/package/windows/sign.ps1` (the executables, the plugin, and Setup and the uninstaller through Inno Setup's `SignTool`). It signs with a certificate in the user's store, where `windows.ps1` puts the `.pfx` for the time of the packaging. A signing service is one more case in that file: the same `signtool`, with the service's options (for Trusted Signing: `/dlib` with its library and `/dmdf` with a small JSON file, and its credentials as secrets) instead of `/sha1`. That case is not written, since it cannot be tested without an account.

### With a `.pfx`

```sh
base64 -i certificate.pfx | gh secret set WINDOWS_CERT_PFX_BASE64 --repo r0main-f/G2fresh
gh secret set WINDOWS_CERT_PASSWORD --repo r0main-f/G2fresh
```

To rehearse the signing steps without a real certificate, make a self-signed one, set the two secrets from it, run the workflow by hand with "sign", look at the Windows job, and **delete the two secrets again**: a self-signed signature is worth less than none to a user.

```sh
openssl req -x509 -newkey rsa:3072 -nodes -keyout test.key -out test.crt -days 30 -subj "/CN=G2fresh signing rehearsal" \
  -addext "keyUsage=critical,digitalSignature" -addext "extendedKeyUsage=critical,codeSigning"
openssl pkcs12 -export -inkey test.key -in test.crt -out test.pfx     # asks for a password
```

### Removing G2fresh

Settings > Apps > Installed apps > G2fresh > Uninstall. Settings stay in `%APPDATA%\G2fresh`.

## Linux

`tools/package/linux.sh` makes the archive: one folder with the app, `g2bridge`, `G2fresh.vst3`, the licence, and `install.sh` / `uninstall.sh` from `tools/package/linux/`. `install.sh` copies the app and `g2bridge` to `~/.local/bin`, writes `~/.local/share/applications/g2fresh.desktop`, copies the plugin to `~/.vst3`, and prints the udev rule a G2 needs (it does not install it: that takes root). Nothing is signed.

## What was checked, and where

On a Mac, without certificates:

- the entitlements, as above;
- `macos.sh` end to end, ad-hoc: the installer taken apart again (`pkgutil --expand-full`: the three bundles at their install locations, owned by root, not relocatable, their signatures and `g2bridge`'s intact, no stray attribute files), the zip's bundles verified;
- `macos.sh`'s signed and notarized path with stand-ins for `codesign`'s identity, `notarytool` and `stapler` (the script's flow for an API key, an Apple ID, a keychain profile and a refusal), not Apple's side of it;
- `linux.sh`, `install.sh` and `uninstall.sh` on a mock build tree with a scratch home;
- `windows.ps1` and `sign.ps1`: parsed by PowerShell, and run on the Mac against a mock build tree with stand-ins for ISCC, `signtool` and the certificate store;
- `shellcheck` on the shell scripts and `actionlint` on the workflow.

Only CI, or CI with the credentials, can show:

- that Inno Setup compiles `G2fresh.iss` and that the installer installs, upgrades and uninstalls as meant (the first run of the Windows job compiles it; installing it is by hand);
- the Chocolatey install of Inno Setup, if the runner has none;
- the import of the real certificates into the temporary keychain, and `codesign` and `productbuild` finding them there;
- the notarization and the stapling, and Gatekeeper accepting the result on another Mac;
- the Windows signature (the certificate import, `signtool`, Inno Setup calling `sign.ps1`);
- the release job: the draft, its five files and its notes.
