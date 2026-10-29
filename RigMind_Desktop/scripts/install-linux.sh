#!/usr/bin/env bash
# Installs the RigMind Linux bundle for the current user (no root needed).
#   ~/.local/share/rigmind   app files
#   ~/.local/bin/rigmind     launcher
#   ~/.local/share/applications/rigmind.desktop
set -euo pipefail

SRC="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${HOME}/.local/share/rigmind"
BIN="${HOME}/.local/bin"
APPS="${HOME}/.local/share/applications"

VERSION="$(cat "$SRC/VERSION" 2>/dev/null || echo unknown)"

# Replace app files only. The database, audit.log and other user data live in $PREFIX too and must survive.
mkdir -p "$PREFIX" "$BIN" "$APPS"
rm -rf "$PREFIX/web" "$PREFIX/slm"
rm -rf "$PREFIX/models"
rm -f "$PREFIX/host.py" "$PREFIX/slm_guard.py" "$PREFIX/rigmind-bridge-cli" "$PREFIX/rigmind-pipeline" "$PREFIX/launch.sh" "$PREFIX/rigmind-256.png" "$PREFIX/librigmindews.so" "$PREFIX/trt_engine.py"
cp -r "$SRC/web" "$SRC/slm" "$SRC/models" "$SRC/host.py" "$SRC/slm_guard.py" "$SRC/rigmind-bridge-cli" "$SRC/rigmind-pipeline" "$SRC/librigmindews.so" "$SRC/trt_engine.py" "$SRC/launch.sh" "$SRC/rigmind-256.png" "$PREFIX/"
chmod +x "$PREFIX/slm/llama-server"
chmod +x "$PREFIX/launch.sh" "$PREFIX/rigmind-bridge-cli" "$PREFIX/rigmind-pipeline"

# Python env for TensorRT (engine build on first launch). Own venv only, never the system site-packages.
# Engines are built per machine by launch.sh and reused from $PREFIX/engines; they are not part of this bundle.
if [ ! -x "$PREFIX/venv/bin/python" ]; then
    python3 -m venv "$PREFIX/venv"
fi
if ! "$PREFIX/venv/bin/python" -c "import tensorrt" >/dev/null 2>&1; then
    "$PREFIX/venv/bin/pip" install --quiet "tensorrt-cu13==10.16.*" || echo "warning: TensorRT not installed; detection will not run" >&2
fi
echo "$VERSION" > "$PREFIX/VERSION"
ln -sf "$PREFIX/launch.sh" "$BIN/rigmind"

# Installation is audited in the same trail as the app (audit.log, JSON lines).
printf '{"ts":%s,"source":"installer","action":"app.install","target":"%s","detail":"%s"}\n' \
    "$(date +%s%3N)" "$PREFIX" "$VERSION" >> "$PREFIX/audit.log"

cat > "$APPS/rigmind.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=RigMind
Comment=Rig safety operations
Exec=${PREFIX}/launch.sh
Icon=${PREFIX}/rigmind-256.png
Terminal=false
Categories=Utility;
StartupWMClass=RigMind
EOF

echo "Installed RigMind to $PREFIX"
echo "Run: rigmind   (or start it from the application menu)"
