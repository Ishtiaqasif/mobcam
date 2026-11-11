# Builds everything for the PC side into pc\build\bin:
#   MobCam.exe (app), mobcam-core.dll, mobcam-cli.exe, mobcam-source.dll (virtual camera)
# Usage:  .\build.ps1            (Release)
#         .\build.ps1 -Run       (build, then start the app)
param([switch]$Run)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -products * -latest -property installationPath
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) { $cmake = 'cmake' }

# A running app locks its DLLs.
Get-Process MobCam, mobcam-cli -ErrorAction SilentlyContinue | Stop-Process -Force

& $cmake -S $root -B "$root\build" -G "Visual Studio 17 2022" -A x64 | Out-Null
& $cmake --build "$root\build" --config Release
if ($LASTEXITCODE -ne 0) { throw "native build failed" }

dotnet build "$root\gui\MobCam.csproj" -c Release -o "$root\build\bin" --nologo -v quiet
if ($LASTEXITCODE -ne 0) { throw "app build failed" }

Write-Host "`nBuilt into $root\build\bin" -ForegroundColor Green
if ($Run) { Start-Process "$root\build\bin\MobCam.exe" }
