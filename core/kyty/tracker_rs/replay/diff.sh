#!/usr/bin/env bash
set -uo pipefail
tools=${1:?dossier des outils}; trace=${2:?trace}; every=${3:-1000000}
work=$(mktemp -d)
mkfifo "$work/cpp" "$work/rs"
"$tools/tracker_replay_cpp" "$trace" --out "$work/cpp" --digest-every "$every" 2> "$work/cpp.log" &
a=$!
"$tools/tracker_replay_rs" "$trace" --out "$work/rs" --digest-every "$every" 2> "$work/rs.log" &
b=$!
"$tools/tracker_compare" "$work/cpp" "$work/rs" --trace "$trace" --show 8
code=$?
wait $a; ca=$?
wait $b; cb=$?
cat "$work/cpp.log" "$work/rs.log"
echo "replay exit codes: cpp $ca, rust $cb, compare $code"
rm -rf "$work"
exit $code
