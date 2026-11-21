#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/out}
KT=${KT:-8}; KB=${KB:-4}; CR=${CR:-24}; NTEX=${NTEX:-512}
mkdir -p "$out/spv"
gen() {
  local name=$1 stage=$2 src=$3; shift 3
  local tmp="$out/spv/$name.$stage"
  { sed -n '1,7p' "$here/common.glsl"; for d in "$@" "KT=$KT" "KB=$KB" "CR=$CR" "NTEX=$NTEX"; do echo "#define ${d%%=*} ${d#*=}"; done; sed -n '8,$p' "$here/common.glsl"; cat "$here/$src"; } > "$tmp"
  glslangValidator --quiet -V --target-env vulkan1.3 -S "$stage" -o "$out/spv/$name.spv" "$tmp"
}
gen vs_ubo vert draw.vert MODE=0 PT=0
gen fs_ubo frag draw.frag MODE=0 PT=0
gen vs_bda vert draw.vert MODE=1 PT=0
gen fs_bda frag draw.frag MODE=1 PT=0
gen vs_g1pt vert draw.vert MODE=2 PT=1
gen fs_g1pt frag draw.frag MODE=2 PT=1
gen vs_g1ct vert draw.vert MODE=2 PT=0
gen fs_g1ct frag draw.frag MODE=2 PT=0
gen resolve_pt comp resolve.comp MODE=2 PT=1
gen resolve_ct comp resolve.comp MODE=2 PT=0
gen tables comp tables.comp MODE=2 PT=0
gen op_bda comp op.comp MODE=1 PT=0
gen op_g1pt comp op.comp MODE=2 PT=1
gen op_g1ct comp op.comp MODE=2 PT=0
echo "$KT $KB $CR $NTEX" > "$out/spv/config.txt"
g++ -O2 -std=c++20 -o "$out/gpuresolve" "$here/gpuresolve.cpp" -lvulkan
echo "ok $out"
