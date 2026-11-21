#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
zy="$here/../../3rdparty/zydis"
zc="$here/deps/zycore"
out="$here/out"
mkdir -p "$out/zy"
if [ ! -f "$out/libzy.a" ]; then
  for f in "$zy"/src/*.c "$zc"/src/*.c "$zc"/src/API/*.c; do
    gcc -O2 -c -DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD -I"$zy/include" -I"$zy/src" -I"$zc/include" -o "$out/zy/$(basename "$f" .c)-$(echo "$f" | md5sum | cut -c1-6).o" "$f" &
  done
  wait
  ar rcs "$out/libzy.a" "$out"/zy/*.o
fi
g++ -O2 -std=c++20 -mavx2 -DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD -I"$zy/include" -I"$zc/include" -o "$out/patch" "$here/patch.cpp" "$out/libzy.a"
echo "ok $out/patch"
