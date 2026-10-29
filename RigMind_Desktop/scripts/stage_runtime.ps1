# Stages everything the installer must carry besides RigMind's own binaries, into RigMind_Desktop\runtime-windows\:
#   gst\bin, gst\lib\gstreamer-1.0, gst\libexec\gstreamer-1.0   GStreamer runtime and only the plugins the pipeline uses
#   vcrt\                                                       Visual C++ runtime DLLs (app-local; no redistributable install)
#   redist\MicrosoftEdgeWebview2Setup.exe                       WebView2 bootstrapper, run by the installer when the runtime is missing
# Needs the GStreamer MSVC x64 runtime (GSTREAMER_1_0_ROOT_MSVC_X86_64) and Visual Studio 2022 with the C++ workload.
# Usage (from RigMind_Desktop):  powershell -ExecutionPolicy Bypass -File scripts\stage_runtime.ps1
$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$Out = Join-Path $Root "runtime-windows"
$Gst = $env:GSTREAMER_1_0_ROOT_MSVC_X86_64
if (-not $Gst) { $Gst = "C:\gstreamer\1.0\msvc_x86_64" }
if (-not (Test-Path (Join-Path $Gst "bin\gstreamer-1.0-0.dll"))) { throw "GStreamer runtime not found at $Gst" }

Remove-Item -Recurse -Force $Out -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$Out\gst\bin", "$Out\gst\lib\gstreamer-1.0", "$Out\gst\libexec\gstreamer-1.0", "$Out\vcrt", "$Out\redist" | Out-Null

# 1. GStreamer core DLLs: all of bin\ (the plugins' own dependencies are spread across them; the set is stable).
Copy-Item (Join-Path $Gst "bin\*.dll") "$Out\gst\bin"

# 2. Plugins the pipeline uses: RTSP in, RTP, WebRTC (ICE/DTLS/SRTP), H.264 parse, GPU decode (D3D11), encoders
#    (Media Foundation, Intel QSV, NVIDIA, OpenH264 as last resort), colour/scale/rate, MP4 recording, typefinding.
$plugins = 'coreelements','rtsp','rtp','rtpmanager','videoparsersbad','d3d11','mediafoundation','qsv','nvcodec','openh264',
           'videoconvertscale','videorate','isomp4','multifile','app','webrtc','nice','dtls','srtp','sdpelem','udp','tcp',
           'videotestsrc','playback','typefindfunctions','rtpmanager','audioparsers','opus','audioconvert','audioresample','volume'
foreach ($name in ($plugins | Select-Object -Unique)) {
    $file = Join-Path $Gst "lib\gstreamer-1.0\gst$name.dll"
    if (Test-Path $file) { Copy-Item $file "$Out\gst\lib\gstreamer-1.0" } else { Write-Warning "plugin not found: gst$name.dll" }
}
$scanner = Join-Path $Gst "libexec\gstreamer-1.0\gst-plugin-scanner.exe"
if (Test-Path $scanner) { Copy-Item $scanner "$Out\gst\libexec\gstreamer-1.0" }

# 3. Visual C++ runtime, app-local.
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$crt = Get-ChildItem (Join-Path $vs "VC\Redist\MSVC") -Recurse -Directory -Filter "Microsoft.VC*.CRT" |
       Where-Object { $_.FullName -match "\\x64\\" } | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $crt) { throw "Visual C++ runtime folder not found under $vs" }
Copy-Item (Join-Path $crt.FullName "*.dll") "$Out\vcrt"

# 4. WebView2 evergreen bootstrapper (Microsoft's own download; ~2 MB).
$bootstrapper = "$Out\redist\MicrosoftEdgeWebview2Setup.exe"
Invoke-WebRequest -Uri "https://go.microsoft.com/fwlink/p/?LinkId=2124703" -OutFile $bootstrapper
$sig = Get-AuthenticodeSignature $bootstrapper
if ($sig.Status -ne "Valid" -or $sig.SignerCertificate.Subject -notmatch "Microsoft") { throw "WebView2 bootstrapper signature check failed: $($sig.Status)" }

$size = (Get-ChildItem $Out -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
"Staged {0:N0} MB in {1}" -f $size, $Out
