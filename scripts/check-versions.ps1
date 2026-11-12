# Verifies that the PC and phone versions agree, and (with -Tag) that a release tag matches them
# and that CHANGELOG.md has a section for it. Used by CI and by the release workflow.
#   ./scripts/check-versions.ps1               -> versions in csproj and app.json are equal
#   ./scripts/check-versions.ps1 -Tag v1.1.0   -> ...and equal to the tag, with release notes
param([string]$Tag)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')

$csproj = Get-Content (Join-Path $root 'pc/gui/MobCam.csproj') -Raw
$pcVersion = [regex]::Match($csproj, '<Version>([^<]+)</Version>').Groups[1].Value
$app = Get-Content (Join-Path $root 'app/app.json') -Raw | ConvertFrom-Json
$phoneVersion = $app.expo.version
$versionCode = $app.expo.android.versionCode

$problems = @()
if (-not $pcVersion) { $problems += 'pc/gui/MobCam.csproj has no <Version>' }
if ($pcVersion -ne $phoneVersion) {
  $problems += "Version mismatch: pc/gui/MobCam.csproj = $pcVersion, app/app.json = $phoneVersion"
}
if (-not ($versionCode -is [int] -or $versionCode -is [long]) -or $versionCode -lt 1) {
  $problems += "app/app.json android.versionCode must be a positive integer (is '$versionCode')"
}

if ($Tag) {
  if ($Tag -ne "v$pcVersion") { $problems += "Tag $Tag does not match the version in the code (v$pcVersion)" }
  $changelog = Get-Content (Join-Path $root 'CHANGELOG.md') -Raw
  if ($changelog -notmatch "(?m)^## \[$([regex]::Escape($pcVersion))\]") {
    $problems += "CHANGELOG.md has no '## [$pcVersion]' section (run scripts/bump-version.ps1)"
  }
}

if ($problems) {
  $problems | ForEach-Object { Write-Host "::error::$_" }
  exit 1
}
Write-Host "Versions OK: $pcVersion (versionCode $versionCode)$(if ($Tag) { ", tag $Tag" })"
