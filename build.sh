#!/usr/bin/env bash
# builds ./editor and, on request, the language packages.
#
# usage: ./build.sh [--debug] [--languages] [--install]
#   --debug      unoptimised build with debug info and address/undefined sanitizers.
#   --languages  also builds a package for each vendored grammar into build/languages.
#   --install    copies the packages in build/languages to the config folder
#                (${XDG_CONFIG_HOME:-~/.config}/editor/languages), replacing those of the
#                same name and leaving any others alone.
#
# the vendored tree-sitter runtime and grammars are compiled once into build/vendor and
# rebuilt only when a file in their folder changes.
# editor sources under src are always rebuilt (in parallel) because they compile quickly.
set -euo pipefail
cd "$(dirname "$0")"

usage() {
  echo "usage: ./build.sh [--debug] [--languages] [--install]" >&2
  exit 2
}

mode=release
languages=0
install=0

for argument in "$@"; do
  case $argument in
    --debug) mode=debug ;;
    --languages) languages=1 ;;
    --install) install=1 ;;
    *) usage ;;
  esac
done

common=(-std=c23 -D_DEFAULT_SOURCE)
strict=(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)
if [[ $mode == debug ]]; then
  optimise=(-O0 -g3 -fno-omit-frame-pointer -fsanitize=address,undefined)
else
  optimise=(-O2)
fi

vendorDir=build/vendor
sourceDir=build/$mode
packageDir=build/languages
mkdir -p "$vendorDir" "$sourceDir"

# the spell checker binary searches the word list, so it must stay in byte order.
if ! LC_ALL=C sort -c data/words.txt 2>/dev/null; then
  echo "build: data/words.txt is not sorted in byte order" >&2
  echo "fix with: LC_ALL=C sort -o data/words.txt data/words.txt" >&2
  exit 1
fi

pids=()
objects=()

# true when target is missing or anything in folder is newer.
stale() {
  local target=$1 folder=$2
  [[ ! -f $target || -n $(find "$folder" -newer "$target" -print -quit) ]]
}

# compiles a vendor source into object when it is stale against folder (the grammar's,
# which may hold headers its scanner includes from elsewhere). flags are added at the end.
compileVendor() {
  local source=$1 object=$2 folder=$3
  shift 3
  stale "$object" "$folder" || return 0

  gcc "${common[@]}" -O2 -w -Ivendor/tree-sitter/include -Ivendor/tree-sitter/src \
    -I"$(dirname "$source")" "$@" -c "$source" -o "$object" &
  pids+=($!)
}

waitAll() {
  local failed=0
  for pid in "${pids[@]}"; do
    wait "$pid" || failed=1
  done
  pids=()
  [[ $failed -eq 0 ]] || { echo "build: compilation failed" >&2; exit 1; }
}

compileVendor vendor/tree-sitter/src/lib.c "$vendorDir/tree-sitter.o" vendor/tree-sitter/src
objects+=("$vendorDir/tree-sitter.o")

for source in src/*.c; do
  object=$sourceDir/$(basename "$source" .c).o
  objects+=("$object")
  gcc "${common[@]}" "${strict[@]}" "${optimise[@]}" -Ivendor/tree-sitter/include \
    -c "$source" -o "$object" &
  pids+=($!)
done

# a grammar is its parser.c and optional scanner.c, at the top of its folder or where
# upstream keeps them (php/php/src). other .c files are included by its scanner (yaml).
grammarSources() {
  find "$1" \( -name parser.c -o -name scanner.c \) | sort
}

if [[ $languages -eq 1 ]]; then
  for grammar in vendor/grammars/*/; do
    id=$(basename "$grammar")
    while IFS= read -r source; do
      compileVendor "$source" "$vendorDir/$id-$(basename "$source" .c).pic.o" "$grammar" -fPIC
    done < <(grammarSources "$grammar")
  done
fi

waitAll
gcc "${optimise[@]}" "${objects[@]}" -o editor
echo "build: ./editor ($mode)"

# a package is the editor's own files for it (languages/<id>: language, tags.scm), the
# vendored queries and the grammar linked as parser.so.
if [[ $languages -eq 1 ]]; then
  for grammar in vendor/grammars/*/; do
    id=$(basename "$grammar")
    package=$packageDir/$id
    queries=vendor/queries/$id
    mkdir -p "$package"
    [[ -f languages/$id/language ]] || { echo "build: no languages/$id/language" >&2; exit 1; }

    grammarObjects=()
    while IFS= read -r source; do
      grammarObjects+=("$vendorDir/$id-$(basename "$source" .c).pic.o")
    done < <(grammarSources "$grammar")

    if stale "$package/parser.so" "$grammar"; then
      gcc -shared -O2 "${grammarObjects[@]}" -o "$package/parser.so"
    fi

    rm -f "$package"/*.scm
    cp "languages/$id"/* "$package/"
    cp "$queries/highlights.scm" "$package/"

    # php keeps the injection of html (into the text outside <?php ?>) in a file of its own.
    if [[ -f $queries/injections-text.scm ]]; then
      { cat "$queries/injections.scm"; printf '\n'; cat "$queries/injections-text.scm"; } \
        > "$package/injections.scm"
    elif [[ -f $queries/injections.scm ]]; then
      cp "$queries/injections.scm" "$package/"
    fi
  done

  echo "build: $packageDir ($(ls "$packageDir" | wc -l) packages)"
fi

# each file is copied beside its target and renamed over it, so a running editor that has
# a parser.so loaded keeps its old copy.
if [[ $install -eq 1 ]]; then
  [[ -d $packageDir ]] || { echo "build: nothing in $packageDir, run with --languages" >&2; exit 1; }
  target=${XDG_CONFIG_HOME:-$HOME/.config}/editor/languages

  for package in "$packageDir"/*/; do
    id=$(basename "$package")
    mkdir -p "$target/$id"
    rm -f "$target/$id"/*.scm

    for file in "$package"*; do
      name=$(basename "$file")
      cp "$file" "$target/$id/.$name.new"
      mv "$target/$id/.$name.new" "$target/$id/$name"
    done
  done

  echo "build: installed $(ls "$packageDir" | wc -l) packages in $target"
fi
