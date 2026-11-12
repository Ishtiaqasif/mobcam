# Builds everything for the PC side into pc\build\bin:
#   MobCam.exe (app), mobcam-core.dll, mobcam-cli.exe, mobcam-source.dll (virtual camera)
# Usage:  .\build.ps1            (Release)
#         .\build.ps1 -Run       (build, then start the app)
#         .\build.ps1 -Test      (build, then run the unit tests)
param([switch]$Run, [switch]$Test)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -products * -latest -property installationPath
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) { $cmake = 'cmake' }

# A running app locks its DLLs.
Get-Process MobCam, mobcam-cli -ErrorAction SilentlyContinue | Stop-Process -Force

& $cmake -S $root -B "$root\build" -A x64 | Out-Null  # newest installed Visual Studio generator
& $cmake --build "$root\build" --config Release
if ($LASTEXITCODE -ne 0) { throw "native build failed" }

dotnet build "$root\gui\MobCam.csproj" -c Release -o "$root\build\bin" --nologo -v quiet
if ($LASTEXITCODE -ne 0) { throw "app build failed" }

Write-Host "`nBuilt into $root\build\bin" -ForegroundColor Green
if ($Test) {
  # mobcam-tests is excluded from the default build (CMakeLists.txt): build it on request only
  & $cmake --build "$root\build" --config Release --target mobcam-tests
  if ($LASTEXITCODE -ne 0) { throw "test build failed" }
  $ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
  if (-not (Test-Path $ctest)) { $ctest = 'ctest' }
  & $ctest --test-dir "$root\build" -C Release --output-on-failure
  if ($LASTEXITCODE -ne 0) { throw "unit tests failed" }
}
if ($Run) { Start-Process "$root\build\bin\MobCam.exe" }
