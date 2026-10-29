# Copies the built SPA (Rigmind_v2\.output\public, from `vite build --config vite.desktop.config.ts`) into RigMind_Desktop\web,
# the folder the native host serves as https://rigmind.app and the installer packs. The SPA shell becomes index.html and the
# sql.js WASM under __l5e (not emitted by the current build) is kept from the previous web folder.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out  = Join-Path $root '..\Rigmind_v2\.output\public' | Resolve-Path
$web  = Join-Path $root 'web'
$wasm = Join-Path $web '__l5e'
if (-not (Test-Path (Join-Path $out '_shell.html'))) { throw "No build output in ${out} - run vite build first" }
$keep = Join-Path $env:TEMP 'rigmind-l5e'
if (Test-Path $wasm) { Remove-Item $keep -Recurse -Force -ErrorAction SilentlyContinue; Copy-Item $wasm $keep -Recurse }
Get-ChildItem $web -Force | Remove-Item -Recurse -Force
Copy-Item (Join-Path $out '*') $web -Recurse -Force
Move-Item (Join-Path $web '_shell.html') (Join-Path $web 'index.html') -Force
Remove-Item (Join-Path $web '_headers') -Force -ErrorAction SilentlyContinue
if (Test-Path $keep) { Copy-Item $keep $wasm -Recurse -Force }
"web staged: $((Get-ChildItem $web -Recurse -File).Count) files"
