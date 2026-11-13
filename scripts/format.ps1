# Formats (or checks) all source code with each language's standard tool (issue #13):
#   C++         clang-format 19   (.clang-format)       pc/**/*.cpp|h, not pc/third_party
#   C#          dotnet format     (.editorconfig)       pc/gui
#   Kotlin      ktlint 1.8.0      (.editorconfig)       app/modules/mobcam-stream/android/src
#   TypeScript  ESLint + Prettier (app/eslint.config.js, app/.prettierrc.json)
#
#   .\scripts\format.ps1           fix everything   (npm run format)
#   .\scripts\format.ps1 -Check    only report, exit 1 if anything is off (npm run lint, CI "Lint" job)
#
# Needs: Visual Studio's clang-format or `python -m pip install clang-format==19.1.5`, the .NET SDK,
# a JDK 17 (ktlint is downloaded to .tools on first use) and `npm install` in app/.
param([switch]$Check)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$failed = [System.Collections.Generic.List[string]]::new()

function Step($name, [scriptblock]$run) {
  Write-Host "`n== $name" -ForegroundColor Cyan
  & $run
  if ($LASTEXITCODE -ne 0) { $failed.Add($name) }
}

Push-Location $root
try {
  # ---- C++
  $clang = $env:CLANG_FORMAT
  if (-not $clang) { $clang = (Get-Command clang-format -ErrorAction SilentlyContinue).Source }
  if (-not $clang) {
    $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -products * -latest -property installationPath 2>$null
    if ($vs) { $clang = Join-Path $vs 'VC\Tools\Llvm\x64\bin\clang-format.exe' }
    if ($vs -and -not (Test-Path $clang)) { $clang = Join-Path $vs 'VC\Tools\Llvm\bin\clang-format.exe' }
  }
  if (-not $clang -or -not (Test-Path $clang)) { throw 'clang-format not found. Install it with: python -m pip install clang-format==19.1.5' }
  $clangVersion = & $clang --version
  # Different major versions format slightly differently; CI pins 19.1.5.
  if ($clangVersion -notmatch 'version 19\.') { throw "Need clang-format 19 (CI uses 19.1.5), found: $clangVersion" }
  $cpp = git ls-files 'pc/*.cpp' 'pc/*.h' | Where-Object { $_ -notlike 'pc/third_party/*' }
  Step "C++ ($($cpp.Count) files, $clangVersion)" {
    if ($Check) { & $clang --dry-run --Werror --style=file $cpp } else { & $clang -i --style=file $cpp }
  }

  # ---- C#
  Step 'C# (dotnet format)' {
    if ($Check) { dotnet format pc/gui/MobCam.csproj --verify-no-changes --verbosity minimal }
    else { dotnet format pc/gui/MobCam.csproj --verbosity minimal }
  }

  # ---- Kotlin
  $ktlintVersion = '1.8.0'
  $ktlintSha256 = 'a3fd620207d5c40da6ca789b95e7f823c54e854b7fade7f613e91096a3706d75'  # from the GitHub release
  $ktlint = Join-Path $root ".tools\ktlint-$ktlintVersion.jar"
  if (-not (Test-Path $ktlint)) {
    New-Item -ItemType Directory -Force (Split-Path $ktlint) | Out-Null
    Write-Host "Downloading ktlint $ktlintVersion"
    Invoke-WebRequest -UseBasicParsing -OutFile "$ktlint.part" `
      -Uri "https://github.com/pinterest/ktlint/releases/download/$ktlintVersion/ktlint"
    if ((Get-FileHash "$ktlint.part" -Algorithm SHA256).Hash -ne $ktlintSha256) {
      Remove-Item "$ktlint.part"; throw 'ktlint download does not match its SHA-256'
    }
    Move-Item "$ktlint.part" $ktlint
  }
  $java = if ($env:JAVA_HOME -and (Test-Path "$env:JAVA_HOME\bin\java.exe")) { "$env:JAVA_HOME\bin\java.exe" } else {
    $jdk = Get-ChildItem 'C:\Program Files\Microsoft', 'C:\Program Files\Eclipse Adoptium', 'C:\Program Files\Java' `
      -Directory -Filter 'jdk-17*' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($jdk) { Join-Path $jdk.FullName 'bin\java.exe' } else { (Get-Command java -ErrorAction SilentlyContinue).Source }
  }
  if (-not $java) { throw 'Java not found (ktlint needs a JDK 17). Install it with: winget install Microsoft.OpenJDK.17' }
  Step "Kotlin (ktlint $ktlintVersion)" {
    $ktArgs = @('-jar', $ktlint, '--relative', 'app/modules/mobcam-stream/android/src/**/*.kt')
    if (-not $Check) { $ktArgs += '--format' }
    & $java @ktArgs
  }

  # ---- TypeScript
  if (-not (Test-Path 'app/node_modules')) { throw 'Run `npm install` in app/ first' }
  Step 'TypeScript (ESLint + Prettier)' {
    Push-Location app
    try { if ($Check) { npx eslint . --max-warnings 0 } else { npx eslint . --fix } } finally { Pop-Location }
  }
} finally {
  Pop-Location
}

if ($failed.Count) {
  $hint = if ($Check) { ' Run `npm run format` to fix formatting; other problems need a code change.' } else { '' }
  Write-Host "`nFailed: $($failed -join ', ').$hint" -ForegroundColor Red
  exit 1
}
Write-Host "`nAll formatted." -ForegroundColor Green
