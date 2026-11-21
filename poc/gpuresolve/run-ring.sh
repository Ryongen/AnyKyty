#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
gpu=${GPU:-RTX}
run() { local dir=$1; shift; "$here/$dir/gpuresolve" --spv "$here/$dir/spv" --gpu "$gpu" --rounds 4 --block 40 "$@"; }
a4=cpu-ubo,cpu-bda,gpu-inv-pt,gpu-inv-ct
echo "== N défaut, blocs CPU recopiés en VRAM pure"; run out --ring-mem copy --arms $a4
echo "== O dessins seuls, lourd, overdraw 24, blocs recopiés"; run out-heavy --dispatches 0 --overdraw 24 --ring-mem copy --arms $a4
echo "== P dessins seuls, défaut, overdraw 3, blocs recopiés"; run out --dispatches 0 --ring-mem copy --arms $a4
echo "== Q léger, contrôle d'image"; run out-light --arms $a4
echo "== R calculs seuls, blocs recopiés"; run out --draws 64 --passes 1 --dispatches 2600 --ring-mem copy --arms $a4
