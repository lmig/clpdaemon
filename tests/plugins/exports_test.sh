#!/bin/sh
# Dynamic symbol table of an executable built with clpd_export_clips (T6b,
# docs/plugins.md): every global symbol defined in libclips.a is exported,
# and no clpd_* daemon internal is.
# usage: exports_test.sh <nm> <executable> <libclips.a>
set -u
NM=${1:?usage: exports_test.sh <nm> <executable> <libclips.a>}
EXE=${2:?usage: exports_test.sh <nm> <executable> <libclips.a>}
LIB=${3:?usage: exports_test.sh <nm> <executable> <libclips.a>}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$NM" -D --defined-only "$EXE" | awk 'NF == 3 { print $3 }' | sort -u >"$WORK/dyn" || exit 2
"$NM" -g --defined-only "$LIB" | awk 'NF == 3 { print $3 }' | sort -u >"$WORK/clips" || exit 2
failures=0
if [ ! -s "$WORK/clips" ]; then
  echo "FAIL: no global symbols read from $LIB"
  failures=$((failures + 1))
fi
missing=$(comm -23 "$WORK/clips" "$WORK/dyn")
if [ -n "$missing" ]; then
  printf 'FAIL: CLIPS symbols not exported (%s):\n%s\n' "$(printf '%s\n' "$missing" | wc -l)" "$(printf '%s\n' "$missing" | head -20)"
  failures=$((failures + 1))
fi
internal=$(grep '^clpd_' "$WORK/dyn")
if [ -n "$internal" ]; then
  printf 'FAIL: daemon internals exported (%s):\n%s\n' "$(printf '%s\n' "$internal" | wc -l)" "$(printf '%s\n' "$internal" | head -20)"
  failures=$((failures + 1))
fi
printf '%s CLIPS symbols exported, %s clpd_* exported\n' "$(wc -l <"$WORK/clips" | tr -d ' ')" "$(printf '%s' "$internal" | grep -c . )"
[ "$failures" -eq 0 ]
