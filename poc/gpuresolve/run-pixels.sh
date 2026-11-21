#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
gpu=${GPU:-RTX}
arms=cpu-ubo,cpu-bda,gpu-inv-pt,gpu-inv-ct
run() { local dir=$1; shift; "$here/$dir/gpuresolve" --spv "$here/$dir/spv" --gpu "$gpu" --rounds 4 --block 40 --arms $arms "$@"; }
echo "== J dessins seuls, shader lourd, overdraw 8"; run out-heavy --dispatches 0 --overdraw 8
echo "== K dessins seuls, shader lourd, overdraw 24"; run out-heavy --dispatches 0 --overdraw 24
echo "== L dessins seuls, shader défaut, overdraw 24"; run out --dispatches 0 --overdraw 24
echo "== M calculs seuls (2600 dispatchs, 1 passe)"; run out --draws 64 --passes 1 --dispatches 2600
