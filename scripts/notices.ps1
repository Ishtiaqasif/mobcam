# Writes THIRD-PARTY-NOTICES.txt: the licences of everything a MobCam release ships (issue #7).
#   .\scripts\notices.ps1 -Target phone                       (after `expo prebuild`)
#       -> app/modules/mobcam-stream/android/src/main/assets/THIRD-PARTY-NOTICES.txt (in the APK)
#   .\scripts\notices.ps1 -Target pc -PublishDir pc\build\publish   (after `dotnet publish`)
#       -> <PublishDir>\THIRD-PARTY-NOTICES.txt (installed next to MobCam.exe)
#
# Phone: the production npm packages (JS bundle and autolinked native modules) and the Android
# libraries of the release runtime classpath, with licences read from their Maven POMs.
# PC: the self-contained .NET and WPF runtime found in the publish folder; the native PC code
# uses only Windows APIs.
#
# The release scripts and the Release workflow run this before packaging, so the notices always
# match what ships. An Android library with a licence it can't place makes it fail: add the text
# to scripts/licenses and to $MavenTexts below.
param(
  [Parameter(Mandatory)][ValidateSet('phone', 'pc')][string]$Target,
  [string]$PublishDir,
  [string]$Out
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$app = Join-Path $root 'app'
$licenseDir = Join-Path $PSScriptRoot 'licenses'
$sb = [System.Text.StringBuilder]::new()
$heavy = '=' * 78
$light = '-' * 78

function Add([string]$text = '') { [void]$sb.Append($text).Append("`n") }
function Tidy([string]$t) { ($t -replace "`r`n", "`n").Trim("`n", ' ', "`t") }
function Read-Text([string]$path) { Tidy ([IO.File]::ReadAllText($path)) }
function Section([string]$title, [string]$intro) {
  Add; Add $heavy; Add $title; Add $heavy
  if ($intro) { Add; Add $intro }
}
# One block per distinct licence text, listing every component that uses it.
function Add-Groups($items) {
  foreach ($g in $items | Group-Object Text | Sort-Object { $_.Group[0].Name }) {
    Add; Add $light
    foreach ($i in $g.Group | Sort-Object Name) { Add "$($i.Name)  ($($i.License))" }
    Add $light; Add; Add $g.Name
  }
}

# Licence texts for Android libraries whose Maven artifacts don't include one, by group prefix.
$MavenTexts = @(
  @{ Prefix = 'com.facebook.'; License = 'MIT'; File = Join-Path $app 'node_modules/react-native/LICENSE' }
  @{ Prefix = 'io.github.expo'; License = 'MIT'; File = Join-Path $app 'node_modules/expo/LICENSE' }
  @{ Prefix = 'com.parse.bolts'; License = 'BSD'; File = Join-Path $licenseDir 'Bolts-BSD.txt' }
  @{ Prefix = 'org.brotli'; License = 'MIT'; File = Join-Path $licenseDir 'Brotli-MIT.txt' }
)

# Native code compiled into the prebuilt Android libraries above (lib/*.so in the APK).
$NativeLibraries = @'
React Native, Fresco and okio-zstd ship prebuilt native libraries that are compiled together
with these projects, each under its own licence:

- Hermes JavaScript engine (libhermesvm.so): MIT, Meta Platforms, Inc. and affiliates,
  same licence text as React Native. https://github.com/facebook/hermes
- Folly: Apache-2.0. https://github.com/facebook/folly
- glog: BSD-3-Clause, Google Inc. https://github.com/google/glog
- fmt: MIT, Victor Zverovich and fmt contributors. https://github.com/fmtlib/fmt
- Boost: BSL-1.0. https://www.boost.org/LICENSE_1_0.txt
- double-conversion: BSD-3-Clause, the V8 project authors. https://github.com/google/double-conversion
- fast_float: Apache-2.0 / MIT / BSL-1.0. https://github.com/fastfloat/fast_float
- LLVM libc++ (libc++_shared.so): Apache-2.0 WITH LLVM-exception. https://llvm.org/LICENSE.txt
- giflib (libgifimage.so): MIT. https://giflib.sourceforge.net/
- libjpeg-turbo (libnative-imagetranscoder.so): IJG, BSD-3-Clause and Zlib.
  https://github.com/libjpeg-turbo/libjpeg-turbo/blob/main/LICENSE.md
- libwebp (libstatic-webp.so): BSD-3-Clause, Google Inc. https://chromium.googlesource.com/webm/libwebp/+/HEAD/COPYING
- Zstandard (libzstd-kmp.so): BSD-3-Clause, Meta Platforms, Inc. and affiliates. https://github.com/facebook/zstd
'@

function Add-Header([string]$what, [string]$version) {
  Add "MobCam $version ($what): third-party notices"
  Add
  Add 'MobCam is free software under the MIT License (below). It includes the third-party'
  Add 'software listed in this file, each used under the licence shown with it.'
  Add 'Source code and issues: https://github.com/Ishtiaqasif/mobcam'
  Section 'MobCam'
  Add; Add (Read-Text (Join-Path $root 'LICENSE'))
}

# ------------------------------------------------------------------ phone: npm packages
function Add-NpmPackages {
  Push-Location $app
  try {
    # Exits non-zero on harmless peer-dependency warnings; the list is still complete.
    $paths = npm ls --omit=dev --all --parseable 2>$null
  } finally { Pop-Location }
  if (-not $paths) { throw 'npm ls returned nothing; run npm install (or npm ci) in app/ first' }

  $seen = @{}; $items = @(); $noFile = @()
  foreach ($dir in $paths | Sort-Object -Unique) {
    if (-not $dir -or ((Resolve-Path $dir).Path -eq (Resolve-Path $app).Path)) { continue }
    $manifest = Join-Path $dir 'package.json'
    if (-not (Test-Path $manifest)) { continue }
    $pkg = Get-Content $manifest -Raw | ConvertFrom-Json -AsHashtable
    $name = "$($pkg.name) $($pkg.version)"
    if ($seen.ContainsKey($name)) { continue }
    $seen[$name] = $true
    $license = switch ($pkg.license) {
      { $_ -is [string] } { $_ }
      { $_ -is [hashtable] } { $_.type }
      default { if ($pkg.licenses) { ($pkg.licenses | ForEach-Object { if ($_ -is [hashtable]) { $_.type } else { $_ } }) -join ' OR ' } else { 'see package' } }
    }
    $file = Get-ChildItem $dir -File | Where-Object Name -match '^(licen[cs]e|copying)([.-]|$)' |
      Sort-Object Name | Select-Object -First 1
    if ($file) { $items += [pscustomobject]@{ Name = $name; License = $license; Text = Read-Text $file.FullName } }
    else { $noFile += "$name  ($license)" }
  }

  Section 'JavaScript and Expo packages (npm)' `
    "The phone app's JavaScript bundle and native modules are built from these $($seen.Count) packages."
  Add-Groups $items
  if ($noFile) {
    Add; Add $light
    Add 'These packages include no licence file; their package.json declares the licence shown:'
    Add $light
    $noFile | Sort-Object | ForEach-Object { Add $_ }
  }
}

# ------------------------------------------------------------------ phone: Android libraries
function Get-AndroidLibraries {
  $android = Join-Path $app 'android'
  $gradlew = Join-Path $android ($IsWindows ? 'gradlew.bat' : 'gradlew')
  if (-not (Test-Path $gradlew)) { throw "app/android is missing: run 'npx expo prebuild --platform android' first" }
  Push-Location $android
  try {
    $tree = & $gradlew :app:dependencies --configuration releaseRuntimeClasspath --console=plain -q
    if ($LASTEXITCODE -ne 0) { throw 'gradle dependencies failed' }
  } finally { Pop-Location }

  $coords = [ordered]@{}
  foreach ($line in $tree) {
    # "+--- group:name:1.0 -> 1.2 (*)"; (c) = constraint and (n) = not resolved: no artifact.
    if ($line -match '--- ([\w.\-]+):([\w.\-]+)(?::([\w.\-]+))?(?: -> ([\w.\-]+))?(?: \(([*cn])\))?\s*$') {
      if ($Matches[5] -in 'c', 'n') { continue }
      $version = $Matches[4] ? $Matches[4] : $Matches[3]
      $coords["$($Matches[1]):$($Matches[2]):$version"] = $true
    }
  }
  if ($coords.Count -eq 0) { throw 'No Android libraries found in the gradle dependency tree' }
  $coords.Keys
}

function Read-Pom([string]$cache, [string]$group, [string]$name, [string]$version) {
  $dir = Join-Path $cache "$group/$name/$version"
  $pom = Get-ChildItem $dir -Recurse -Filter "$name-$version.pom" -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($pom) { [xml](Get-Content $pom.FullName -Raw) }
}

function Add-AndroidLibraries {
  $gradleHome = $env:GRADLE_USER_HOME ? $env:GRADLE_USER_HOME : (Join-Path ([Environment]::GetFolderPath('UserProfile')) '.gradle')
  $cache = Join-Path $gradleHome 'caches/modules-2/files-2.1'
  $apache = Read-Text (Join-Path $licenseDir 'Apache-2.0.txt')
  $items = @()
  foreach ($c in Get-AndroidLibraries) {
    $group, $name, $version = $c -split ':'
    # Expo modules come prebuilt from their npm packages, which are listed above.
    if ($group -eq 'host.exp.exponent' -or $group -like 'expo.modules.*') { continue }

    # Licences from the POM, or from its parent POMs.
    $licenses = @(); $g = $group; $n = $name; $v = $version
    for ($depth = 0; $depth -lt 5 -and -not $licenses; $depth++) {
      $pom = Read-Pom $cache $g $n $v
      if (-not $pom) { break }
      $licenses = @($pom.project.licenses.license | Where-Object { $_ -and ($_.name -or $_.url) })
      if (-not $licenses -and $pom.project.parent) {
        $g = $pom.project.parent.groupId; $n = $pom.project.parent.artifactId; $v = $pom.project.parent.version
      } elseif (-not $licenses) { break }
    }
    if (-not $licenses) { throw "No licence found for $c (no POM in the Gradle cache, or none declared)" }

    $declared = ($licenses | ForEach-Object { "$($_.name) $($_.url)" }) -join ' '
    $label = if ($declared -match '(?i)apache') { 'Apache-2.0' } elseif ($declared -match '(?i)\bmit\b') { 'MIT' }
             elseif ($declared -match '(?i)\bbsd\b') { 'BSD' } else { $declared.Trim() }
    if ($label -eq 'Apache-2.0') {
      $text = $apache
    } else {
      $known = $MavenTexts | Where-Object { $group.StartsWith($_.Prefix) -and $_.License -eq $label } | Select-Object -First 1
      if (-not $known) {
        throw "No licence text for $c ($label). Add it to scripts/licenses and to `$MavenTexts in notices.ps1."
      }
      $text = Read-Text $known.File
    }
    $items += [pscustomobject]@{ Name = "$group`:$name $version"; License = $label; Text = $text }
  }

  Section 'Android libraries (Maven)' "Libraries in the phone app's release build ($($items.Count))."
  Add-Groups $items
  Section 'Native code inside the Android libraries'
  Add; Add (Tidy $NativeLibraries)
}

