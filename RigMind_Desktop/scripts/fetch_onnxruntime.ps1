# Downloads an ONNX Runtime package (NuGet, a .nupkg is a zip) and unpacks the pieces rigmind-pipeline needs (include\,
# lib\onnxruntime.lib, bin\*.dll). The binaries are not committed (.gitignore).
#
#   scripts\fetch_onnxruntime.ps1       CPU package -> third_party\onnxruntime      (default 1.24.4)
#   scripts\fetch_onnxruntime.ps1 -Gpu  GPU package -> third_party\onnxruntime-gpu  (default 1.17.3: built for CUDA 11.8 and
#                                       TensorRT 8.6; it also contains the CPU provider)
# CMake uses onnxruntime-gpu when it exists, else onnxruntime.
param([switch]$Gpu, [string]$Version = "")
if (-not $Version) { $Version = if ($Gpu) { "1.17.3" } else { "1.24.4" } }
# The natives live in the .Windows package (1.17+). 1.18 moved to TensorRT 10, which the bundled TensorRT 8.6 does not satisfy.
$package_id = if ($Gpu) { "Microsoft.ML.OnnxRuntime.Gpu.Windows" } else { "Microsoft.ML.OnnxRuntime" }
$ErrorActionPreference = "Stop"

$target = Join-Path $PSScriptRoot ("..\third_party\" + $(if ($Gpu) { "onnxruntime-gpu" } else { "onnxruntime" }))
$stamp = Join-Path $target "VERSION"
if ((Test-Path $stamp) -and ((Get-Content $stamp -Raw).Trim() -eq $Version) -and (Test-Path (Join-Path $target "bin\onnxruntime.dll"))) {
    Write-Host "onnxruntime $Version already present in $target"
    exit 0
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("ort-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    $package = Join-Path $work "ort.zip"
    Invoke-WebRequest -UseBasicParsing -Uri "https://www.nuget.org/api/v2/package/$package_id/$Version" -OutFile $package
    $extracted = Join-Path $work "x"
    Expand-Archive -Path $package -DestinationPath $extracted

    # The GPU.Windows package keeps its headers under buildTransitive (build\native\include is empty there).
    $include = Join-Path $extracted $(if ($Gpu) { "buildTransitive\native\include" } else { "build\native\include" })
    $native = Join-Path $extracted "runtimes\win-x64\native"
    foreach ($need in @($include, (Join-Path $native "onnxruntime.dll"), (Join-Path $native "onnxruntime.lib"))) {
        if (-not (Test-Path $need)) { throw "package layout changed: missing $need" }
    }

    if (Test-Path $target) { Remove-Item -Recurse -Force $target }
    New-Item -ItemType Directory -Path (Join-Path $target "lib"), (Join-Path $target "bin") | Out-Null
    Copy-Item -Recurse $include (Join-Path $target "include")
    Copy-Item (Join-Path $native "onnxruntime.lib") (Join-Path $target "lib")
    # onnxruntime.dll plus its companion DLLs (the CPU package has onnxruntime_providers_shared.dll; the GPU one adds _cuda and _tensorrt).
    Get-ChildItem $native -Filter *.dll | Copy-Item -Destination (Join-Path $target "bin")
    if (Test-Path (Join-Path $extracted "LICENSE")) { Copy-Item (Join-Path $extracted "LICENSE") $target }
    Set-Content -Path $stamp -Value $Version -Encoding ascii
    Write-Host "onnxruntime $Version installed in $target"
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
