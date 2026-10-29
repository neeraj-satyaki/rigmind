#!/usr/bin/env bash
# Wraps the Linux bundle tarball into one self-extracting installer: RigMind-Setup-linux-x64.run.
# The tarball already holds the app, pipeline, host, BitNet runtime and model, so the .run file is complete.
# Usage: make_run_installer.sh <rigmind-linux-x64-VERSION.tar.gz> <output.run>
set -euo pipefail

TARBALL="$1"
OUT="$2"
PACKAGE="$(basename "$TARBALL" .tar.gz)"

cat > "$OUT" <<'HEADER'
#!/bin/sh
# RigMind single-file installer (Linux x64). Unpacks the bundle to a temp folder and runs install-linux.sh.
# Installs for the current user only. User data (rigmind.db, audit.log) is kept.
set -e
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
LINE="$(awk '/^__ARCHIVE__$/ { print NR + 1; exit }' "$0")"
tail -n +"$LINE" "$0" | tar -xz -C "$TMP"
bash "$TMP"/rigmind-linux-x64-*/install-linux.sh
exit 0
__ARCHIVE__
HEADER

cat "$TARBALL" >> "$OUT"
chmod +x "$OUT"
echo "Built $OUT ($PACKAGE)"
