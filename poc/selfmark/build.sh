#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$here/out"
g++ -O2 -std=c++20 -pthread -o "$here/out/selfmark" "$here/selfmark.cpp"
echo "ok $here/out"
