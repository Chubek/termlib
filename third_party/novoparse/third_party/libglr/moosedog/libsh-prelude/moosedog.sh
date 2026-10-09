# moosedog.sh -- shell helpers for Moosedog-generated parsers (libshell style).
# Usage: . moosedog.sh && moosedog_build JSON.grm out/
moosedog_build() {
  _grm="$1"; _out="${2:-.}"; _base="$(basename "$_grm" .grm)"
  moosedog --output-dir "$_out" --basename "$_base" "$_grm"
}
moosedog_check() {
  moosedog --check "$1"
}
