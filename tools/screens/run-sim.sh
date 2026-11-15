#!/usr/bin/env bash
# Usage: run-sim.sh <sim-binary> <out-dir>   Runs every flows/*.sim, writes JSONL per flow, fails on any failed expect.
set -u
sim=$1; out=$2; here=$(cd "$(dirname "$0")" && pwd); mkdir -p "$out"; rc=0
# Provenance: the lib/ used (may be another checkout with local changes): git diff --stat + sha256 of untracked files.
if [ -n "${FW_LIB_DIR:-}" ]; then
  { echo "FW_LIB_DIR=$FW_LIB_DIR"
    repo=$(git -C "$FW_LIB_DIR" rev-parse --show-toplevel 2>/dev/null || true)
    [ -n "$repo" ] && { git -C "$repo" rev-parse HEAD; git -C "$repo" diff --stat; }
    for d in "$FW_LIB_DIR"/*/; do   # lib/ui, lib/menu, ... are submodules with their own state
      [ -e "$d/.git" ] || continue
      echo "== $d"; git -C "$d" rev-parse HEAD; git -C "$d" diff --stat
      git -C "$d" ls-files --others --exclude-standard | while read -r u; do sha256sum "$d/$u"; done
    done
  } > "$out/provenance.txt" 2>&1
fi
for f in "$here"/flows/*.sim; do
  "$sim" "$f" "$out/$(basename "$f" .sim).jsonl" >/dev/null 2>"$out/err.txt" || { cat "$out/err.txt"; rc=1; }
done
exit $rc