# ------------------------------------------------------------------ PC: .NET runtime
function Add-DotnetRuntime([string]$publish) {
  $deps = Get-ChildItem $publish -Filter '*.deps.json' | Select-Object -First 1
  if (-not $deps) { throw "No .deps.json in $publish; pass the dotnet publish folder" }
  $json = Get-Content $deps.FullName -Raw | ConvertFrom-Json -AsHashtable
  $packs = $json.libraries.Keys | Where-Object { $_ -like 'runtimepack.*' }
  if (-not $packs) { throw "$($deps.Name) has no runtime packs: publish with --self-contained true" }
  $nuget = $env:NUGET_PACKAGES ? $env:NUGET_PACKAGES : (Join-Path ([Environment]::GetFolderPath('UserProfile')) '.nuget/packages')

  Section '.NET runtime' 'MobCam.exe includes the .NET and Windows Desktop (WPF, Windows Forms) runtime.'
  foreach ($pack in $packs | Sort-Object) {
    $id, $version = $pack.Substring('runtimepack.'.Length) -split '/'
    $dir = Join-Path $nuget "$($id.ToLowerInvariant())/$version"
    if (-not (Test-Path $dir)) { throw "Runtime pack $id $version not found in $nuget" }
    $files = Get-ChildItem $dir -File | Where-Object Name -match '^(licen[cs]e|third-party-notices)' | Sort-Object Name
    if (-not ($files | Where-Object Name -match '^licen')) { throw "No licence file in $dir" }
    foreach ($f in $files) {
      Add; Add $light; Add "$id $version  ($($f.Name))"; Add $light; Add
      Add (Read-Text $f.FullName)
    }
  }
}

# ------------------------------------------------------------------ main
if ($Target -eq 'phone') {
  $version = (Get-Content (Join-Path $app 'app.json') -Raw | ConvertFrom-Json).expo.version
  if (-not $Out) { $Out = Join-Path $app 'modules/mobcam-stream/android/src/main/assets/THIRD-PARTY-NOTICES.txt' }
  Add-Header 'Android app' $version
  Add-NpmPackages
  Add-AndroidLibraries
} else {
  if (-not $PublishDir) { throw '-PublishDir is required for -Target pc' }
  $version = ([xml](Get-Content (Join-Path $root 'pc/gui/MobCam.csproj'))).Project.PropertyGroup.Version | Select-Object -First 1
  if (-not $Out) { $Out = Join-Path $PublishDir 'THIRD-PARTY-NOTICES.txt' }
  Add-Header 'Windows app' $version
  Add-DotnetRuntime $PublishDir
}

New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
[IO.File]::WriteAllText($Out, $sb.ToString(), [Text.UTF8Encoding]::new($false))
Write-Host ("Notices: {0} ({1:N0} KB)" -f $Out, ((Get-Item $Out).Length / 1KB))
