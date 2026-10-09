#!/bin/bash
# fuzz-smoke.sh -- feed moosedog random/garbage inputs, assert no crashes.
# Usage: fuzz-smoke.sh <moosedog-binary> <moosedog-source-dir> [iterations]
# Exit 0 when every run exits 0/1/2 (ok / validation error / usage error).
# Any signal death (segfault/abort, exit > 128 or 134) fails the test.
set -u
BIN="$1"
SRCDIR="$2"
N="${3:-60}"
TMPD="$(mktemp -d /tmp/md-fuzz-XXXXXX)"
trap 'rm -rf "$TMPD"' EXIT INT TERM
fails=0
for ((i = 0; i < N; i++)); do
  f="$TMPD/case$i.grm"
  case $((i % 4)) in
    0) head -c $((RANDOM % 3000 + 1)) /dev/urandom > "$f";;
    1) { printf 'Entrypoint {{{{\nAST {{ Rule(((\nLexical '; head -c 200 /dev/urandom | tr -c '[:print:]\n' 'x'; printf '\n'; } > "$f";;
    2) printf 'Entrypoint { Config { MainLexer = "Nope" } } Syntactic { StartRule = "x" }' > "$f";;
    3) head -c $((RANDOM % 2000 + 10)) "$SRCDIR/examples/JSON/JSON.grm" > "$f";;
  esac
  "$BIN" --check "$f" > /dev/null 2>&1
  rc=$?
  if [ "$rc" -gt 2 ]; then
    echo "CRASH/ABORT: case $i exited with $rc"
    fails=$((fails + 1))
  fi
done
if [ "$fails" -ne 0 ]; then
  echo "fuzz-smoke: $fails/$N crashing inputs"
  exit 1
fi
echo "fuzz-smoke: $N inputs, no crashes"
