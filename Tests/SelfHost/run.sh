#!/usr/bin/env bash
# CXXE self-hosting readiness suite.
#
#   Tests/SelfHost/run.sh [path/to/cxxe]
#
# Without an argument, cxxe is built from Compiler/Src with gcc. Every case is
# translated by cxxe, compiled with g++ -std=c++20 together with the exported
# runtime, then executed (exit code 0 expected; 27 for cpp_regression).
#   Cases/   single-file programs (also runs Tests/v0.2/examples)
#   Multi/   one directory per program: Main.cppe + other .cppe + .hppe headers
#   Errors/  programs cxxe must reject with a diagnostic
# Works on Linux and on Windows from Git Bash (MinGW gcc/g++ in PATH).
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/cxxe-selfhost.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
EXE=""; case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXE=".exe";; esac

if [ $# -ge 1 ]; then CXXE="$1"; else
    CXXE="$WORK/cxxe$EXE"
    gcc -std=c11 -O2 -Wall -Wextra -I"$ROOT/Compiler/Src" $(find "$ROOT/Compiler/Src" -name '*.c') -o "$CXXE" || exit 1
fi
CXX="${CXX:-g++}"
cd "$WORK" && "$CXXE" runtime export rt >/dev/null || exit 1

pass=0; fail=0
report() { fail=$((fail + 1)); echo "FAIL $1 -> $2"; }

# run_program NAME MAIN.cppe EXPECTED_RC INCLUDE_DIR [extra generated .cpp...]
run_program() {
    local n="$1" src="$2" exp="$3" inc="$4"; shift 4
    if ! "$CXXE" -I"$inc" roundtrip "$src" "$n.cpp" >"$n.log" 2>&1; then report "$n" "cxxe: $(grep -m1 error "$n.log")"; return; fi
    if ! "$CXX" -std=c++20 -w -Irt -I. "$n.cpp" "$@" rt/cxxe_runtime.cpp -o "$n$EXE" 2>"$n.gcc"; then report "$n" "$CXX: $(grep -m1 error "$n.gcc")"; return; fi
    ./"$n$EXE" >"$n.out" 2>&1; local rc=$?
    if [ "$rc" = "$exp" ]; then pass=$((pass + 1)); else report "$n" "exit $rc: $(tail -1 "$n.out")"; fi
}

for f in "$ROOT"/Tests/v0.2/examples/*.cppe "$HERE"/Cases/*.cppe; do
    n="$(basename "${f%.cppe}")"; e=0; [ "$n" = cpp_regression ] && e=27
    run_program "$n" "$f" "$e" "$(dirname "$f")"
done

for d in "$HERE"/Multi/*/; do
    name="$(basename "$d")"; extra=()
    for h in "$d"*.hppe; do [ -e "$h" ] || continue
        b="$(basename "${h%.hppe}")"
        "$CXXE" -I"$d" roundtrip "$h" "$b.hpp" >/dev/null 2>"$name.hlog" || report "$name" "cxxe header $b: $(grep -m1 error "$name.hlog")"
    done
    for s in "$d"*.cppe; do b="$(basename "${s%.cppe}")"; [ "$b" = Main ] && continue
        "$CXXE" -I"$d" roundtrip "$s" "${name}_$b.cpp" >/dev/null 2>"$name.slog" || report "$name" "cxxe $b: $(grep -m1 error "$name.slog")"
        extra+=("${name}_$b.cpp")
    done
    run_program "$name" "$d/Main.cppe" 0 "$d" "${extra[@]}"
done

for f in "$HERE"/Errors/*.cppe; do
    n="$(basename "${f%.cppe}")"
    if "$CXXE" roundtrip "$f" err.cpp >err.log 2>&1; then report "$n" "expected a cxxe error"
    elif grep -q ':[0-9]*:[0-9]*: error:' err.log; then pass=$((pass + 1))
    else report "$n" "error without file:line:col: $(head -1 err.log)"; fi
done

echo "== $pass passed, $fail failed"
[ "$fail" = 0 ]
