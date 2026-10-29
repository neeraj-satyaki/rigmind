# Builds the RigMind Windows bundle: native host (Win32 + WebView2), rigmind-pipeline, BitNet SLM, NSIS installer.
# Run on Windows with Visual Studio 2022 (C++ workload), CMake, NSIS, and GStreamer MSVC SDK
# (set GSTREAMER_1_0_ROOT_MSVC_X86_64 and put pkg-config on PATH). Not verified on this build host.
#
# Usage (from RigMind_Desktop):  powershell -ExecutionPolicy Bypass -File scripts\package_windows.ps1
$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$Build = Join-Path $Root "build"

# 1. Web bundle: run scripts/package_linux.sh (or copy Rigmind_v2 build output) so web\ is current.
if (-not (Test-Path (Join-Path $Root "web\index.html"))) { throw "web\index.html missing: build the SPA first" }

# 1b. BitNet runtime and model for plain-language descriptions (staged for the installer's slm\ folder).
$Bitnet = Join-Path $Root "third_party\bitnet.cpp\build\bin\Release"
$Model = Join-Path $Root "third_party\bitnet-model\ggml-model-i2_s.gguf"
if (-not (Test-Path (Join-Path $Bitnet "llama-server.exe"))) { throw "BitNet llama-server.exe missing: build third_party\bitnet.cpp first" }
if (-not (Test-Path $Model)) { throw "BitNet model missing: $Model" }
$SlmStage = Join-Path $Root "slm-windows"
Remove-Item -Recurse -Force $SlmStage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $SlmStage | Out-Null
Copy-Item (Join-Path $Bitnet "llama-server.exe") $SlmStage
Get-ChildItem $Bitnet -Filter *.dll | Copy-Item -Destination $SlmStage
Copy-Item $Model $SlmStage

# 2. Configure and build (Release).
cmake -S $Root -B $Build -G "Visual Studio 17 2022" -A x64
cmake --build $Build --config Release

# 3. Unit tests.
ctest --test-dir $Build -C Release --output-on-failure

# 4. Installer and portable zip.
Push-Location $Build
cpack -C Release -G NSIS
cpack -C Release -G ZIP
Pop-Location

Write-Host "Installer and zip written to $Build"
