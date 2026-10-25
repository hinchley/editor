#!/usr/bin/env bash
# makes a language package from a tree-sitter grammar repository.
#
# usage: tools/language.sh <grammar> <id> [<package>]
#   grammar  the folder holding the grammar's src/parser.c (for a repository with several
#            grammars, the one inside it, such as tree-sitter-php/php).
#   id       the package's folder name, such as go.
#   package  where to make it; ${XDG_CONFIG_HOME:-~/.config}/editor/languages/<id> by
#            default.
#
# builds parser.so from src/parser.c and src/scanner.c, copies queries/highlights.scm and
# queries/injections.scm (from the grammar's folder or the repository's), and writes a
# language file to fill in unless the package has one. see README.md, "Adding a language".
# only the default (glibc) build loads parser.so; a static one cannot.
set -euo pipefail

[[ $# -eq 2 || $# -eq 3 ]] || { echo "usage: tools/language.sh <grammar> <id> [<package>]" >&2; exit 2; }
grammar=$1
id=$2
package=${3:-${XDG_CONFIG_HOME:-$HOME/.config}/editor/languages/$id}

if [[ ! -f $grammar/src/parser.c ]]; then
  echo "language: no $grammar/src/parser.c; run tree-sitter generate in $grammar first" >&2
  exit 1
fi

[[ -f $grammar/src/scanner.cc ]] && { echo "language: C++ scanners are not supported" >&2; exit 1; }

sources=("$grammar/src/parser.c")
[[ -f $grammar/src/scanner.c ]] && sources+=("$grammar/src/scanner.c")
mkdir -p "$package"
cc -shared -fPIC -O2 -w -I"$grammar/src" "${sources[@]}" -o "$package/parser.so"

for query in highlights injections; do
  for folder in "$grammar/queries" "$grammar/../queries"; do
    if [[ -f $folder/$query.scm ]]; then
      cp "$folder/$query.scm" "$package/"
      break
    fi
  done
done

[[ -f $package/highlights.scm ]] || echo "language: no queries/highlights.scm; write one" >&2

# the entry point, when it is not the one the id gives.
symbol=$(nm -D --defined-only "$package/parser.so" | awk '$3 ~ /^tree_sitter_/ { print $3 }' | head -1)
default=tree_sitter_${id//-/_}

if [[ ! -f $package/language ]]; then
  {
    echo "# see README.md, \"Adding a language\". the values below are guesses."
    echo "name = ${id^}"
    echo "extensions = $id"
    echo "comment = //"
    [[ -n $symbol && $symbol != "$default" ]] && echo "symbol = $symbol"
  } > "$package/language"
  echo "language: wrote $package/language; check it"
fi

echo "language: made $package (it is read when the editor starts)"
