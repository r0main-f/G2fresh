# Signs the files it is given (Authenticode, SHA-256, with a timestamp so that the signature outlives the
# certificate). Every Windows signature goes through here: windows.ps1 calls it for the executables and the
# plugin, and Inno Setup for Setup and the uninstaller (the "g2sign" sign tool, see G2fresh.iss).
#
#   sign.ps1 FILE...
#
# How it signs comes from the environment, so that another way only changes this file:
#   G2FRESH_SIGN_THUMBPRINT   the SHA-1 thumbprint of a certificate in the user's store (Cert:\CurrentUser\My);
#                             windows.ps1 imports the release's .pfx there and sets this
#   WINDOWS_TIMESTAMP_URL     the RFC 3161 timestamp server (default: DigiCert's)
#
# A signing service instead of a .pfx (Azure Trusted Signing, a cloud HSM: docs/RELEASING.md) is one more case
# below: the same signtool with the service's /dlib and /dmdf options instead of /sha1.
# Runs under Windows PowerShell 5.1 too (Inno Setup starts it with powershell.exe).
param([Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)][string[]]$Files)

$ErrorActionPreference = 'Stop'

$signtool = $null
$onPath = Get-Command signtool.exe -ErrorAction SilentlyContinue | Select-Object -First 1
if ($onPath) {
    $signtool = $onPath.Source
} else {
    # not on the PATH outside a developer prompt: the newest Windows SDK's
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $signtool = Get-ChildItem -Path $kits -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like '*\x64\signtool.exe' } |
        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $signtool) { throw 'sign.ps1: signtool.exe not found (it comes with the Windows SDK)' }

$timestamp = $env:WINDOWS_TIMESTAMP_URL
if (-not $timestamp) { $timestamp = 'http://timestamp.digicert.com' }

if ($env:G2FRESH_SIGN_THUMBPRINT) {
    $how = @('/sha1', $env:G2FRESH_SIGN_THUMBPRINT)
} else {
    throw 'sign.ps1: nothing to sign with (G2FRESH_SIGN_THUMBPRINT is not set; windows.ps1 sets it)'
}

# a timestamp server that does not answer fails the signature: try again a few times
for ($attempt = 1; ; $attempt++) {
    & $signtool sign @how /fd SHA256 /tr $timestamp /td SHA256 /d 'G2fresh' /du 'https://github.com/r0main-f/G2fresh' @Files
    if ($LASTEXITCODE -eq 0) { break }
    if ($attempt -ge 4) { throw "sign.ps1: signtool failed (exit code $LASTEXITCODE)" }
    Start-Sleep -Seconds (5 * $attempt)
}
