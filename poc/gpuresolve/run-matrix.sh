#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
gpu=${GPU:-RTX}
run() { local dir=$1; shift; "$here/$dir/gpuresolve" --spv "$here/$dir/spv" --gpu "$gpu" --rounds 4 --block 40 "$@"; }
echo "== A défaut"; run out
echo "== B tables écrites par le GPU"; run out --gpu-tables 1
echo "== C blocs par dessin en mémoire hôte"; run out --ring-mem ram --arms cpu-ubo,cpu-bda
echo "== D shader léger KT4 KB2 CR8"; run out-light
echo "== E shader lourd KT16 KB8 CR48"; run out-heavy
echo "== F shader lourd, tables GPU"; run out-heavy --gpu-tables 1
echo "== G plus de pixels (overdraw 8)"; run out --overdraw 8
echo "== H dessins seuls"; run out --dispatches 0
echo "== I une passe par image pour la résolution groupée"; run out --passes 20
