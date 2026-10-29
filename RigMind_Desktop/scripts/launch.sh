#!/usr/bin/env bash
# RigMind launcher (Linux). Starts the host (SPA + C++ backend) on 127.0.0.1 and opens it in an app window.
set -euo pipefail

DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
PORT="${RIGMIND_PORT:-8765}"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}/rigmind"
DB="${RIGMIND_DB:-$DATA/rigmind.db}"
URL="http://127.0.0.1:${PORT}/"

# Lifecycle audit entries share audit.log with the backend and the installer.
audit() {
    printf '{"ts":%s,"source":"launcher","action":"%s","target":"%s","detail":""}\n' \
        "$(date +%s%3N)" "$1" "$PORT" >> "$(dirname "$DB")/audit.log"
}

# Plain-language SLM (BitNet b1.58) runs from the bundle; host.py starts it.
export LD_LIBRARY_PATH="$DIR/slm${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export RIGMIND_SLM_SERVER="$DIR/slm/llama-server"
export RIGMIND_SLM_MODEL="$DIR/slm/ggml-model-i2_s.gguf"

# Detection engines are built on this machine (GPU + TensorRT version) and reused afterwards. First launch is slower.
PY="$DATA/venv/bin/python"
[ -x "$PY" ] || PY=python3
if "$PY" -c "import tensorrt" >/dev/null 2>&1; then
    for model in ppe idss_model; do
        "$PY" "$DIR/trt_engine.py" "$DIR/models/$model.onnx" "$DATA/engines" >/dev/null \
            || echo "engine build failed for $model: detection disabled" >&2
    done
fi

python3 "$DIR/host.py" "$DIR/web" "$PORT" "$DIR/rigmind-bridge-cli" "$DB" "$DIR/rigmind-pipeline" &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null || true; audit app.exit' EXIT
audit app.launch

# Wait until the host answers (max ~5 s).
for _ in $(seq 1 50); do
    curl -s -o /dev/null "$URL" && break
    sleep 0.1
done

BROWSER="$(command -v google-chrome || command -v chromium || command -v chromium-browser || true)"
if [ -n "$BROWSER" ]; then
    # Alert tones fire from the backend, not from a click: allow audio without a user gesture.
    # 16:9 window: the boot splash and the shell fill the window, so its size sets the aspect ratio.
    "$BROWSER" --app="$URL" --class=RigMind --window-size=1600,900 --autoplay-policy=no-user-gesture-required \
        --user-data-dir="$DATA/browser-profile" "$@"
else
    xdg-open "$URL" >/dev/null 2>&1 || echo "Open $URL in a browser"
    wait "$SERVER_PID"
fi
