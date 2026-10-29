# Stages the NVIDIA GPU runtime that the installer carries, into RigMind_Desktop\runtime-windows\gpu\:
#   TensorRT 8.6 (nvinfer, plugin, ONNX parser, builder resource), CUDA 11.8 runtime (cudart, cuBLAS, cuBLASLt, cuFFT,
#   cuRAND) and cuDNN 8.9 (inference libraries only). The CMake build requires this folder.
# TensorRT and CUDA come from their installs on this machine; cuDNN is downloaded from NVIDIA's public redistribution
# server and checked against the SHA-256 in NVIDIA's manifest. Every DLL must carry a valid NVIDIA signature (TensorRT,
# cuDNN) or, for the unsigned CUDA toolkit libraries, NVIDIA vendor and version metadata.
# You are responsible for NVIDIA's redistribution terms (CUDA Toolkit, cuDNN and TensorRT licence attachments).
# About 2.5 GB staged; the installer is over 1 GB because of it.
# Usage (from RigMind_Desktop):  pwsh -File scripts\stage_gpu_runtime.ps1
param(
    [string]$TensorRT = "C:\TensorRT-8.6.1.6",
    [string]$Cuda = "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v11.8",
    [string]$CudnnVersion = "8.9.7"
)
$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$Out = Join-Path $Root "runtime-windows\gpu"
$Cache = Join-Path $Root "runtime-windows\.download"
New-Item -ItemType Directory -Force $Out, $Cache | Out-Null
Get-ChildItem $Out -File | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }

# TensorRT and cuDNN DLLs carry an NVIDIA Authenticode signature. The CUDA 11.8 toolkit libraries are not signed at all
# by NVIDIA; for those the file's own vendor and version fields must say NVIDIA and 11.x.
function Copy-Signed($source) {
    if (-not (Test-Path $source)) { throw "missing: $source" }
    $name = Split-Path $source -Leaf
    $sig = Get-AuthenticodeSignature $source
    if ($sig.Status -eq "Valid" -and $sig.SignerCertificate.Subject -match "NVIDIA") {
        Copy-Item $source $Out
        return
    }
    $info = (Get-Item $source).VersionInfo
    if ($sig.Status -eq "NotSigned" -and $info.CompanyName -match "NVIDIA" -and $source -match '\\CUDA\\v11\.') {
        Write-Host ("  {0}: unsigned CUDA 11 toolkit library, vendor '{1}', file version {2}" -f $name, $info.CompanyName, $info.FileVersion)
        Copy-Item $source $Out
        return
    }
    throw "$name`: signature is $($sig.Status), vendor '$($info.CompanyName)', version '$($info.ProductVersion)'"
}

# 1. TensorRT: the runtime set, including both plugin libraries (nvinfer_plugin and the version-compatible nvinfer_vc_plugin).
#    nvinfer_builder_resource builds engines on the target GPU on first use.
foreach ($name in "nvinfer", "nvinfer_plugin", "nvinfer_vc_plugin", "nvinfer_builder_resource", "nvonnxparser") {
    Copy-Signed (Join-Path $TensorRT "lib\$name.dll")
}

# 2. CUDA runtime libraries TensorRT and ONNX Runtime's CUDA/TensorRT providers load.
foreach ($name in "cudart64_110", "cublas64_11", "cublasLt64_11", "cufft64_10", "curand64_10") {
    Copy-Signed (Join-Path $Cuda "bin\$name.dll")
}

# 3. cuDNN: public redistribution archive, verified against NVIDIA's manifest. Inference libraries only (no *_train).
$manifest = Invoke-RestMethod "https://developer.download.nvidia.com/compute/cudnn/redist/redistrib_$CudnnVersion.json"
$entry = $manifest.cudnn.'windows-x86_64'.cuda11
$archive = Join-Path $Cache (Split-Path $entry.relative_path -Leaf)
if (-not (Test-Path $archive) -or (Get-FileHash $archive -Algorithm SHA256).Hash.ToLower() -ne $entry.sha256) {
    Write-Host ("Downloading cuDNN {0} ({1:N0} MB)..." -f $CudnnVersion, ([double]$entry.size / 1MB))
    Invoke-WebRequest -Uri ("https://developer.download.nvidia.com/compute/cudnn/redist/" + $entry.relative_path) -OutFile $archive
}
if ((Get-FileHash $archive -Algorithm SHA256).Hash.ToLower() -ne $entry.sha256) { throw "cuDNN archive checksum mismatch" }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($archive)
try {
    foreach ($e in $zip.Entries) {
        if ($e.FullName -match '/bin/(cudnn[^/]*\.dll)$' -and $e.Name -notmatch 'train') {
            $target = Join-Path $Out $e.Name
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $target, $true)
            $sig = Get-AuthenticodeSignature $target
            if ($sig.Status -ne "Valid" -or $sig.SignerCertificate.Subject -notmatch "NVIDIA") { throw "$($e.Name): signature $($sig.Status)" }
        }
    }
} finally { $zip.Dispose() }

$files = Get-ChildItem $Out -File
"Staged {0} files, {1:N0} MB, in {2}" -f $files.Count, (($files | Measure-Object Length -Sum).Sum / 1MB), $Out
$files | Sort-Object Length -Descending | ForEach-Object { "  {0,-34} {1,6:N0} MB" -f $_.Name, ($_.Length / 1MB) }
