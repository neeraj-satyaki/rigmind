#!/usr/bin/env bash
# Builds the RigMind Linux bundle: SPA from Rigmind_v2 + launcher + installer.
# Output: dist/rigmind-linux-x64-<version>.tar.gz
set -euo pipefail

VERSION="1.0.0"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$(cd "$ROOT/../Rigmind_v2" && pwd)"
STAGE="$ROOT/dist/rigmind-linux-x64"
PACKAGE="rigmind-linux-x64-${VERSION}"

# 1. Build the SPA (Vite must run under the Bun runtime; system Node 18 is too old).
(cd "$APP" && bun --bun node_modules/vite/bin/vite.js build --config vite.desktop.config.ts)

# 2. Stage web/ from the build output.
rm -rf "$ROOT/web" "$STAGE"
mkdir -p "$ROOT/web" "$STAGE"
cp -r "$APP/.output/public/assets" "$APP/.output/public/favicon.png" "$APP/.output/public/robots.txt" "$ROOT/web/"
cp "$APP/.output/public/_shell.html" "$ROOT/web/index.html"

# 3. Mirror the sql.js WASM to the path its asset URL points at (served from web/ root).
WASM_URL="$(grep -o '"url": *"[^"]*"' "$APP/src/assets/sql-wasm.wasm.asset.json" | cut -d'"' -f4)"
WASM_SRC="$(find "$APP/node_modules/sql.js" -name sql-wasm.wasm | head -1)"
mkdir -p "$ROOT/web$(dirname "$WASM_URL")"
cp "$WASM_SRC" "$ROOT/web$WASM_URL"

# 4. Build the C++ backend CLI and the GStreamer WebRTC pipeline (needs gstreamer dev packages).
cmake -S "$ROOT" -B "$ROOT/build-linux" -DCMAKE_BUILD_TYPE=Release -DRIGMIND_PIPELINE=ON >/dev/null
cmake --build "$ROOT/build-linux" --target rigmind-bridge-cli rigmind-pipeline -j"$(nproc)" >/dev/null

# 4b. BitNet b1.58 runtime (llama-server + its shared libraries) and the 2B model.
BITNET="$ROOT/third_party/bitnet.cpp/build/bin"
MODEL="$ROOT/third_party/bitnet-model/ggml-model-i2_s.gguf"
[ -x "$BITNET/llama-server" ] || { echo "missing BitNet llama-server: build third_party/bitnet.cpp first" >&2; exit 1; }
[ -f "$MODEL" ] || { echo "missing BitNet model: $MODEL" >&2; exit 1; }
rm -rf "$ROOT/slm-linux" && mkdir -p "$ROOT/slm-linux"
cp -L "$BITNET/llama-server" "$ROOT/slm-linux/"
find "$BITNET" -maxdepth 1 -name "*.so*" -exec cp -P {} "$ROOT/slm-linux/" \;
cp "$MODEL" "$ROOT/slm-linux/ggml-model-i2_s.gguf"

# 5. Stage the bundle: web, host, backend CLI, launcher, installer, icon.
cp -r "$ROOT/web" "$STAGE/"
cp "$ROOT/scripts/host.py" "$ROOT/scripts/slm_guard.py" "$ROOT/scripts/launch.sh" "$ROOT/scripts/install-linux.sh" "$STAGE/"
cp "$ROOT/build-linux/rigmind-bridge-cli" "$ROOT/build-linux/rigmind-pipeline" "$STAGE/"
cp "$ROOT/resources/rigmind-256.png" "$STAGE/"
cp -r "$ROOT/slm-linux" "$STAGE/slm"
chmod +x "$STAGE/launch.sh" "$STAGE/install-linux.sh" "$STAGE/rigmind-bridge-cli" "$STAGE/rigmind-pipeline"
echo "$VERSION" > "$STAGE/VERSION"

# 5b. Detection models (ONNX only) and the EWS plugin. TensorRT engines are NOT shipped: each machine builds
# its own on first launch (scripts/trt_engine.py), because an engine only loads on the GPU and TensorRT it was built on.
PPE_ONNX="$ROOT/third_party/ppe-model/ppe.onnx"
IDSS_ONNX="$ROOT/../IDSS/input_modules/models/idss_model.onnx"
[ -f "$PPE_ONNX" ] && [ -f "$IDSS_ONNX" ] || { echo "missing detection ONNX models: export them first" >&2; exit 1; }
[ -f "$ROOT/build-linux/librigmindews.so" ] || { echo "missing librigmindews.so: build the rigmindews target" >&2; exit 1; }
mkdir -p "$STAGE/models"
cp "$PPE_ONNX" "$STAGE/models/ppe.onnx"
cp "$IDSS_ONNX" "$STAGE/models/idss_model.onnx"
cp "$ROOT/build-linux/librigmindews.so" "$ROOT/scripts/trt_engine.py" "$STAGE/"

# 5. Archive.
mkdir -p "$ROOT/dist"
rm -rf "$ROOT/dist/$PACKAGE" && cp -r "$STAGE" "$ROOT/dist/$PACKAGE"
tar -C "$ROOT/dist" -czf "$ROOT/dist/$PACKAGE.tar.gz" "$PACKAGE"
rm -rf "$ROOT/dist/$PACKAGE"
echo "Built $ROOT/dist/$PACKAGE.tar.gz"

# 6. Single-file installer (app, host, pipeline, BitNet runtime and model, launcher).
scripts/make_run_installer.sh "$ROOT/dist/$PACKAGE.tar.gz" "$ROOT/dist/RigMind-Setup-linux-x64.run"
