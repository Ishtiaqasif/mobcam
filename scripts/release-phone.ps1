# Builds the signed release APK into dist\MobCam-<version>.apk (JS bundled in, no Metro needed).
#   .\scripts\release-phone.ps1            build
#   .\scripts\release-phone.ps1 -Install   build and install on the USB-connected phone
#
# Signing key: %USERPROFILE%\.mobcam\mobcam-release.jks (created on first run, password in
# keystore.properties next to it). Back both up: updates must be signed with the same key.
param([switch]$Install)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'android-env.ps1')
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$app = Join-Path $root 'app'

# ---- signing key (outside the repo)
$keyDir = Join-Path $env:USERPROFILE '.mobcam'
$keystore = Join-Path $keyDir 'mobcam-release.jks'
$props = Join-Path $keyDir 'keystore.properties'
if (-not (Test-Path $keystore)) {
  New-Item -ItemType Directory -Force $keyDir | Out-Null
  $password = -join ((48..57) + (65..90) + (97..122) | Get-Random -Count 32 | ForEach-Object { [char]$_ })
  & "$env:JAVA_HOME\bin\keytool.exe" -genkeypair -keystore $keystore -storetype PKCS12 -alias mobcam `
    -keyalg RSA -keysize 2048 -validity 10000 -dname 'CN=MobCam' -storepass $password -keypass $password 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'keytool failed' }
  "password=$password" | Set-Content $props -Encoding ascii
  Write-Host "Created signing key $keystore (back it up together with keystore.properties)" -ForegroundColor Yellow
}
$password = ((Get-Content $props | Where-Object { $_ -like 'password=*' }) -split '=', 2)[1]

$version = (Get-Content (Join-Path $app 'app.json') -Raw | ConvertFrom-Json).expo.version

Push-Location $app
try {
  if (-not (Test-Path node_modules)) { npm install }
  # Regenerate android/ from app.json (icons, version, permissions). It is generated, never edited.
  $env:CI = '1'
  npx expo prebuild --platform android --no-install
  if ($LASTEXITCODE -ne 0) { throw 'expo prebuild failed' }

  Push-Location android
  try {
    .\gradlew.bat assembleRelease --console=plain `
      "-PreactNativeArchitectures=arm64-v8a" `
      "-Pandroid.injected.signing.store.file=$keystore" `
      "-Pandroid.injected.signing.store.password=$password" `
      "-Pandroid.injected.signing.key.alias=mobcam" `
      "-Pandroid.injected.signing.key.password=$password"
    if ($LASTEXITCODE -ne 0) { throw 'gradle assembleRelease failed' }
  } finally {
    Pop-Location
  }
} finally {
  Pop-Location
}

$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
$apk = Join-Path $dist "MobCam-$version.apk"
Copy-Item (Join-Path $app 'android\app\build\outputs\apk\release\app-release.apk') $apk -Force
Write-Host "`nAPK: $apk" -ForegroundColor Green

if ($Install) {
  adb install -r $apk
  if ($LASTEXITCODE -ne 0) {
    Write-Host 'Install failed. If a development build is installed, remove it first: adb uninstall com.mobcam.app' -ForegroundColor Yellow
  }
}
