#!/usr/bin/env bash
# Inside the container: build Windows binaries from /src (the RigMind_Desktop folder) and run the tests under Wine.
set -euo pipefail
export WINEDEBUG=-all WINEPREFIX=/tmp/wineprefix
cmake -S /src -B /tmp/build-win -DCMAKE_TOOLCHAIN_FILE=/src/docker/mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release \
      -DRIGMIND_PIPELINE=OFF -DCMAKE_EXE_LINKER_FLAGS="-static -static-libgcc -static-libstdc++"
cmake --build /tmp/build-win --target backend_test rigmind-bridge-cli -j"$(nproc)"
echo "== backend_test.exe under Wine"
xvfb-run -a /usr/lib/wine/wine64 /tmp/build-win/backend_test.exe
echo "== rigmind-bridge-cli.exe: app.version over stdin under Wine"
echo '{"id":1,"method":"app.version"}' | xvfb-run -a /usr/lib/wine/wine64 /tmp/build-win/rigmind-bridge-cli.exe 'Z:\tmp\wine-cli.db'
