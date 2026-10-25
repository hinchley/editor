#!/usr/bin/env bash
# builds ./editor (and compile_commands.json, for language servers) and, on request, the
# language packages.
#
# usage: ./build.sh [--debug | --static] [--languages] [--install]
#   --debug      unoptimised build with debug info and address/undefined sanitizers.
#   --static     optimised build linked statically against musl (with musl-gcc), which
#                has the vendored grammars, the packages --languages would build and
#                data/words.txt built in, so it needs nothing installed.
#   --languages  also builds a package for each vendored grammar into build/languages.
#   --install    copies data/words.txt to the config folder
#                (${XDG_CONFIG_HOME:-~/.config}/editor) when it has no word list yet, and
#                the packages in build/languages to its languages folder, replacing those
#                of the same name and leaving any others alone.
#
# the vendored tree-sitter runtime and grammars are compiled once into build/vendor (or
# build/vendor-static, for musl) and rebuilt only when a file in their folder changes.
# editor sources under src are always rebuilt (in parallel) because they compile quickly.
set -euo pipefail
cd "$(dirname "$0")"

usage() {
  echo "usage: ./build.sh [--debug | --static] [--languages] [--install]" >&2
  exit 2
}

mode=release
languages=0
install=0

for argument in "$@"; do
  case $argument in
    --debug) [[ $mode == static ]] && usage; mode=debug ;;
    --static) [[ $mode == debug ]] && usage; mode=static ;;
    --languages) languages=1 ;;
    --install) install=1 ;;
    *) usage ;;
  esac
done

common=(-std=c23 -D_DEFAULT_SOURCE)
warnings=(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wformat-nonliteral)
# -Wjump-misses-init is gcc's alone, so it stays out of compile_commands.json.
strict=("${warnings[@]}" -Wjump-misses-init -Werror)
# the sanitizers' runtimes need glibc, so a debug build cannot be a static one.
cc=gcc
link=()
vendorDir=build/vendor
if [[ $mode == debug ]]; then
  optimise=(-O0 -g3 -fno-omit-frame-pointer -fsanitize=address,undefined)
else
  optimise=(-O2)
fi
if [[ $mode == static ]]; then
  if ! command -v musl-gcc > /dev/null; then
    echo "build: --static needs musl-gcc (from musl)" >&2
    exit 1
  fi
  cc=musl-gcc
  link=(-static)
  vendorDir=build/vendor-static
fi

sourceDir=build/$mode
packageDir=build/languages
mkdir -p build/vendor "$vendorDir" "$sourceDir"

pids=()
objects=()

# true when target is missing or anything in folder is newer.
stale() {
  local target=$1 folder=$2
  [[ ! -f $target || -n $(find "$folder" -newer "$target" -print -quit) ]]
}

