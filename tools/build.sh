#!/usr/bin/env bash
# Build the PS5 title into ../PS5_ReXGlueTemplate/dist/PPSA90120/.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
template=${PS5_REXGLUE_TEMPLATE:-$(dirname "$root")/PS5_ReXGlueTemplate}
[[ -d $root/rexglue-sdk ]] && export REXGLUE_SRC=${REXGLUE_SRC:-$root/rexglue-sdk} REXGLUE_PS5_BUILD=${REXGLUE_PS5_BUILD:-$root/build/rexglue}
GAME=$root/games/tron exec "$template/tools/build.sh" "$@"
