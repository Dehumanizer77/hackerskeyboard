#!/bin/bash
# Compile and run the dictionary parser harness under AddressSanitizer.
#
#   ./run.sh                 # test the parser in this working tree
#   ./run.sh /path/to/cpp    # test some other copy (e.g. an unpatched one)
#
# Exits non-zero if ASan reports anything.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${1:-$HERE/../../app/src/main/cpp}"
ITERATIONS="${ITERATIONS:-3000}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CXX="${CXX:-g++}"
echo "parser source: $SRC"

# Memory errors are fatal; integer-overflow reports are printed but allowed to
# continue, so one benign overflow does not hide a later out-of-bounds access.
"$CXX" -std=c++14 -g -O1 -fno-omit-frame-pointer \
    -fsanitize=address,undefined -fno-sanitize-recover=address \
    -Wno-unused-value -Wno-write-strings \
    -I"$SRC" \
    "$HERE/fuzz_dictionary.cpp" "$SRC/dictionary.cpp" "$SRC/char_utils.cpp" \
    -o "$OUT/fuzz" || { echo "COMPILE FAILED"; exit 2; }

ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=0 "$OUT/fuzz" "$ITERATIONS"
rc=$?
echo
if [ $rc -eq 0 ]; then
    echo "RESULT: clean (exit 0)"
else
    echo "RESULT: sanitizer reported a fault (exit $rc)"
fi
exit $rc
