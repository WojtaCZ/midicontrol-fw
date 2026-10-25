#!/usr/bin/env bash
# Exports every base screen as PNG (+ index.html) by running the real GUI code on the host.
# Usage: tools/screens/export.sh [output dir, default build-screens/png] [scale, default 4]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="${1:-$root/build-screens/png}"
scale="${2:-4}"
cmake -S "$here" -B "$root/build-screens" -G Ninja -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build "$root/build-screens"
mkdir -p "$out"
rm -f "$out"/*.png
"$root/build-screens/screens" "$out" "$scale"
echo "-> $out/index.html"
