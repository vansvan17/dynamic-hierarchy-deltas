#!/usr/bin/env sh
set -eu

binary="${1:-./build/delta_scale_experiment}"
seed="${2:-17}"
header_printed=false

run_case() {
  output="$("$binary" "$@")"
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
      run_case "$nodes" "$ratio" "$locality" "$seed" full 5 local-sharing
    done
  done
done

for nodes in 1000000 10000000; do
  for locality in global local; do
    run_case "$nodes" 0.01 "$locality" "$seed" full 5 cross-sharing
  done
done
