#!/usr/bin/env bash
# builds ./editor.
#
# usage: ./build.sh [--debug]
#   --debug  unoptimised build with debug info and address/undefined sanitizers.
#
# vendored tree-sitter sources are compiled once into build/vendor and rebuilt only when a
# file in their folder changes.
# editor sources under src are always rebuilt (in parallel) because they compile quickly.
set -euo pipefail
cd "$(dirname "$0")"

mode=release
if [[ $# -gt 0 ]]; then
  [[ $1 == --debug && $# -eq 1 ]] || { echo "usage: ./build.sh [--debug]" >&2; exit 2; }
  mode=debug
fi

common=(-std=c23 -D_DEFAULT_SOURCE)
strict=(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)
if [[ $mode == debug ]]; then
  optimise=(-O0 -g3 -fno-omit-frame-pointer -fsanitize=address,undefined)
else
  optimise=(-O2)
fi

vendorDir=build/vendor
sourceDir=build/$mode
mkdir -p "$vendorDir" "$sourceDir"

# the spell checker binary searches the word list, so it must stay in byte order.
if ! LC_ALL=C sort -c data/words.txt 2>/dev/null; then
  echo "build: data/words.txt is not sorted in byte order" >&2
  echo "fix with: LC_ALL=C sort -o data/words.txt data/words.txt" >&2
  exit 1
fi

pids=()
objects=()

# compiles a vendor source when its object is missing or anything in folder (the grammar's,
# which may hold headers its scanner includes from elsewhere) is newer.
compileVendor() {
  local source=$1 name=$2 folder=$3
  local object=$vendorDir/$name.o
  objects+=("$object")

  if [[ -f $object && -z $(find "$folder" -newer "$object" -print -quit) ]]; then
    return
  fi

  gcc "${common[@]}" -O2 -w -Ivendor/tree-sitter/include -Ivendor/tree-sitter/src \
    -I"$(dirname "$source")" -c "$source" -o "$object" &
  pids+=($!)
}

compileVendor vendor/tree-sitter/src/lib.c tree-sitter vendor/tree-sitter/src

# a grammar is its parser.c and optional scanner.c, at the top of its folder or where
# upstream keeps them (php/php/src). other .c files are included by its scanner (yaml).
for grammar in vendor/grammars/*/; do
  name=$(basename "$grammar")
  while IFS= read -r source; do
    compileVendor "$source" "$name-$(basename "$source" .c)" "$grammar"
  done < <(find "$grammar" \( -name parser.c -o -name scanner.c \) | sort)
done

for source in src/*.c; do
  object=$sourceDir/$(basename "$source" .c).o
  objects+=("$object")
  gcc "${common[@]}" "${strict[@]}" "${optimise[@]}" -Ivendor/tree-sitter/include \
    -c "$source" -o "$object" &
  pids+=($!)
done

failed=0
for pid in "${pids[@]}"; do
  wait "$pid" || failed=1
done
[[ $failed -eq 0 ]] || { echo "build: compilation failed" >&2; exit 1; }

gcc "${optimise[@]}" "${objects[@]}" -o editor
echo "build: ./editor ($mode)"
