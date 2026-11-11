# Builds the Windows installer into dist\MobCam-Setup-<version>.exe
# (self-contained .NET app + native engine + virtual camera). Needs Inno Setup 6:
#   winget install JRSoftware.InnoSetup
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$pc = Join-Path $root 'pc'
$publish = Join-Path $pc 'build\publish'
$version = ([xml](Get-Content (Join-Path $pc 'gui\MobCam.csproj'))).Project.PropertyGroup.Version | Select-Object -First 1

$iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
          "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 not found. Install it with: winget install JRSoftware.InnoSetup' }

# Native binaries (engine, CLI, virtual camera) via the regular build
& (Join-Path $pc 'build.ps1')

# Self-contained app: no .NET install needed on the target PC
if (Test-Path $publish) { Remove-Item $publish -Recurse -Force }
dotnet publish (Join-Path $pc 'gui\MobCam.csproj') -c Release -r win-x64 --self-contained true `
  -p:DebugType=none -o $publish --nologo -v quiet
if ($LASTEXITCODE -ne 0) { throw 'dotnet publish failed' }
foreach ($f in 'mobcam-core.dll', 'mobcam-cli.exe', 'mobcam-source.dll') {
  Copy-Item (Join-Path $pc "build\bin\$f") $publish -Force
}

$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
& $iscc /Q "/DAppVersion=$version" "/DSourceDir=$publish" "/O$dist" (Join-Path $pc 'installer\mobcam.iss')
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup failed' }
Write-Host "`nInstaller: $(Join-Path $dist "MobCam-Setup-$version.exe")" -ForegroundColor Green
