#!/usr/bin/env bash
# Compare Hoshi against C (clang -O2) on the same programs.
# usage: bench/compare.sh [path-to-hoshic]
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
hoshic="${1:-$root/build/hoshic}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# name | hoshi source | C source | arguments
benches=(
  "fib|examples/fib.hoshi|bench/fib.c|"
  "nbody|examples/nbody.hoshi|bench/nbody.c|50000000"
)

best_of_3() {
  local best=""
  for _ in 1 2 3; do
    local start end ms
    start=$(date +%s%N)
    "$@" >/dev/null
    end=$(date +%s%N)
    ms=$(( (end - start) / 1000000 ))
    if [[ -z "$best" || "$ms" -lt "$best" ]]; then best=$ms; fi
  done
  echo "$best"
}

printf "%-8s %10s %10s %8s\n" "bench" "hoshi(ms)" "c(ms)" "ratio"
for entry in "${benches[@]}"; do
  IFS='|' read -r name hs cs args <<<"$entry"
  "$hoshic" -O2 "$root/$hs" -o "$tmp/$name-hoshi"
  clang -O2 "$root/$cs" -o "$tmp/$name-c" -lm

  if ! diff <("$tmp/$name-hoshi" $args) <("$tmp/$name-c" $args) >/dev/null; then
    echo "$name: outputs differ between Hoshi and C" >&2
    exit 1
  fi
  h=$(best_of_3 "$tmp/$name-hoshi" $args)
  c=$(best_of_3 "$tmp/$name-c" $args)
  printf "%-8s %10d %10d %8s\n" "$name" "$h" "$c" "$(awk "BEGIN { printf \"%.2fx\", $h / $c }")"
done
