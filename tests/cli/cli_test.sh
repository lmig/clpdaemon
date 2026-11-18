#!/bin/sh
# CLI and configuration tests for clpdaemon (T2, docs/configuration.md).
# usage: cli_test.sh <path-to-clpdaemon>
set -u

BIN=${1:?usage: cli_test.sh <path-to-clpdaemon>}
case $BIN in /*) ;; *) BIN=$PWD/$BIN ;; esac
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 2

USAGE='usage: clpdaemon --config <file> [--foreground] [--check]'
NL='
'
failures=0
cases=0

# check <name> <expected-exit> <expected-stdout> <expected-stderr> -- <args...>
check() {
  name=$1 want_rc=$2 want_out=$3 want_err=$4
  shift 5
  cases=$((cases + 1))
  "$BIN" "$@" >out.txt 2>err.txt
  rc=$?
  # The sentinel keeps trailing newlines visible to the comparison: every
  # expected non-empty stream ends with exactly one newline.
  out=$(cat out.txt; printf x) err=$(cat err.txt; printf x)
  out=${out%x} err=${err%x}
  want_out=${want_out:+$want_out$NL} want_err=${want_err:+$want_err$NL}
  if [ "$rc" != "$want_rc" ] || [ "$out" != "$want_out" ] || [ "$err" != "$want_err" ]; then
    failures=$((failures + 1))
    printf 'FAIL: %s\n  exit:   got %s, want %s\n  stdout: got [%s]\n          want [%s]\n  stderr: got [%s]\n          want [%s]\n' \
      "$name" "$rc" "$want_rc" "$out" "$want_out" "$err" "$want_err"
  fi
}

conf() { printf '%s\n' "$2" >"$1"; }

# --- command line -----------------------------------------------------------
conf ok.conf 'node_id = n1'
check "help" 0 "$USAGE" "" -- --help
check "missing --config" 2 "" "clpdaemon: missing required option --config$NL$USAGE" -- --check
check "--config without value" 2 "" "clpdaemon: option --config requires a file$NL$USAGE" -- --config
check "unknown option" 2 "" "clpdaemon: unknown option '--bogus'$NL$USAGE" -- --config ok.conf --bogus
check "check ok" 0 "clpdaemon: ok.conf: configuration OK" "" -- --config ok.conf --check
check "foreground accepted" 0 "clpdaemon: ok.conf: configuration OK" "" -- --foreground --check --config ok.conf
check "cannot open" 1 "" "clpdaemon: nope.conf: cannot open: No such file or directory" -- --config nope.conf --check
mkdir confdir
check "cannot read (directory)" 1 "" "clpdaemon: confdir: cannot read: Is a directory" -- --config confdir --check
check "repeated --config" 2 "" "clpdaemon: option --config given more than once$NL$USAGE" -- --config ok.conf --config ok.conf --check

# --- full valid file (requirements section 7) ---------------------------------
# A minimal lint-clean rule package used by the cases below.
mkdir pkg
conf pkg/package.conf 'definition = fibre-installation
version = v3
files = rules.clp'
conf pkg/rules.clp '(deftemplate order (slot wf-id) (slot status))
(defrule start ?o <- (order (wf-id ?w) (status new)) => (modify ?o (status started)))
(deffacts seed (order (wf-id w1) (status new)))'
conf full.conf '# sample
node_id                  = n1
tick_interval_ms         = 5000
cycle_min_interval_ms    = 200

max_rules_per_cycle      = -1
safety_poll_interval_ms  = 5000
idle_eviction_after      = 2h
admin_socket             = /run/clpdaemon/admin.sock
trace_text               = /var/log/clpdaemon/trace.log
trace_jsonl              =
watch                    = rules
packages                 = pkg
plugins                  = /usr/lib/clpdaemon/clpd-orm.so, /usr/lib/clpdaemon/clpd-zmq.so'
check "full valid file" 0 "clpdaemon: full.conf: configuration OK" "" -- --config full.conf --check
conf equal.conf 'node_id = n1
tick_interval_ms = 1000
cycle_min_interval_ms = 1000
cycle_min_interval_ms_dummy_absent_check = x'
# equal intervals are valid; the unknown key line keeps this case honest about reporting
check "equal intervals allowed, unknown key rejected" 1 "" \
  "clpdaemon: equal.conf:4: unknown key 'cycle_min_interval_ms_dummy_absent_check'" -- --config equal.conf --check
conf zero.conf 'node_id = n1
cycle_min_interval_ms = 0
watch =
packages ='
check "zero min interval and empty lists" 0 "clpdaemon: zero.conf: configuration OK" "" -- --config zero.conf --check

# --- rule packages (docs/packages.md) -------------------------------------------
conf nopkg.conf 'node_id = n1
packages = pkg, missing'
check "package directory without manifest" 1 "" \
  "clpdaemon: missing/package.conf: cannot open: No such file or directory" -- --config nopkg.conf --check
mkdir gone
conf gone/package.conf 'definition = gone
version = 1
files = a.clp'
conf gone.conf 'node_id = n1
packages = gone'
check "package file missing" 1 "" \
  "clpdaemon: gone/a.clp: cannot open: No such file or directory" -- --config gone.conf --check
mkdir dm
conf dm/package.conf 'definition = dm
version = 1
files = m.clp'
conf dm/m.clp '(defmodule other)'
conf dm.conf 'node_id = n1
packages = dm'
check "defmodule in a package file" 1 "" \
  "clpdaemon: dm/m.clp: defmodule is not allowed in package files" -- --config dm.conf --check
conf twice.conf 'node_id = n1
packages = pkg, ./pkg'
check "same package twice" 1 "" \
  "clpdaemon: ./pkg: package fibre-installation@v3 is already loaded from pkg" -- --config twice.conf --check
# A failing package does not stop the next one from being checked.
conf both.conf 'node_id = n1
packages = gone, dm'
check "errors of every package" 1 "" \
  "clpdaemon: gone/a.clp: cannot open: No such file or directory
clpdaemon: dm/m.clp: defmodule is not allowed in package files" -- --config both.conf --check

# --- syntax -------------------------------------------------------------------
conf syntax.conf 'node_id = n1
just a line
tick_interval_ms = 1000
foo = 1
tick_interval_ms = 2000'
check "syntax, unknown and duplicate keys" 1 "" \
  "clpdaemon: syntax.conf:2: expected 'key = value'
clpdaemon: syntax.conf:4: unknown key 'foo'
clpdaemon: syntax.conf:5: duplicate key 'tick_interval_ms' (first set on line 3)" -- --config syntax.conf --check

conf emptykey.conf 'node_id = n1
 = 5'
check "empty key" 1 "" "clpdaemon: emptykey.conf:2: expected 'key = value'" -- --config emptykey.conf --check

printf 'node_id = n1\ntick_interval_ms = 1\0000\n' >nul.conf
check "NUL byte in a line" 1 "" "clpdaemon: nul.conf:2: line contains a NUL byte" -- --config nul.conf --check

conf missing.conf '# empty'
check "node_id required" 1 "" "clpdaemon: missing.conf: missing required key 'node_id'" -- --config missing.conf --check

# --- values -------------------------------------------------------------------
conf values.conf 'node_id = a b
tick_interval_ms = 0
cycle_min_interval_ms = -1
max_rules_per_cycle = 0
safety_poll_interval_ms = 12x
idle_eviction_after = 30d
admin_socket =
watch = facts,agenda
packages = /a,,/b
plugins = 2147483648'
check "invalid values" 1 "" \
  "clpdaemon: values.conf:1: node_id: expected 1-64 characters from [A-Za-z0-9._-], got 'a b'
clpdaemon: values.conf:2: tick_interval_ms: expected a positive integer, got '0'
clpdaemon: values.conf:3: cycle_min_interval_ms: expected a non-negative integer, got '-1'
clpdaemon: values.conf:4: max_rules_per_cycle: expected -1 or a positive integer, got '0'
clpdaemon: values.conf:5: safety_poll_interval_ms: expected a positive integer, got '12x'
clpdaemon: values.conf:6: idle_eviction_after: expected a duration such as 30s, 30m or 2h, got '30d'
clpdaemon: values.conf:7: admin_socket: expected a path
clpdaemon: values.conf:8: watch: unknown item 'agenda' (expected facts, rules)
clpdaemon: values.conf:9: packages: empty list item" -- --config values.conf --check

conf range.conf 'node_id = n1
tick_interval_ms = 2147483648
max_rules_per_cycle = 2147483647
idle_eviction_after = 0m'
check "integer range and zero duration" 1 "" \
  "clpdaemon: range.conf:2: tick_interval_ms: expected a positive integer, got '2147483648'
clpdaemon: range.conf:4: idle_eviction_after: expected a duration such as 30s, 30m or 2h, got '0m'" -- --config range.conf --check

long_id=$(printf 'a%.0s' $(seq 1 65))
conf long.conf "node_id = $long_id"
check "node_id longer than 64" 1 "" \
  "clpdaemon: long.conf:1: node_id: expected 1-64 characters from [A-Za-z0-9._-], got '$long_id'" -- --config long.conf --check

conf order.conf 'node_id = n1
tick_interval_ms = 100
cycle_min_interval_ms = 300'
check "min interval above tick" 1 "" \
  "clpdaemon: order.conf: cycle_min_interval_ms (300) must not exceed tick_interval_ms (100)" -- --config order.conf --check
conf order2.conf 'node_id = n1
cycle_min_interval_ms = 6000'
check "min interval above default tick" 1 "" \
  "clpdaemon: order2.conf: cycle_min_interval_ms (6000) must not exceed tick_interval_ms (5000)" -- --config order2.conf --check

# Errors are reported even when a run is requested without --check.
check "run validates" 1 "" "clpdaemon: missing.conf: missing required key 'node_id'" -- --config missing.conf

# --- running daemon and signals (FR-5, L25) ---------------------------------
# wait_line <file> <line>: waits up to 5 s for an exact line in <file>.
wait_line() {
  n=0
  while ! grep -qxF "$2" "$1" 2>/dev/null; do
    n=$((n + 1))
    [ "$n" -gt 100 ] && return 1
    sleep 0.05
  done
}

# wait_exit <pid>: waits up to 5 s for the process to exit and sets rc to its
# exit status; a process still running is killed and rc is set to "hung".
wait_exit() {
  n=0
  while kill -0 "$1" 2>/dev/null; do
    n=$((n + 1))
    if [ "$n" -gt 100 ]; then
      kill -KILL "$1" 2>/dev/null
      wait "$1" 2>/dev/null
      rc=hung
      return
    fi
    sleep 0.05
  done
  wait "$1"
  rc=$?
}

# daemon <name> <conf> <signals> <expected-stderr> [ignore-int]: starts the
# daemon (node_id must be n1), waits until it is running, sends each signal of
# the space-separated list (waiting after SIGHUP for its line), and expects
# exit 0, empty stdout and the exact stderr. With ignore-int the daemon is
# started with SIGINT ignored, as a non-interactive shell does for background
# jobs.
daemon() {
  name=$1 dconf=$2 sigs=$3 want_err=$4 mode=${5:-}
  cases=$((cases + 1))
  if [ "$mode" = ignore-int ]; then
    sh -c 'trap "" INT; exec "$0" --config "$1"' "$BIN" "$dconf" >out.txt 2>err.txt &
  else
    "$BIN" --config "$dconf" >out.txt 2>err.txt &
  fi
  pid=$!
  ok=1
  wait_line err.txt "clpdaemon: n1: running" || ok=0
  for s in $sigs; do
    kill -"$s" "$pid" 2>/dev/null
    if [ "$s" = HUP ]; then
      wait_line err.txt "clpdaemon: received SIGHUP, reopening trace files" || ok=0
    fi
  done
  wait_exit "$pid"
  out=$(cat out.txt; printf x) err=$(cat err.txt; printf x)
  out=${out%x} err=${err%x}
  if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ "$out" != "" ] || [ "$err" != "$want_err$NL" ]; then
    failures=$((failures + 1))
    printf 'FAIL: %s\n  exit: got %s, want 0 (lines seen: %s)\n  stdout: got [%s]\n  stderr: got [%s]\n          want [%s]\n' \
      "$name" "$rc" "$ok" "$out" "$err" "$want_err"
  fi
}

conf fast.conf 'node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10'
daemon "SIGTERM stops gracefully" fast.conf TERM "clpdaemon: n1: running
clpdaemon: received SIGTERM, stopping"
daemon "SIGINT stops gracefully" fast.conf INT "clpdaemon: n1: running
clpdaemon: received SIGINT, stopping"
conf fastpkg.conf 'node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
packages = pkg'
daemon "running with a package" fastpkg.conf TERM "clpdaemon: n1: running
clpdaemon: received SIGTERM, stopping"
daemon "SIGINT with SIGINT inherited as ignored" fast.conf INT "clpdaemon: n1: running
clpdaemon: received SIGINT, stopping" ignore-int
daemon "SIGHUP keeps running" fast.conf "HUP TERM" "clpdaemon: n1: running
clpdaemon: received SIGHUP, reopening trace files
clpdaemon: received SIGTERM, stopping"

printf '%d/%d CLI cases passed\n' $((cases - failures)) "$cases"
[ "$failures" -eq 0 ]
