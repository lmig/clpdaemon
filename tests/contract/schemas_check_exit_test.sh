#!/bin/sh
# Checks the exit status of tests/contract/schemas_check.py for a missing
# required schema file (T12c, R3-missing-schema-exit2-unexercised).
#
# usage: schemas_check_exit_test.sh <python>
#
# <python> is the interpreter that runs schemas_check.py; it must have the
# jsonschema package. For each of envelope.schema.json, work-order-closed@1.json
# and delivery-failed@1.json the script copies docs/schemas to a temporary
# directory without that file, runs
#   <python> tests/contract/schemas_check.py <dir> docs/exchange-contract.md
# and requires exit 2 and a stderr line naming the file; with the full copy it
# requires exit 0. Temporary directories are removed on exit.
#
# Exit status: 0 all checks passed, 1 a check failed, 2 usage error,
# 77 <python> lacks jsonschema (ctest SKIP_RETURN_CODE).

if [ "$#" -ne 1 ]; then
    echo 'usage: schemas_check_exit_test.sh <python>' >&2
    exit 2
fi
PY=$1
ROOT=$(cd "$(dirname "$0")/../.." && pwd) || exit 2
CHECK=$ROOT/tests/contract/schemas_check.py
SCHEMAS=$ROOT/docs/schemas
DOC=$ROOT/docs/exchange-contract.md

if ! "$PY" -c 'import jsonschema' >/dev/null 2>&1; then
    echo "schemas_check_exit_test.sh: $PY has no jsonschema; skipped" >&2
    exit 77
fi

WORK=$(mktemp -d) || exit 2
trap 'rm -rf "$WORK"' EXIT
trap 'exit 2' HUP INT TERM

fails=0
checks=0
result() {  # result <name> <ok 0|1> [<detail>]
    checks=$((checks + 1))
    if [ "$2" -eq 0 ]; then
        echo "PASS $1"
    else
        echo "FAIL $1 $3"
        fails=$((fails + 1))
    fi
}

for missing in envelope.schema.json work-order-closed@1.json delivery-failed@1.json; do
    dir=$WORK/without-$missing
    mkdir "$dir" && cp "$SCHEMAS"/*.json "$dir"/ || exit 2
    mv "$dir/$missing" "$WORK/moved-$missing" || exit 2   # the copy, never the repository file
    "$PY" "$CHECK" "$dir" "$DOC" >"$WORK/out" 2>"$WORK/err"
    rc=$?
    named=1
    grep -F -- "$missing" "$WORK/err" >/dev/null && named=0
    ok=1
    [ "$rc" -eq 2 ] && [ "$named" -eq 0 ] && ok=0
    result "without $missing: exit 2 and stderr names the file" "$ok" "(exit $rc, stderr: $(cat "$WORK/err"))"
done

dir=$WORK/full
mkdir "$dir" && cp "$SCHEMAS"/*.json "$dir"/ || exit 2
"$PY" "$CHECK" "$dir" "$DOC" >"$WORK/out" 2>"$WORK/err"
rc=$?
ok=1
[ "$rc" -eq 0 ] && ok=0
result "full copy: exit 0" "$ok" "(exit $rc, stderr: $(cat "$WORK/err"))"

echo "checks $checks fails $fails"
[ "$fails" -eq 0 ]
