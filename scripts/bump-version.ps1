# Sets the release version everywhere in one step:
#   - pc/gui/MobCam.csproj <Version>
#   - app/app.json expo.version, and android.versionCode + 1 (Android needs it to increase)
#   - CHANGELOG.md: "## [Unreleased]" becomes "## [<version>] - <date>" with a fresh empty
#     [Unreleased] section above it, and the compare links are updated
#
#   npm run bump -- 1.1.0          (or ./scripts/bump-version.ps1 1.1.0)
#   npm run bump -- 1.2.0-rc.1     pre-release versions are published as GitHub pre-releases
param([Parameter(Mandatory = $true, Position = 0)][string]$Version)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$repoUrl = 'https://github.com/Ishtiaqasif/mobcam'

if ($Version -notmatch '^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$') {
  throw "'$Version' is not a version like 1.2.0 or 1.2.0-rc.1"
}

# ---- CHANGELOG first: refuse to release without notes
$changelogPath = Join-Path $root 'CHANGELOG.md'
$changelog = Get-Content $changelogPath -Raw
if ($changelog -notmatch '(?m)^## \[Unreleased\]\s*$') {
  throw 'CHANGELOG.md has no "## [Unreleased]" section. Add the changes for this release there first.'
}
if ($changelog -match "(?m)^## \[$([regex]::Escape($Version))\]") { throw "CHANGELOG.md already has a section for $Version" }
# Body of [Unreleased]: everything up to the next "## [" heading or the link definitions.
$unreleased = [regex]::Match($changelog, '(?ms)^## \[Unreleased\][ \t]*\r?\n(.*?)(?=^## \[|^\[[^\]]+\]: |\z)').Groups[1].Value.Trim()
if (-not $unreleased) { throw 'The [Unreleased] section in CHANGELOG.md is empty. Describe the changes first.' }

$date = Get-Date -Format 'yyyy-MM-dd'
$nl = if ($changelog.Contains("`r`n")) { "`r`n" } else { "`n" }
$changelog = [regex]::Replace($changelog, '(?m)^## \[Unreleased\]\s*$', "## [Unreleased]$nl$nl## [$Version] - $date", 1)
$unreleasedLink = "[Unreleased]: $repoUrl/compare/v$Version...dev"
$versionLink = "[$Version]: $repoUrl/releases/tag/v$Version"
if ($changelog -match '(?m)^\[Unreleased\]: .*$') {
  $changelog = [regex]::Replace($changelog, '(?m)^\[Unreleased\]: .*$', "$unreleasedLink$nl$versionLink", 1)
} elseif ($changelog -match '(?m)^\[[^\]]+\]: ') {
  $changelog = [regex]::Replace($changelog, '(?m)^(\[[^\]]+\]: )', "$unreleasedLink$nl$versionLink$nl`$1", 1)
} else {
  $changelog = $changelog.TrimEnd() + "$nl$nl$unreleasedLink$nl$versionLink$nl"
}

# ---- versions (regex edits keep the files' formatting)
$csprojPath = Join-Path $root 'pc/gui/MobCam.csproj'
$csproj = Get-Content $csprojPath -Raw
if ($csproj -notmatch '<Version>[^<]+</Version>') { throw 'No <Version> in pc/gui/MobCam.csproj' }
$csproj = [regex]::Replace($csproj, '<Version>[^<]+</Version>', "<Version>$Version</Version>", 1)

$appPath = Join-Path $root 'app/app.json'
$appJson = Get-Content $appPath -Raw
$oldCode = [int]($appJson | ConvertFrom-Json).expo.android.versionCode
$newCode = $oldCode + 1
$appJson = [regex]::Replace($appJson, '"version":\s*"[^"]*"', "`"version`": `"$Version`"", 1)
$appJson = [regex]::Replace($appJson, '"versionCode":\s*\d+', "`"versionCode`": $newCode", 1)

Set-Content $changelogPath $changelog -NoNewline -Encoding utf8
Set-Content $csprojPath $csproj -NoNewline -Encoding utf8
Set-Content $appPath $appJson -NoNewline -Encoding utf8

& (Join-Path $PSScriptRoot 'check-versions.ps1') -Tag "v$Version"
Write-Host ""
Write-Host "Bumped to $Version (Android versionCode $oldCode -> $newCode). Next:" -ForegroundColor Green
Write-Host "  1. Commit and push to dev:  git commit -am `"Release $Version`"; git push"
Write-Host "  2. Open and merge a pull request dev -> main (merge commit)"
Write-Host "  3. Tag main:                git switch main; git pull; git tag v$Version; git push origin v$Version"
Write-Host "     GitHub Actions then builds, signs and publishes the release."
