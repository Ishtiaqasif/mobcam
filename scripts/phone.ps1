# Development: builds the Android app, installs it on the USB-connected phone and starts Metro.
#   .\scripts\phone.ps1
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'android-env.ps1')
$app = Join-Path $PSScriptRoot '..\app' | Resolve-Path

Push-Location $app
try {
  if (-not (Test-Path node_modules)) { npm install }
  npx expo run:android
} finally {
  Pop-Location
}
