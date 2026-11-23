#!/usr/bin/env bash
# Recompile the player's own default.xex (recomp/assets/default.xex) with the
# ReXGlue host tools that match the PS5 runtime (nightly-20261002-bd833a2a).
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
rexglue=${REXGLUE:-$HOME/rexglue-host/linux-amd64/bin/rexglue}
[[ -f $root/recomp/assets/default.xex ]] || { echo "error: put your default.xex in recomp/assets/" >&2; exit 2; }
cd "$root/recomp"
"$rexglue" -f codegen tronevolution_manifest.toml 2>&1 | tee codegen.log
