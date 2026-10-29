# Downloads the official OpenCV Windows release (a 7-zip self-extractor) and unpacks the pieces rigmind-pipeline needs into
# third_party\opencv\ (include\opencv2, lib\opencv_world*.lib, bin\opencv_world*.dll). Used for the platform mask overlay
# (resize, Canny, findContours). The binaries are not committed (.gitignore).
#
#   powershell -ExecutionPolicy Bypass -File scripts\fetch_opencv.ps1 [-Version 4.10.0]
param([string]$Version = "4.10.0")
$ErrorActionPreference = "Stop"

$target = Join-Path $PSScriptRoot "..\third_party\opencv"
$stamp = Join-Path $target "VERSION"
if ((Test-Path $stamp) -and ((Get-Content $stamp -Raw).Trim() -eq $Version) -and (Get-ChildItem (Join-Path $target "bin") -Filter "opencv_world*.dll" -ErrorAction SilentlyContinue)) {
    Write-Host "opencv $Version already present in $target"
    exit 0
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("opencv-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    $installer = Join-Path $work "opencv.exe"
    Invoke-WebRequest -UseBasicParsing -Uri "https://github.com/opencv/opencv/releases/download/$Version/opencv-$Version-windows.exe" -OutFile $installer
    $extracted = Join-Path $work "x"
    & $installer "-o$extracted" -y | Out-Null
    $build = Join-Path $extracted "opencv\build"
    $lib = Get-ChildItem (Join-Path $build "x64\vc16\lib") -Filter "opencv_world*.lib" | Where-Object { $_.Name -notmatch "d\.lib$" } | Select-Object -First 1
    $dll = Get-ChildItem (Join-Path $build "x64\vc16\bin") -Filter "opencv_world*.dll" | Where-Object { $_.Name -notmatch "d\.dll$" } | Select-Object -First 1
    if (-not (Test-Path (Join-Path $build "include\opencv2")) -or -not $lib -or -not $dll) { throw "package layout changed under $build" }

    if (Test-Path $target) { Remove-Item -Recurse -Force $target }
    New-Item -ItemType Directory -Path (Join-Path $target "lib"), (Join-Path $target "bin") | Out-Null
    Copy-Item -Recurse (Join-Path $build "include") (Join-Path $target "include")
    Copy-Item $lib.FullName (Join-Path $target "lib")
    Copy-Item $dll.FullName (Join-Path $target "bin")
    Set-Content -Path $stamp -Value $Version -Encoding ascii
    Write-Host "opencv $Version installed in $target"
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
