# Packages the Windows build: G2fresh-<version>-Windows-x64.zip (the app, g2bridge, the VST3 plugin) and the
# installer G2fresh-<version>-Windows-x64-setup.exe (Inno Setup 6.3 or later, windows\G2fresh.iss), and
# signing-Windows.txt, a line that says whether they are signed.
#
#   pwsh tools/package/windows.ps1 [-Build DIR] [-Out DIR]      (default: build, dist)
#
# Unsigned unless the environment has a certificate (docs/RELEASING.md):
#   WINDOWS_CERT_PFX_BASE64   a code-signing certificate with its key, a .pfx in base64
#   WINDOWS_CERT_PASSWORD     the .pfx's password
# It is imported into the user's certificate store for the time of the packaging and removed again; the
# executables, the plugin, Setup and the uninstaller are then signed by windows\sign.ps1.
param(
    [string]$Build = 'build',
    [string]$Out = 'dist'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$version = (Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern '^project\(G2fresh VERSION ([0-9][0-9.]*)').Matches[0].Groups[1].Value
if (-not $version) { throw 'windows.ps1: no project(G2fresh VERSION ...) in CMakeLists.txt' }

$Build = (Resolve-Path $Build).Path
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$artefacts = Join-Path $Build 'plugin\G2Editor_artefacts\Release'
$bridge = Join-Path $Build 'bridge\g2bridge.exe'
foreach ($f in (Join-Path $artefacts 'Standalone\G2fresh.exe'), (Join-Path $artefacts 'VST3\G2fresh.vst3'), $bridge) {
    if (-not (Test-Path $f)) { throw "windows.ps1: $f is missing: build first (cmake --build $Build)" }
}

$name = "G2fresh-$version-Windows-x64"
$work = Join-Path ([IO.Path]::GetTempPath()) "g2fresh-package-$PID"
$stage = Join-Path $work 'stage'
$signScript = Join-Path $PSScriptRoot 'windows\sign.ps1'
$certificate = $null

try {
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    Copy-Item (Join-Path $artefacts 'Standalone\G2fresh.exe') $stage
    Copy-Item $bridge $stage
    Copy-Item (Join-Path $artefacts 'VST3\G2fresh.vst3') $stage -Recurse
    # g2bridge also goes in the plugin's Resources, where the plugin looks for it
    $resources = Join-Path $stage 'G2fresh.vst3\Contents\Resources'
    New-Item -ItemType Directory -Force -Path $resources | Out-Null
    Copy-Item $bridge $resources
    Copy-Item (Join-Path $root 'LICENSE') (Join-Path $stage 'LICENSE.txt')
    Copy-Item (Join-Path $root 'NOTICE') (Join-Path $stage 'NOTICE.txt')

    # ---- the signatures of what is installed -----------------------------------------------------------------
    if ($env:WINDOWS_CERT_PFX_BASE64) {
        if (-not $env:WINDOWS_CERT_PASSWORD) { throw 'windows.ps1: WINDOWS_CERT_PFX_BASE64 is set: WINDOWS_CERT_PASSWORD must be too' }
        $pfx = Join-Path $work 'certificate.pfx'
        [IO.File]::WriteAllBytes($pfx, [Convert]::FromBase64String($env:WINDOWS_CERT_PFX_BASE64))
        $password = ConvertTo-SecureString $env:WINDOWS_CERT_PASSWORD -AsPlainText -Force
        $certificate = @(Import-PfxCertificate -FilePath $pfx -CertStoreLocation Cert:\CurrentUser\My -Password $password) |
            Where-Object { $_.HasPrivateKey } | Select-Object -First 1
        if (-not $certificate) { throw 'windows.ps1: the .pfx holds no certificate with its private key' }
        Remove-Item $pfx -Force
        $env:G2FRESH_SIGN_THUMBPRINT = $certificate.Thumbprint
        Write-Host "windows.ps1: signing as $($certificate.Subject)"
        $binaries = @(Get-ChildItem -Path $stage -Recurse -File | Where-Object { $_.Extension -in '.exe', '.vst3', '.dll' } |
            ForEach-Object { $_.FullName })
        & $signScript @binaries
    } else {
        Write-Host 'windows.ps1: no WINDOWS_CERT_PFX_BASE64: nothing is signed'
    }

    # ---- the zip ---------------------------------------------------------------------------------------------
    $zip = Join-Path $Out "$name.zip"
    Remove-Item $zip -Force -ErrorAction SilentlyContinue
    if (Get-Command 7z -ErrorAction SilentlyContinue) {
        Push-Location $stage
        try {
            & 7z a -tzip $zip . | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "windows.ps1: 7z failed (exit code $LASTEXITCODE)" }
        } finally { Pop-Location }
    } else {
        Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
    }

    # ---- the installer ---------------------------------------------------------------------------------------
    $onPath = Get-Command iscc.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($onPath) {
        $iscc = $onPath.Source
    } else {
        $iscc = Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'
        if (-not (Test-Path $iscc)) { throw 'windows.ps1: Inno Setup 6 is not installed (ISCC.exe; e.g. choco install innosetup)' }
    }
    $options = @("/DAppVersion=$version", "/DStage=$stage", "/DOutDir=$Out", "/DOutName=$name-setup")
    if ($certificate) {
        # Inno Setup signs Setup and the uninstaller with this command; $q is a quote and $f the file, in its syntax
        $options += '/DSigned'
        $options += "/Sg2sign=powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File `$q$signScript`$q `$f"
    }
    & $iscc /Q @options (Join-Path $PSScriptRoot 'windows\G2fresh.iss')
    if ($LASTEXITCODE -ne 0) { throw "windows.ps1: Inno Setup failed (exit code $LASTEXITCODE)" }

    $setup = Join-Path $Out "$name-setup.exe"
    if (-not (Test-Path $setup)) { throw "windows.ps1: Inno Setup did not write $setup" }
    $status = (Get-AuthenticodeSignature $setup).Status
    Write-Host "windows.ps1: $name-setup.exe, signature: $status"
    if ($certificate -and $status -eq 'NotSigned') { throw 'windows.ps1: Setup is not signed' }
    # What was done, in one line: in the run's summary, and in signing-Windows.txt for the release's notes (ci.yml)
    $state = if ($certificate) { 'signed' } else { 'not signed: Windows SmartScreen warns before it runs (README, Download)' }
    Set-Content -Path (Join-Path $Out 'signing-Windows.txt') -Value "Windows 10 or later (x64): $state"
    if ($env:GITHUB_ACTIONS) { Write-Host "::notice title=Windows package::${name}: $state" }
    Get-Item $zip, $setup | Format-Table Name, Length
} finally {
    if ($certificate) {
        # out of the store again, with its key; a failure here must not hide what went wrong above
        try {
            Remove-Item "Cert:\CurrentUser\My\$($certificate.Thumbprint)" -DeleteKey
        } catch {
            Write-Host "windows.ps1: the certificate could not be removed from the store: $_"
        }
    }
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
