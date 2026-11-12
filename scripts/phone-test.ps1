# Runs the phone's JVM unit tests (app/modules/mobcam-stream/android/src/test): SEI bytes, lens
# rules and protocol v2 against the fixtures shared with the PC (tests/fixtures).
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'android-env.ps1')
$app = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..')) 'app'

Push-Location $app
try {
  if (-not (Test-Path node_modules)) { npm install }
  if (-not (Test-Path android)) {
    $env:CI = '1'
    npx expo prebuild --platform android --no-install
    if ($LASTEXITCODE -ne 0) { throw 'expo prebuild failed' }
  }
  Push-Location android
  try {
    .\gradlew.bat :mobcam-stream:testDebugUnitTest --console=plain
    if ($LASTEXITCODE -ne 0) { throw 'phone unit tests failed (report: app/modules/mobcam-stream/android/build/reports/tests)' }
  } finally { Pop-Location }
} finally { Pop-Location }
