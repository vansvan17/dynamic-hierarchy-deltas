#!/usr/bin/env sh
set -eu

binary="${1:-./build/dh}"
seed="${2:-17}"
k="${3:-4}"
header_printed=false

run_case() {
  output="$("$binary" scale "$@")"
  if [ "$header_printed" = false ]; then
    printf '%s\n' "$output"
    header_printed=true
  else
    printf '%s\n' "$output" | sed -n '2p'
  fi
}

for nodes in 1000000 5000000 10000000; do
  for ratio in 0.01 0.05 0.10; do
    for locality in global local; do
      run_case "$nodes" "$ratio" "$locality" "$seed" 5 local-sharing "$k"
    done
  done
done

for nodes in 1000000 10000000; do
  for locality in global local; do
    run_case "$nodes" 0.01 "$locality" "$seed" 5 cross-sharing "$k"
  done
done