# compiles a vendor source into object with compiler when it is stale against folder (the
# grammar's, which may hold headers its scanner includes from elsewhere). flags are added
# at the end.
compileVendor() {
  local compiler=$1 source=$2 object=$3 folder=$4
  shift 4
  stale "$object" "$folder" || return 0

  "$compiler" "${common[@]}" -O2 -w -Ivendor/tree-sitter/include -Ivendor/tree-sitter/src \
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

runtime=vendor/tree-sitter/src
compileVendor "$cc" "$runtime/lib.c" "$vendorDir/tree-sitter.o" "$runtime"
objects+=("$vendorDir/tree-sitter.o")

# a grammar is its parser.c and optional scanner.c, at the top of its folder or where
# upstream keeps them (php/php/src). other .c files are included by its scanner (yaml).
grammarSources() {
  find "$1" \( -name parser.c -o -name scanner.c \) | sort
}

# fills folder with the files of package id but its grammar: the editor's own files for it
# (languages/<id>: language, tags.scm), and the vendored queries.
assemblePackage() {
  local id=$1 package=$2 queries=vendor/queries/$1
  [[ -f languages/$id/language ]] || { echo "build: no languages/$id/language" >&2; exit 1; }
  mkdir -p "$package"
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
}

# writes a byte array named name holding file, by its absolute path.
embed() {
  local name=$1 file=$2
  [[ -s $file ]] || { echo "build: cannot build in the empty $file" >&2; exit 1; }
  printf 'static const u8 %s[] = {\n#embed "%s"\n};\n\n' "$name" "$(realpath "$file")"
}

# writes builtin-data.c, the tables of builtin.h: the grammars, packages and word list of a
# static build, or none for any other.
writeBuiltins() {
  local id grammar symbol file count=0
  local -a ids=() grammars=() files=()

  if [[ $mode == static ]]; then
    for grammar in vendor/grammars/*/; do
      ids+=("$(basename "$grammar")")
    done
  fi

  echo "// generated by build.sh: the tables of builtin.h for a $mode build."
  echo "// do not edit by hand."
  echo "#include \"builtin.h\""
  echo

  for id in "${ids[@]}"; do
    symbol=$(sed -n 's/^symbol *= *//p' "languages/$id/language")
    symbol=${symbol:-tree_sitter_${id//-/_}}
    echo "const TSLanguage *$symbol(void);"
    grammars+=("  {\"$symbol\", $symbol},")
  done
  [[ ${#ids[@]} -eq 0 ]] || echo

  for id in "${ids[@]}"; do
    assemblePackage "$id" "$sourceDir/packages/$id"
    for file in "$sourceDir/packages/$id"/*; do
      embed "file$count" "$file"
      files+=("  {\"$id\", \"$(basename "$file")\", file$count, sizeof(file$count)},")
      count=$((count + 1))
    done
  done

  echo "const BuiltinGrammar builtinGrammars[] = {"
  [[ ${#grammars[@]} -eq 0 ]] || printf '%s\n' "${grammars[@]}"
  echo "  {nullptr, nullptr}"
  echo "};"
  echo
  echo "const char *const builtinPackages[] = {"
  for id in "${ids[@]}"; do
    echo "  \"$id\","
  done
  echo "  nullptr"
  echo "};"
  echo
  echo "const BuiltinFile builtinFiles[] = {"
  [[ ${#files[@]} -eq 0 ]] || printf '%s\n' "${files[@]}"
  echo "  {nullptr, nullptr, nullptr, 0}"
  echo "};"
  echo

  if [[ $mode == static ]]; then
    embed words data/words.txt
    echo "const BuiltinFile builtinWords = {nullptr, \"words.txt\", words, sizeof(words)};"
  else
    echo "const BuiltinFile builtinWords = {};"
  fi
}

if [[ $mode == static ]]; then
  for grammar in vendor/grammars/*/; do
    id=$(basename "$grammar")
    while IFS= read -r source; do
      object=$vendorDir/$id-$(basename "$source" .c).o
      compileVendor "$cc" "$source" "$object" "$grammar"
      objects+=("$object")
    done < <(grammarSources "$grammar")
  done
fi

rm -rf "$sourceDir/packages"
writeBuiltins > "$sourceDir/builtin-data.c"

for source in src/*.c "$sourceDir/builtin-data.c"; do
  object=$sourceDir/$(basename "$source" .c).o
  objects+=("$object")
  "$cc" "${common[@]}" "${strict[@]}" "${optimise[@]}" -Isrc -Ivendor/tree-sitter/include \
    -c "$source" -o "$object" &
  pids+=($!)
done

# a package's grammar is always built for glibc, as only a glibc build loads parser.so.
if [[ $languages -eq 1 ]]; then
  for grammar in vendor/grammars/*/; do
    id=$(basename "$grammar")
    while IFS= read -r source; do
      object=build/vendor/$id-$(basename "$source" .c).pic.o
      compileVendor gcc "$source" "$object" "$grammar" -fPIC
    done < <(grammarSources "$grammar")
  done
fi

waitAll
"$cc" "${optimise[@]}" "${link[@]}" "${objects[@]}" -o editor
echo "build: ./editor ($mode)"

# compile_commands.json tells a language server (clangd) how each source is compiled, so it
# can index the whole project for references and renames. it leaves out -Werror (or every
# warning would show as an error) and the optimisation and sanitizer flags, so release and
# debug builds write the same file, and it is only replaced when it changes, since clangd
# indexes again when it does.
directory=$(pwd | sed 's/\\/\\\\/g; s/"/\\"/g')
{
  echo "["
  separator=""
  for source in src/*.c; do
    arguments=""
    command=(gcc "${common[@]}" "${warnings[@]}" -Ivendor/tree-sitter/include -c "$source")
    for argument in "${command[@]}"; do
      arguments+="${arguments:+, }\"$argument\""
    done
    printf '%s  {"directory": "%s", "file": "%s", "arguments": [%s]}' \
      "$separator" "$directory" "$source" "$arguments"
    separator=$',\n'
  done
  printf '\n]\n'
} > build/compile_commands.json
cmp -s build/compile_commands.json compile_commands.json ||
  cp build/compile_commands.json compile_commands.json

# a package is the files assemblePackage gives it and the grammar linked as parser.so.
if [[ $languages -eq 1 ]]; then
  for grammar in vendor/grammars/*/; do
    id=$(basename "$grammar")
    package=$packageDir/$id
    assemblePackage "$id" "$package"

    grammarObjects=()
    while IFS= read -r source; do
      grammarObjects+=("build/vendor/$id-$(basename "$source" .c).pic.o")
    done < <(grammarSources "$grammar")

    if stale "$package/parser.so" "$grammar"; then
      gcc -shared -O2 "${grammarObjects[@]}" -o "$package/parser.so"
    fi
  done

  echo "build: $packageDir ($(ls "$packageDir" | wc -l) packages)"
fi

# each file is copied beside its target and renamed over it, so a running editor that has
# a parser.so loaded keeps its old copy.
# the word list is only copied when there is none, so words added to it are kept.
if [[ $install -eq 1 ]]; then
  config=${XDG_CONFIG_HOME:-$HOME/.config}/editor
  if [[ ! -e $config/words.txt ]]; then
    mkdir -p "$config"
    cp data/words.txt "$config/.words.txt.new"
    mv "$config/.words.txt.new" "$config/words.txt"
    echo "build: installed the word list in $config"
  fi

  [[ -d $packageDir ]] || { echo "build: nothing in $packageDir, run with --languages" >&2; exit 1; }
  target=$config/languages

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
