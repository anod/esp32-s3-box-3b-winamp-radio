#!/bin/sh
# Compile and run host-side C++ unit tests (no ESP toolchain needed).
set -e
cd "$(dirname "$0")/host"
OUT=.host-test-build
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT HUP INT TERM
for t in test_*.cpp; do
  g++ -std=c++17 -Wall -Wextra -Istubs -fsanitize=address,undefined -o "$OUT/${t%.cpp}" "$t"
  "$OUT/${t%.cpp}"
done
rm -rf "$OUT"
