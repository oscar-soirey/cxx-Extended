#!/usr/bin/env sh
# Build the CXXE language server (Linux/macOS) and copy it into the VS Code
# extension under server/<platform>-<arch>/.
#   Tools/Ls/build.sh [output]
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
COMPILER="$ROOT/../../Compiler/Src"
OUT=${1:-"$ROOT/cxxe-language-server"}
SRCS=$(find "$ROOT/Src" "$COMPILER" -name '*.c' ! -path '*/Cli/Main.c')
# shellcheck disable=SC2086
${CC:-cc} -std=c11 -O2 -Wall -Wextra -I"$COMPILER" -I"$ROOT/Src" $SRCS -o "$OUT"
"$OUT" --version
case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) PLATFORM=linux-x64 ;;
    Darwin-x86_64) PLATFORM=darwin-x64 ;;
    Darwin-arm64) PLATFORM=darwin-arm64 ;;
    *) PLATFORM="" ;;
esac
EXT="$ROOT/../Extensions/Vscode/Source/server"
if [ -n "$PLATFORM" ] && [ -d "$EXT" ]; then
    mkdir -p "$EXT/$PLATFORM"
    cp "$OUT" "$EXT/$PLATFORM/cxxe-language-server"
    echo "Copied to $EXT/$PLATFORM/cxxe-language-server"
fi
