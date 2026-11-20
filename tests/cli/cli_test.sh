#!/bin/sh
# CLI and configuration tests for clpdaemon (T2, docs/configuration.md).
# usage: cli_test.sh <path-to-clpdaemon> <plugin-dir> <plugin-desc-size> <admin-client>
# <plugin-dir> holds the example and test plugins (<build>/plugins);
# <plugin-desc-size> prints sizeof(clpd_plugin) of this build;
# <admin-client> talks to the admin socket (tests/admin/admin_client.c).
set -u

U='usage: cli_test.sh <path-to-clpdaemon> <plugin-dir> <plugin-desc-size> <admin-client>'
BIN=${1:?$U}
PLUGINS=${2:?$U}
DESC_SIZE=$("${3:?$U}") || exit 2
ADMIN=${4:?$U}
case $BIN in /*) ;; *) BIN=$PWD/$BIN ;; esac
case $PLUGINS in /*) ;; *) PLUGINS=$PWD/$PLUGINS ;; esac
case $ADMIN in /*) ;; *) ADMIN=$PWD/$ADMIN ;; esac
WORK=$(mktemp -d)
# A background child left by a case (ypid) is killed even on failure.
trap 'if [ -s "$WORK/ypid" ]; then kill -KILL "$(cat "$WORK/ypid")" 2>/dev/null; fi; rm -rf "$WORK"' EXIT
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
  # RUN (default empty) bounds cases that would start a daemon on a
  # regression, e.g. RUN='timeout 10'; a timeout then fails with exit 124.
  ${RUN:-} "$BIN" "$@" >out.txt 2>err.txt
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
# rconf: a configuration for a running daemon, with its admin socket in the
# work directory (the default /run/clpdaemon/admin.sock may not exist).
rconf() { printf '%s\nadmin_socket = admin.sock\n' "$2" >"$1"; }

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
trace_text               = full-trace.log
trace_jsonl              =
watch                    = rules
packages                 = pkg
plugins                  = '"$PLUGINS/clpd_example.so, $PLUGINS/test_hooks_a.so"
check "full valid file" 0 "clpdaemon: full.conf: configuration OK" "clpdaemon: example: init
clpdaemon: hooks-a: init node_id=n1 post_fact=-1 EINVAL
clpdaemon: hooks-a: shutdown
clpdaemon: example: shutdown" -- --config full.conf --check
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
mkdir lint
conf lint/package.conf 'definition = lint
version = 1
files = l.clp
lint_exempt = template:catalog'
conf lint/l.clp '(deftemplate catalog (slot code))
(deftemplate order (slot wf-id) (slot code))
(deftemplate item (slot owner))
(defrule r (order (wf-id ?w) (code ?c)) (catalog (code ?c)) (item (owner ?w)) =>)'
conf lint.conf 'node_id = n1
packages = lint'
check "wf-id lint errors" 1 "" \
  "clpdaemon: lint@1: lint: template 'item' has no wf-id slot
clpdaemon: lint@1: lint: rule 'r': pattern 3 (item) does not bind wf-id to a variable" -- --config lint.conf --check
# Every construct of a package file must be in the package's module
# (T9e, L87): a rule planted in another package's module, or a function in
# MAIN, is a load error, also when the file switches back to its own
# module.
mkdir plant
conf plant/package.conf 'definition = plant
version = 1
files = p.clp'
conf plant/p.clp '(deftemplate go (slot wf-id))
(defrule fibre-installation@v3::planted (order (wf-id ?w)) => )
(deffunction MAIN::helper () 1)
(defrule plant@1::back (go (wf-id ?w)) => )'
conf plant.conf 'node_id = n1
packages = pkg, plant'
check "construct outside the package module" 1 "" \
  "clpdaemon: plant/p.clp: defrule 'fibre-installation@v3::planted' is outside the package module plant@1
clpdaemon: plant/p.clp: deffunction 'MAIN::helper' is outside the package module plant@1" -- --config plant.conf --check
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
  # Truncated here, not only by the background child's redirection, so the
  # wait below never reads a stale running line from the previous case.
  : >out.txt
  : >err.txt
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

rconf fast.conf 'node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10'
daemon "SIGTERM stops gracefully" fast.conf TERM "clpdaemon: n1: running
clpdaemon: received SIGTERM, stopping"
daemon "SIGINT stops gracefully" fast.conf INT "clpdaemon: n1: running
clpdaemon: received SIGINT, stopping"
rconf fastpkg.conf 'node_id = n1
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

# --- trace files (FR-2, FR-5, L48) -------------------------------------------
# fail <name> <detail>: records a failed custom case.
fail() {
  failures=$((failures + 1))
  printf 'FAIL: %s\n  %s\n' "$1" "$2"
}

conf trnodir.conf 'node_id = n1
trace_text = nodir/t.log'
check "trace cannot open" 1 "" "clpdaemon: nodir/t.log: cannot open: No such file or directory" -- --config trnodir.conf --check
RUN='timeout 10'
check "run opens the trace first" 1 "" "clpdaemon: nodir/t.log: cannot open: No such file or directory" -- --config trnodir.conf
RUN=
conf trdir.conf 'node_id = n1
trace_jsonl = confdir'
check "trace path is a directory" 1 "" "clpdaemon: confdir: cannot open: Is a directory" -- --config trdir.conf --check
conf trok.conf 'node_id = n1
trace_text = tr.log
trace_jsonl = tr.jsonl'
check "check opens the trace files" 0 "clpdaemon: trok.conf: configuration OK" "" -- --config trok.conf --check
cases=$((cases + 1))
[ -f tr.log ] && [ ! -s tr.log ] && [ -f tr.jsonl ] && [ ! -s tr.jsonl ] ||
  fail "check creates empty trace files" "$(ls -l tr.log tr.jsonl 2>&1)"

# wait_has <file> <text>: waits up to 5 s for <text> anywhere in <file>.
wait_has() {
  n=0
  while ! grep -qF "$2" "$1" 2>/dev/null; do
    n=$((n + 1))
    [ "$n" -gt 100 ] && return 1
    sleep 0.05
  done
}

# size <file>: byte size, 0 when missing.
size() { if [ -f "$1" ]; then wc -c <"$1" | tr -d ' '; else echo 0; fi; }

# wait_grows <file> <bytes>: waits up to 5 s for <file> to exceed <bytes>.
wait_grows() {
  n=0
  while [ "$(size "$1")" -le "$2" ]; do
    n=$((n + 1))
    [ "$n" -gt 100 ] && return 1
    sleep 0.05
  done
}

# The engine is traced from Reset on (T5b): the deffacts assert, then the
# firing of rule start and its modify as one event. Timestamps are stripped.
rconf trpkg.conf 'node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
packages = pkg
trace_text = run.log
trace_jsonl = run.jsonl'
cases=$((cases + 1))
"$BIN" --config trpkg.conf >out.txt 2>err.txt &
pid=$!
ok=1
wait_line err.txt "clpdaemon: n1: running" || ok=0
wait_has run.jsonl '"event":"modify"' || ok=0
kill -TERM "$pid"
wait_exit "$pid"
want_text="n1 w1 FACT+ f-1 (order (wf-id w1) (status new))
n1 w1 FIRE fibre-installation@v3::start f-1
n1 w1 FACT~ f-1 (order (wf-id w1) (status started))"
want_jsonl='{"node_id":"n1","wf_id":"w1","event":"assert","fact":1,"template":"order","text":"(order (wf-id w1) (status new))"}
{"node_id":"n1","wf_id":"w1","event":"fire","module":"fibre-installation@v3","rule":"start","facts":[1]}
{"node_id":"n1","wf_id":"w1","event":"modify","fact":1,"template":"order","text":"(order (wf-id w1) (status started))"}'
got_text=$(cut -d' ' -f2- run.log)
got_jsonl=$(sed 's/"ts":"[^"]*",//' run.jsonl)
want="clpdaemon: n1: running
clpdaemon: received SIGTERM, stopping"
if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ -s out.txt ] || [ "$(cat err.txt)" != "$want" ] ||
  [ "$got_text" != "$want_text" ] || [ "$got_jsonl" != "$want_jsonl" ]; then
  fail "trace of Reset and a rule firing" "exit $rc, lines seen $ok, stdout [$(cat out.txt)], stderr [$(cat err.txt)]
  text [$got_text]
  jsonl [$got_jsonl]"
fi

# SIGHUP after rotation: the moved text file is recreated and receives the
# next lines; the JSONL file, whose directory was moved away, cannot be
# reopened, so its old stream (now jdir.1/t.jsonl) keeps receiving lines and
# the failure is reported. A rule firing once per cycle keeps both busy.
mkdir rot jdir tick
conf tick/package.conf 'definition = tick
version = 1
files = t.clp'
conf tick/t.clp '(deftemplate counter (slot wf-id) (slot n))
(defrule step ?c <- (counter (wf-id ?w) (n ?n)) => (modify ?c (n (+ ?n 1))))
(deffacts seed (counter (wf-id c1) (n 0)))'
rconf trhup.conf 'node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
max_rules_per_cycle = 1
packages = tick
trace_text = rot/t.log
trace_jsonl = jdir/t.jsonl'
cases=$((cases + 1))
"$BIN" --config trhup.conf >out.txt 2>err.txt &
pid=$!
ok=1
wait_line err.txt "clpdaemon: n1: running" || ok=0
wait_has rot/t.log 'FACT~' || ok=0
mv rot/t.log rot/t.log.1 && mv jdir jdir.1
kill -HUP "$pid"
wait_line err.txt "clpdaemon: jdir/t.jsonl: cannot reopen: No such file or directory" || ok=0
before=$(size jdir.1/t.jsonl)
wait_grows rot/t.log 0 || ok=0
wait_grows jdir.1/t.jsonl "$before" || ok=0
kill -TERM "$pid"
wait_exit "$pid"
want="clpdaemon: n1: running
clpdaemon: received SIGHUP, reopening trace files
clpdaemon: jdir/t.jsonl: cannot reopen: No such file or directory
clpdaemon: received SIGTERM, stopping"
if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ -s out.txt ] || [ "$(cat err.txt)" != "$want" ] ||
  ! grep -q 'FIRE tick@1::step f-1' rot/t.log || ! grep -q 'FIRE tick@1::step f-1' rot/t.log.1 ||
  [ -e jdir ]; then
  fail "SIGHUP reopens the trace files" "exit $rc, lines seen $ok, stdout [$(cat out.txt)], stderr [$(cat err.txt)], files [$(ls -l rot jdir.1)]"
fi

# With a trace, fds 1 and 2 go to pipes whose lines become stdout/stderr
# output events (FR-16, L48): a child started by CLIPS system writes to
# them, while the daemon messages stay on the original stderr.
mkdir sys
conf sys/package.conf 'definition = sys
version = 1
files = s.clp'
conf sys/s.clp '(defrule shell => (system "echo out-child; echo err-child >&2"))'
rconf trsys.conf 'node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
packages = sys
trace_text = sys.log
trace_jsonl = sys.jsonl'
cases=$((cases + 1))
"$BIN" --config trsys.conf >out.txt 2>err.txt &
pid=$!
ok=1
wait_line err.txt "clpdaemon: n1: running" || ok=0
wait_has sys.log 'stdout out-child' || ok=0
wait_has sys.log 'stderr err-child' || ok=0
kill -TERM "$pid"
wait_exit "$pid"
got_text=$(cut -d' ' -f2- sys.log | LC_ALL=C sort)
got_jsonl=$(sed 's/"ts":"[^"]*",//' sys.jsonl | LC_ALL=C sort)
want_text='n1 - FIRE sys@1::shell
n1 - stderr err-child
n1 - stdout out-child'
want_jsonl='{"node_id":"n1","event":"fire","module":"sys@1","rule":"shell","facts":[]}
{"node_id":"n1","event":"output","stream":"stderr","text":"err-child"}
{"node_id":"n1","event":"output","stream":"stdout","text":"out-child"}'
want="clpdaemon: n1: running
clpdaemon: received SIGTERM, stopping"
if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ -s out.txt ] || [ "$(cat err.txt)" != "$want" ] ||
  [ "$got_text" != "$want_text" ] || [ "$got_jsonl" != "$want_jsonl" ]; then
  fail "process stdout and stderr go to the trace" "exit $rc, lines seen $ok, stdout [$(cat out.txt)], stderr [$(cat err.txt)]
  text [$got_text]
  jsonl [$got_jsonl]"
fi

# Without a trace nothing is redirected: the child writes to the console.
rconf nosys.conf 'node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
packages = sys'
cases=$((cases + 1))
"$BIN" --config nosys.conf >out.txt 2>err.txt &
pid=$!
ok=1
wait_line err.txt "clpdaemon: n1: running" || ok=0
wait_line err.txt "err-child" || ok=0
wait_line out.txt "out-child" || ok=0
kill -TERM "$pid"
wait_exit "$pid"
if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ "$(cat out.txt)" != out-child ] ||
  [ "$(grep -cv '^clpdaemon: ' err.txt)" != 1 ]; then
  fail "no trace: process output on the console" "exit $rc, lines seen $ok, stdout [$(cat out.txt)], stderr [$(cat err.txt)]"
fi

# S5/T5c D2: a busy background child (it inherits the blocked SIGTERM)
# must not hang stop; once the daemon is gone it gets SIGPIPE or EPIPE.
mkdir yes
conf yes/package.conf 'definition = yes
version = 1
files = y.clp'
conf yes/y.clp '(defrule busy => (system "yes busy & echo $! > ypid"))'
rconf tryes.conf 'node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
packages = yes
trace_text = yes.log'
cases=$((cases + 1))
"$BIN" --config tryes.conf >out.txt 2>err.txt &
pid=$!
ok=1
wait_line err.txt "clpdaemon: n1: running" || ok=0
wait_has yes.log 'stdout busy' || ok=0
kill -TERM "$pid"
wait_exit "$pid"
# The child must end by itself shortly after the daemon (no reader left).
child=gone
if [ -s ypid ]; then
  n=0
  while kill -0 "$(cat ypid)" 2>/dev/null; do
    n=$((n + 1))
    if [ "$n" -gt 100 ]; then child=running; break; fi
    sleep 0.05
  done
  kill -KILL "$(cat ypid)" 2>/dev/null
fi
before=$(size yes.log)
sleep 0.2
after=$(size yes.log)
want="clpdaemon: n1: running
clpdaemon: received SIGTERM, stopping"
if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ -s out.txt ] || [ "$(cat err.txt)" != "$want" ] ||
  [ "$child" != gone ] || [ "$before" != "$after" ]; then
  fail "busy background child does not hang stop" "exit $rc, lines seen $ok, child $child, trace $before -> $after bytes, stdout [$(cat out.txt)], stderr [$(cat err.txt)]"
fi
: >ypid

# L52 R4: while the trace cannot be written, CLIPS STDERR lines also go to
# the daemon log stream, the original stderr (fds 1 and 2 are captured).
mkdir errpkg
conf errpkg/package.conf 'definition = err
version = 1
files = e.clp'
conf errpkg/e.clp '(defrule warn => (printout stderr "engine-error" crlf) (printout t "engine-out" crlf))'
rconf trfull.conf 'node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
packages = errpkg
trace_text = /dev/full'
cases=$((cases + 1))
"$BIN" --config trfull.conf >out.txt 2>err.txt &
pid=$!
ok=1
wait_line err.txt "clpdaemon: STDERR: engine-error" || ok=0
kill -TERM "$pid"
wait_exit "$pid"
want="clpdaemon: n1: running
clpdaemon: /dev/full: cannot write: No space left on device
clpdaemon: STDERR: engine-error
clpdaemon: received SIGTERM, stopping"
if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ -s out.txt ] || [ "$(cat err.txt)" != "$want" ]; then
  fail "failing trace: engine errors on stderr" "exit $rc, lines seen $ok, stdout [$(cat out.txt)], stderr [$(cat err.txt)]"
fi

# T5d A2: router lines at the 65536-byte cap. A 65535-byte line is one
# event; 65536 bytes then "a" give a 65536-byte event and "a".
mkdir cappkg
conf cappkg/package.conf 'definition = cap
version = 1
files = c.clp'
conf cappkg/c.clp '(defrule big => (bind ?s "x") (loop-for-count 16 (bind ?s (str-cat ?s ?s))) (printout t (sub-string 1 65535 ?s) crlf) (printout t ?s "a" crlf))'
rconf trcap.conf 'node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
packages = cappkg
trace_text = t.txt'
cases=$((cases + 1))
"$BIN" --config trcap.conf >out.txt 2>err.txt &
pid=$!
ok=1
wait_line err.txt "clpdaemon: n1: running" || ok=0
wait_has t.txt ' - STDOUT a' || ok=0
kill -TERM "$pid"
wait_exit "$pid"
got=$(grep -F ' STDOUT ' t.txt | sed 's/^[^ ]* n1 - STDOUT //' | awk '{ printf "%s%d", sep, length($0); sep = "," }')
last=$(grep -F ' STDOUT ' t.txt | tail -n 1 | sed 's/^[^ ]* n1 - STDOUT //')
want="clpdaemon: n1: running
clpdaemon: received SIGTERM, stopping"
if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ -s out.txt ] || [ "$(cat err.txt)" != "$want" ] ||
  [ "$got" != "65535,65536,1" ] || [ "$last" != a ]; then
  fail "router line cap" "exit $rc, lines seen $ok, lengths [$got], last [$last], stderr [$(cat err.txt)]"
fi

# --- plugins (FR-17, FR-18, FR-19, L59) --------------------------------------
# RUN bounds every case: a regression must not leave a daemon running.
EX=$PLUGINS/clpd_example.so
mkdir udfpkg
conf udfpkg/package.conf 'definition = udf
version = 1
files = u.clp'
conf udfpkg/u.clp '(deftemplate job (slot wf-id) (slot n))
(deffacts jobs (job (wf-id w1) (n 2)) (job (wf-id w2) (n x)))
(defrule add (job (wf-id ?w) (n ?n&:(integerp ?n))) => (printout t "sum " (clpd-example-add ?n 3) " " (clpd-example-greet ?w) crlf))
(defrule bad ?j <- (job (wf-id ?w) (n ?n&:(symbolp ?n))) => (retract ?j) (bind ?x (clpd-example-add ?n 3)))'
conf plugcheck.conf "node_id = n1
plugins = $EX
packages = udfpkg"
RUN='timeout 10' check "check with a plugin and a package calling its functions" 0 \
  "clpdaemon: plugcheck.conf: configuration OK" "clpdaemon: example: init
clpdaemon: example: shutdown" -- --config plugcheck.conf --check
conf nopluguse.conf 'node_id = n1
packages = udfpkg'
cases=$((cases + 1))
timeout 10 "$BIN" --config nopluguse.conf --check >out.txt 2>err.txt
rc=$?
if [ "$rc" != 1 ] || [ -s out.txt ] || ! grep -qF 'clpdaemon: udf@1: clips: ' err.txt ||
  ! grep -qF 'clpd-example-add' err.txt; then
  fail "package calling a plugin function without the plugin" "exit $rc, stdout [$(cat out.txt)], stderr [$(cat err.txt)]"
fi

# plugerr <name> <plugins-value> <expected-stderr>: --check fails with exit 1.
plugerr() {
  conf plugerr.conf "node_id = n1
plugins = $2"
  RUN='timeout 10' check "$1" 1 "" "$3" -- --config plugerr.conf --check
}
plugerr "plugin path without a slash" "clpd_example.so" \
  "clpdaemon: clpd_example.so: plugin path must contain a '/'"
plugerr "plugin cannot be loaded" "./nope.so" \
  "clpdaemon: ./nope.so: cannot load: ./nope.so: cannot open shared object file: No such file or directory"
plugerr "plugin without clpd_plugin_get" "$PLUGINS/test_bad_nosym.so" \
  "clpdaemon: $PLUGINS/test_bad_nosym.so: missing clpd_plugin_get"
plugerr "clpd_plugin_get returns NULL" "$PLUGINS/test_bad_null.so" \
  "clpdaemon: $PLUGINS/test_bad_null.so: clpd_plugin_get returned NULL"
plugerr "plugin ABI mismatch" "$PLUGINS/test_bad_abi.so" \
  "clpdaemon: $PLUGINS/test_bad_abi.so: ABI version 2, expected 1"
plugerr "plugin descriptor too small" "$PLUGINS/test_bad_size.so" \
  "clpdaemon: $PLUGINS/test_bad_size.so: descriptor size 8, expected at least $DESC_SIZE"
# The daemon exports the CLIPS API only, not its clpd_* internals (T6b).
plugerr "plugin using a daemon internal" "$PLUGINS/test_bad_internal.so" \
  "clpdaemon: $PLUGINS/test_bad_internal.so: cannot load: $PLUGINS/test_bad_internal.so: undefined symbol: clpd_log"
plugerr "invalid plugin name" "$PLUGINS/test_bad_name.so" \
  "clpdaemon: $PLUGINS/test_bad_name.so: plugin name: expected 1-64 characters from [A-Za-z0-9._-], got 'bad name'"
plugerr "plugin without init" "$PLUGINS/test_bad_noinit.so" \
  "clpdaemon: $PLUGINS/test_bad_noinit.so: descriptor has no init"
plugerr "same plugin path twice" "$EX, $EX" \
  "clpdaemon: example: init
clpdaemon: $EX: plugin path listed more than once
clpdaemon: example: shutdown"
plugerr "same plugin under another path" "$EX, $PLUGINS/./clpd_example.so" \
  "clpdaemon: example: init
clpdaemon: $PLUGINS/./clpd_example.so: plugin example is already loaded from $EX
clpdaemon: example: shutdown"
plugerr "failing init shuts down the others in reverse order" \
  "$PLUGINS/test_hooks_a.so, $PLUGINS/test_hooks_b.so, $PLUGINS/test_fail_init.so, $EX" \
  "clpdaemon: hooks-a: init node_id=n1 post_fact=-1 EINVAL
clpdaemon: hooks-b: init node_id=n1 post_fact=-1 EINVAL
clpdaemon: fail-init: init node_id=n1 post_fact=-1 EINVAL
clpdaemon: $PLUGINS/test_fail_init.so: init failed
clpdaemon: hooks-b: shutdown
clpdaemon: hooks-a: shutdown"
conf plugpkg.conf "node_id = n1
plugins = $EX
packages = nopkg"
RUN='timeout 10' check "package error after plugins shuts them down" 1 "" "clpdaemon: example: init
clpdaemon: nopkg/package.conf: cannot open: No such file or directory
clpdaemon: example: shutdown" -- --config plugpkg.conf
# Teardown order (T6b): shutdown, the CLIPS environment, then dlclose.
conf plugmark.conf "node_id = n1
plugins = $PLUGINS/test_markers.so"
export CLPD_TEST_MARKERS="$WORK/markers.txt"
RUN='timeout 10' check "check unloads after the environment" 0 \
  "clpdaemon: plugmark.conf: configuration OK" "clpdaemon: markers: init node_id=n1 post_fact=-1 EINVAL
clpdaemon: markers: shutdown" -- --config plugmark.conf --check
unset CLPD_TEST_MARKERS
cases=$((cases + 1))
if [ "$(cat markers.txt 2>/dev/null)" != "shutdown${NL}env-cleanup${NL}destructor" ]; then
  fail "unload order shutdown, env cleanup, destructor" "markers [$(cat markers.txt 2>/dev/null)]"
fi
conf plugabi.conf "node_id = n1
plugins = $PLUGINS/test_bad_abi.so"
RUN='timeout 10' check "plugin error aborts a run too" 1 "" \
  "clpdaemon: $PLUGINS/test_bad_abi.so: ABI version 2, expected 1" -- --config plugabi.conf

# plugdaemon <name> <conf> <line> <expected-stderr>: starts the daemon, waits
# for "running" and then for <line> on stderr, stops it with SIGTERM and
# expects exit 0, empty stdout and the exact stderr.
plugdaemon() {
  cases=$((cases + 1))
  : >out.txt # truncated before the start, as in daemon()
  : >err.txt
  "$BIN" --config "$2" >out.txt 2>err.txt &
  pid=$!
  ok=1
  wait_line err.txt "clpdaemon: n1: running" || ok=0
  wait_line err.txt "$3" || ok=0
  kill -TERM "$pid"
  wait_exit "$pid"
  if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ -s out.txt ] || [ "$(cat err.txt)" != "$4" ]; then
    fail "$1" "exit $rc, lines seen $ok, stdout [$(cat out.txt)], stderr [$(cat err.txt)]"
  fi
}
rconf plugrun.conf "node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
plugins = $EX
packages = udfpkg
trace_text = plug.log"
plugdaemon "running daemon calls the plugin hooks" plugrun.conf \
  "clpdaemon: example: cycle_end 2 ok=1" "clpdaemon: example: init
clpdaemon: example: after_load
clpdaemon: n1: running
clpdaemon: example: cycle_begin 1
clpdaemon: example: cycle_end 1 ok=0
clpdaemon: example: cycle_end 2 ok=1
clpdaemon: received SIGTERM, stopping
clpdaemon: example: shutdown"
cases=$((cases + 1))
if ! grep -q ' n1 w1 STDOUT sum 5 hello, w1$' plug.log ||
  ! grep -qF 'clpd-example-add' plug.log; then
  fail "rules call the plugin functions" "trace [$(cat plug.log)]"
fi
rconf plugorder.conf "node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
plugins = $PLUGINS/test_hooks_a.so, $PLUGINS/test_hooks_b.so"
plugdaemon "hooks in configured order, shutdown in reverse" plugorder.conf \
  "clpdaemon: hooks-b: cycle_end ok=1" "clpdaemon: hooks-a: init node_id=n1 post_fact=-1 EINVAL
clpdaemon: hooks-b: init node_id=n1 post_fact=-1 EINVAL
clpdaemon: hooks-a: after_load
clpdaemon: hooks-b: after_load
clpdaemon: n1: running
clpdaemon: hooks-a: cycle_begin
clpdaemon: hooks-b: cycle_begin
clpdaemon: hooks-a: cycle_end ok=1
clpdaemon: hooks-b: cycle_end ok=1
clpdaemon: received SIGTERM, stopping
clpdaemon: hooks-b: shutdown
clpdaemon: hooks-a: shutdown"

# cycle_end ok (T6b, L59): 0 only when Run stopped on a CLIPS evaluation
# error; a Run cut by max_rules_per_cycle or halted by SIGTERM is ok.
mkdir throwpkg looppkg
conf throwpkg/package.conf 'definition = throw
version = 1
files = t.clp'
conf throwpkg/t.clp '(deftemplate job (slot wf-id))
(deffacts jobs (job (wf-id w1)))
(defrule boom (job (wf-id ?w)) => (hooks-a-throw))'
conf looppkg/package.conf 'definition = loop
version = 1
files = l.clp'
conf looppkg/l.clp '(deftemplate counter (slot wf-id) (slot n))
(deffacts seed (counter (wf-id c1) (n 0)))
(defrule step ?c <- (counter (wf-id ?w) (n ?n)) => (modify ?c (n (+ ?n 1))))'
rconf plugthrow.conf "node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
plugins = $PLUGINS/test_hooks_a.so
packages = throwpkg"
plugdaemon "cycle_end ok=0 after UDFThrowError" plugthrow.conf \
  "clpdaemon: hooks-a: cycle_end ok=0" "clpdaemon: hooks-a: init node_id=n1 post_fact=-1 EINVAL
clpdaemon: hooks-a: after_load
clpdaemon: n1: running
clpdaemon: hooks-a: cycle_begin
[PRCCODE4] Execution halted during the actions of defrule 'boom'.
clpdaemon: hooks-a: cycle_end ok=0
clpdaemon: received SIGTERM, stopping
clpdaemon: hooks-a: shutdown"
rconf pluglimit.conf "node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
max_rules_per_cycle = 1
plugins = $PLUGINS/test_hooks_a.so
packages = looppkg"
plugdaemon "cycle_end ok=1 for a Run cut by max_rules_per_cycle" pluglimit.conf \
  "clpdaemon: hooks-a: cycle_end ok=1" "clpdaemon: hooks-a: init node_id=n1 post_fact=-1 EINVAL
clpdaemon: hooks-a: after_load
clpdaemon: n1: running
clpdaemon: hooks-a: cycle_begin
clpdaemon: hooks-a: cycle_end ok=1
clpdaemon: received SIGTERM, stopping
clpdaemon: hooks-a: shutdown"
# Without a rule limit the first Run never ends by itself: only SIGTERM
# halts it (the default max_rules_per_cycle would end it first).
rconf plugterm.conf "node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
max_rules_per_cycle = -1
plugins = $PLUGINS/test_hooks_a.so
packages = looppkg"
plugdaemon "cycle_end ok=1 for a Run halted by SIGTERM" plugterm.conf \
  "clpdaemon: hooks-a: cycle_begin" "clpdaemon: hooks-a: init node_id=n1 post_fact=-1 EINVAL
clpdaemon: hooks-a: after_load
clpdaemon: n1: running
clpdaemon: hooks-a: cycle_begin
clpdaemon: hooks-a: cycle_end ok=1
clpdaemon: received SIGTERM, stopping
clpdaemon: hooks-a: shutdown"

# T6c: the stop signals are blocked before the plugins load. A thread a
# plugin starts in init inherits that mask, so SIGTERM still reaches the
# signalfd and the daemon stops gracefully instead of taking the default
# action (exit 143) on that thread.
rconf plugsleeper.conf "node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
plugins = $PLUGINS/test_sleeper.so"
plugdaemon "SIGTERM with a plugin thread stops gracefully" plugsleeper.conf \
  "clpdaemon: sleeper: cycle_end ok=1" "clpdaemon: sleeper: init node_id=n1 post_fact=-1 EINVAL
clpdaemon: sleeper: after_load
clpdaemon: n1: running
clpdaemon: sleeper: cycle_begin
clpdaemon: sleeper: cycle_end ok=1
clpdaemon: received SIGTERM, stopping
clpdaemon: sleeper: shutdown"
# A SIGTERM during plugin init is held until the loop starts, which stops
# before any cycle.
rconf plugslow.conf "node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
plugins = $PLUGINS/test_slow.so"
cases=$((cases + 1))
"$BIN" --config plugslow.conf >out.txt 2>err.txt &
pid=$!
ok=1
wait_line err.txt "clpdaemon: slow: init node_id=n1 post_fact=-1 EINVAL" || ok=0
kill -TERM "$pid"
wait_exit "$pid"
want="clpdaemon: slow: init node_id=n1 post_fact=-1 EINVAL
clpdaemon: slow: after_load
clpdaemon: n1: running
clpdaemon: received SIGTERM, stopping
clpdaemon: slow: shutdown"
if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ -s out.txt ] || [ "$(cat err.txt)" != "$want" ]; then
  fail "SIGTERM during plugin init stops gracefully after startup" "exit $rc, lines seen $ok, stdout [$(cat out.txt)], stderr [$(cat err.txt)]"
fi
# A SIGHUP during plugin init is handled once the loop starts, which keeps
# running; a later SIGINT stops it.
cases=$((cases + 1))
"$BIN" --config plugslow.conf >out.txt 2>err.txt &
pid=$!
ok=1
wait_line err.txt "clpdaemon: slow: init node_id=n1 post_fact=-1 EINVAL" || ok=0
kill -HUP "$pid"
wait_line err.txt "clpdaemon: received SIGHUP, reopening trace files" || ok=0
wait_line err.txt "clpdaemon: slow: cycle_end ok=1" || ok=0
kill -INT "$pid"
wait_exit "$pid"
if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ -s out.txt ] ||
  ! grep -qxF "clpdaemon: received SIGINT, stopping" err.txt ||
  [ "$(tail -n 1 err.txt)" != "clpdaemon: slow: shutdown" ]; then
  fail "SIGHUP during plugin init reopens and keeps running" "exit $rc, lines seen $ok, stdout [$(cat out.txt)], stderr [$(cat err.txt)]"
fi
# A SIGTERM during package loading is held as well.
mkdir slowpkg
conf slowpkg/package.conf 'definition = slowpkg
version = 1
files = s.clp'
conf slowpkg/s.clp '(defglobal ?*z* = (system "sleep 0.6"))'
rconf plugslowpkg.conf "node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
plugins = $PLUGINS/test_hooks_a.so
packages = slowpkg"
cases=$((cases + 1))
"$BIN" --config plugslowpkg.conf >out.txt 2>err.txt &
pid=$!
ok=1
wait_line err.txt "clpdaemon: hooks-a: init node_id=n1 post_fact=-1 EINVAL" || ok=0
kill -TERM "$pid"
wait_exit "$pid"
want="clpdaemon: hooks-a: init node_id=n1 post_fact=-1 EINVAL
clpdaemon: hooks-a: after_load
clpdaemon: n1: running
clpdaemon: received SIGTERM, stopping
clpdaemon: hooks-a: shutdown"
if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ -s out.txt ] || [ "$(cat err.txt)" != "$want" ]; then
  fail "SIGTERM during package loading stops gracefully after startup" "exit $rc, lines seen $ok, stdout [$(cat out.txt)], stderr [$(cat err.txt)]"
fi
# --check never runs the loop and keeps the default SIGTERM action.
cases=$((cases + 1))
"$BIN" --config plugslow.conf --check >out.txt 2>err.txt &
pid=$!
ok=1
wait_line err.txt "clpdaemon: slow: init node_id=n1 post_fact=-1 EINVAL" || ok=0
kill -TERM "$pid"
wait_exit "$pid"
if [ "$ok" != 1 ] || [ "$rc" != 143 ]; then
  fail "--check keeps the default SIGTERM action" "exit $rc, lines seen $ok, stderr [$(cat err.txt)]"
fi

# --- ingest (T7b, docs/ingest.md) ---------------------------------------------
# A plugin posts two facts in init, before the event loop exists: the first
# cycle drains them before cycle_begin; the valid one fires a package rule,
# the other is rejected under the plugin's name.
mkdir ingpkg
conf ingpkg/package.conf 'definition = shop
version = 1
files = s.clp
ingest = order'
conf ingpkg/s.clp '(deftemplate order (slot wf-id) (slot qty) (multislot tags))
(defrule seen (order (wf-id ?w) (qty ?q) (tags $?t)) => (printout t "seen " ?w " " ?q " " (implode$ ?t) crlf))'
rconf ingest.conf "node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
plugins = $PLUGINS/test_poster.so
packages = ingpkg"
cases=$((cases + 1))
"$BIN" --config ingest.conf >out.txt 2>err.txt &
pid=$!
ok=1
wait_line err.txt "clpdaemon: poster: cycle_end ok=1" || ok=0
kill -TERM "$pid"
wait_exit "$pid"
want='clpdaemon: poster: init node_id=n1 post_fact=-1 EINVAL
clpdaemon: poster: posted 0 0
clpdaemon: poster: after_load
clpdaemon: n1: running
clpdaemon: poster: post_fact rejected: template '"'order'"' has no slot '"'bogus'"'
clpdaemon: poster: cycle_begin
clpdaemon: poster: cycle_end ok=1
clpdaemon: received SIGTERM, stopping
clpdaemon: poster: shutdown'
if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ "$(cat out.txt)" != 'seen w1 3 "a" b' ] ||
  [ "$(cat err.txt)" != "$want" ]; then
  fail "facts posted in init are asserted in the first cycle" "exit $rc, lines seen $ok, stdout [$(cat out.txt)], stderr [$(cat err.txt)]"
fi
# An ingest name that is not a deftemplate of the package fails --check.
mkdir ingbad
conf ingbad/package.conf 'definition = bad
version = 1
files = s.clp
ingest = order, nope'
conf ingbad/s.clp '(deftemplate order (slot wf-id))'
conf ingbad.conf 'node_id = n1
packages = ingbad'
RUN='timeout 10' check "ingest names an unknown template" 1 "" \
  "clpdaemon: bad@1: ingest names unknown template 'nope'" -- --config ingbad.conf --check
conf ingdup.conf 'node_id = n1
packages = ingpkg, ingbad'
RUN='timeout 10' check "ingest template listed by two packages" 1 "" \
  "clpdaemon: bad@1: ingest names template 'order', already listed by shop@1
clpdaemon: bad@1: ingest names unknown template 'nope'" -- --config ingdup.conf --check

# An implied (ordered-pattern) template is not a deftemplate of the package.
mkdir ingimp
conf ingimp/package.conf 'definition = imp
version = 1
files = s.clp
ingest = imp'
conf ingimp/s.clp '(defrule r (imp ?w) =>)'
conf ingimp.conf 'node_id = n1
packages = ingimp'
RUN='timeout 10' check "ingest names an implied template" 1 "" \
  "clpdaemon: imp@1: lint: template 'imp' has no wf-id slot
clpdaemon: imp@1: lint: rule 'r': pattern 1 (imp) does not bind wf-id to a variable
clpdaemon: imp@1: ingest names unknown template 'imp'" -- --config ingimp.conf --check
# A package that failed to load still claims the names of its ingest list.
mkdir ingfail
conf ingfail/package.conf 'definition = fail
version = 1
files = missing.clp
ingest = order'
conf ingfaildup.conf 'node_id = n1
packages = ingfail, ingpkg'
RUN='timeout 10' check "ingest template listed by a package that failed" 1 "" \
  "clpdaemon: ingfail/missing.clp: cannot open: No such file or directory
clpdaemon: shop@1: ingest names template 'order', already listed by fail@1" -- --config ingfaildup.conf --check
# Facts still queued at teardown are dropped with one line on stderr: under
# --check the facts a plugin posts in init are never drained.
conf ingcheck.conf "node_id = n1
plugins = $PLUGINS/test_poster.so
packages = ingpkg"
RUN='timeout 10' check "queued facts dropped at shutdown under --check" 0 \
  "clpdaemon: ingcheck.conf: configuration OK" "clpdaemon: poster: init node_id=n1 post_fact=-1 EINVAL
clpdaemon: poster: posted 0 0
clpdaemon: poster: shutdown
clpdaemon: 2 queued facts dropped at shutdown" -- --config ingcheck.conf --check
conf ingone.conf "node_id = n1
plugins = $PLUGINS/test_thread.so"
RUN='timeout 10' check "one queued fact dropped at shutdown" 0 \
  "clpdaemon: ingone.conf: configuration OK" "clpdaemon: thread: init node_id=n1 post_fact=-1 EINVAL
clpdaemon: thread: thread post_fact=0
clpdaemon: thread: shutdown
clpdaemon: 1 queued fact dropped at shutdown" -- --config ingone.conf --check
conf ingonefail.conf "node_id = n1
plugins = $PLUGINS/test_thread.so
packages = nopkg"
RUN='timeout 10' check "queued fact dropped after a startup failure" 1 "" \
  "clpdaemon: thread: init node_id=n1 post_fact=-1 EINVAL
clpdaemon: thread: thread post_fact=0
clpdaemon: nopkg/package.conf: cannot open: No such file or directory
clpdaemon: thread: shutdown
clpdaemon: 1 queued fact dropped at shutdown" -- --config ingonefail.conf
# A post wakes the loop: with a 60 s tick, a fact a plugin thread posts
# 200 ms after init fires its rule within 2 s, long before the next tick.
mkdir ingwake
conf ingwake/package.conf 'definition = wake
version = 1
files = s.clp
ingest = order'
conf ingwake/s.clp '(deftemplate order (slot wf-id))
(defrule seen (order (wf-id ?w)) => (printout t "seen " ?w crlf))'
rconf ingwake.conf "node_id = n1
tick_interval_ms = 60000
plugins = $PLUGINS/test_late.so
packages = ingwake"
cases=$((cases + 1))
"$BIN" --config ingwake.conf >out.txt 2>err.txt &
pid=$!
ok=1
wait_line err.txt "clpdaemon: n1: running" || ok=0
n=0
while ! grep -qxF "seen late" out.txt 2>/dev/null; do
  n=$((n + 1))
  if [ "$n" -gt 40 ]; then ok=0; break; fi
  sleep 0.05
done
kill -TERM "$pid"
wait_exit "$pid"
want='clpdaemon: late: init node_id=n1 post_fact=-1 EINVAL
clpdaemon: late: after_load
clpdaemon: n1: running
clpdaemon: late: cycle_begin
clpdaemon: late: cycle_end ok=1
clpdaemon: received SIGTERM, stopping
clpdaemon: late: shutdown late post_fact=0'
if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ "$(cat out.txt)" != "seen late" ] ||
  [ "$(cat err.txt)" != "$want" ]; then
  fail "a post wakes the loop" "exit $rc, lines seen $ok, stdout [$(cat out.txt)], stderr [$(cat err.txt)]"
fi

# --- admin socket (FR-20, T7c, docs/admin-socket.md) -------------------------
conf modebad.conf 'node_id = n1
admin_socket_mode = 0666'
check "admin_socket_mode with bits for others" 1 "" \
  "clpdaemon: modebad.conf:2: admin_socket_mode: expected an octal mode such as 0600 or 0660, with no permission bits for others, got '0666'" -- --config modebad.conf --check
# --check never creates the socket.
conf admchk.conf 'node_id = n1
admin_socket = chk.sock'
check "check with an admin socket" 0 "clpdaemon: admchk.conf: configuration OK" "" -- --config admchk.conf --check
cases=$((cases + 1))
[ ! -e chk.sock ] || fail "check creates no admin socket" "$(ls -l chk.sock)"
# Startup errors exit 1 after the plugins' shutdown and leave the path as
# it was.
printf 'keep\n' >reg.sock
conf admreg.conf "node_id = n1
admin_socket = reg.sock
plugins = $PLUGINS/test_hooks_a.so"
RUN='timeout 10' check "admin socket path is a regular file" 1 "" \
  "clpdaemon: hooks-a: init node_id=n1 post_fact=-1 EINVAL
clpdaemon: reg.sock: not a socket
clpdaemon: hooks-a: shutdown" -- --config admreg.conf
cases=$((cases + 1))
[ "$(cat reg.sock)" = keep ] || fail "regular file at the admin socket path is kept" "$(ls -l reg.sock)"
conf admnodir.conf 'node_id = n1
admin_socket = nodir/a.sock'
RUN='timeout 10' check "admin socket directory missing" 1 "" \
  "clpdaemon: nodir/a.sock: cannot bind the admin socket: No such file or directory" -- --config admnodir.conf
cases=$((cases + 1))
[ ! -e nodir ] || fail "missing admin socket directory is not created" "$(ls -ld nodir)"
p108=$(printf 's%.0s' $(seq 1 108))
conf admlong.conf "node_id = n1
admin_socket = $p108"
RUN='timeout 10' check "admin socket path longer than 107 bytes" 1 "" \
  "clpdaemon: $p108: admin socket path too long (max 107 bytes)" -- --config admlong.conf
p107=${p108#s}
conf adm107.conf "node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
admin_socket = $p107"
daemon "admin socket path of 107 bytes" adm107.conf TERM "clpdaemon: n1: running
clpdaemon: received SIGTERM, stopping"
cases=$((cases + 1))
[ ! -e "$p107" ] || fail "107-byte admin socket removed at stop" "$(ls -l "$p107")"
# admin_socket = none disables the socket (L71): the daemon runs without it
# and leaves a file named none alone.
printf 'keep\n' >none
conf admnone.conf 'node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
admin_socket = none'
daemon "admin socket disabled with none" admnone.conf TERM "clpdaemon: n1: running
clpdaemon: received SIGTERM, stopping"
cases=$((cases + 1))
[ "$(cat none)" = keep ] || fail "admin_socket = none touches no file" "$(ls -l none)"
check "check with the admin socket disabled" 0 "clpdaemon: admnone.conf: configuration OK" "" -- --config admnone.conf --check

# acase <name> <expected-output> <admin_client args...>: one client run;
# the output must match exactly (with its final newline).
acase() {
  aname=$1 awant=$2
  shift 2
  cases=$((cases + 1))
  "$ADMIN" "$@" >aout.txt 2>aerr.txt
  arc=$?
  aout=$(cat aout.txt; printf x)
  aout=${aout%x}
  if [ "$arc" != 0 ] || [ "$aout" != "${awant:+$awant$NL}" ] || [ -s aerr.txt ]; then
    failures=$((failures + 1))
    printf 'FAIL: %s\n  exit: %s\n  reply: got [%s]\n         want [%s]\n  stderr: [%s]\n' \
      "$aname" "$arc" "$aout" "$awant" "$(cat aerr.txt)"
  fi
}
# wait_reply <socket> <command> <reply>: waits up to 5 s for that reply.
wait_reply() {
  n=0
  while [ "$("$ADMIN" "$1" "$2\n" 2>/dev/null)" != "$3" ]; do
    n=$((n + 1))
    [ "$n" -gt 100 ] && return 1
    sleep 0.05
  done
}
# start <conf> <socket>: starts the daemon in the background (pid) and waits
# for its running line and its socket; ok=0 when either is missing.
start() {
  : >out.txt # truncated before the start, as in daemon()
  : >err.txt
  "$BIN" --config "$1" >out.txt 2>err.txt &
  pid=$!
  ok=1
  wait_line err.txt "clpdaemon: n1: running" || ok=0
  [ -S "$2" ] || ok=0
}

# The commands, on a daemon whose next tick is a minute away: an inject
# wakes the loop at once and its rule fires (the deterministic wake case).
conf admcmd.conf 'node_id = n1
tick_interval_ms = 60000
packages = ingwake
admin_socket = adm.sock'
cases=$((cases + 1))
start admcmd.conf adm.sock
[ "$(stat -c %a adm.sock 2>/dev/null)" = 600 ] || fail "admin socket default mode 0600" "$(ls -l adm.sock)"
wait_reply adm.sock status "OK node_id=n1 cycles=1 queue=0 facts=0" || ok=0
S0="OK node_id=n1 cycles=1 queue=0 facts=0"
acase "admin: errors and several commands in one write" "ERR invalid JSON: offset 1: unexpected end of input
ERR template 'nope' is not ingestible
ERR template 'order' has no slot 'tags'
ERR unknown command 'bogus'
ERR unknown command 'bo?gus'
ERR empty command
ERR 'status' takes no arguments
ERR 'reopen' takes no arguments
ERR unknown command 'Status'
ERR NUL byte not allowed
$S0
$S0" adm.sock 'inject {\ninject {"template":"nope","slots":{}}\ninject {"template":"order","slots":{"tags":1}}\nbogus arg\nbo\tgus\n\nstatus x\nreopen now\nStatus\nsta\0tus\nstatus\r\nstatus\n'
acase "admin: inject" "OK queued" adm.sock 'inject {"template":"order","slots":{"wf-id":"w1"}}\n'
n=0
while ! grep -qxF "seen w1" out.txt 2>/dev/null; do
  n=$((n + 1))
  if [ "$n" -gt 40 ]; then ok=0; break; fi
  sleep 0.05
done
S1="OK node_id=n1 cycles=2 queue=0 facts=1"
acase "admin: status after the inject" "$S1" adm.sock 'status\n'
acase "admin: command split across writes" "$S1
OK queued" adm.sock 'sta' 'tus\ninj' 'ect {"template":"order","slots":{"wf-id":"w2"}}\n'
wait_reply adm.sock status "OK node_id=n1 cycles=3 queue=0 facts=2" || ok=0
S2="OK node_id=n1 cycles=3 queue=0 facts=2"
# A client that disconnects mid-line: the partial command never runs.
acase "admin: disconnect mid-line" "" -c adm.sock 'inject {"template":"order","slots":{"wf-id":"w9"}}'
sleep 0.4
acase "admin: nothing queued by a partial line" "$S2" adm.sock 'status\n'
x64k=$(head -c 65536 /dev/zero | tr '\0' x)
x64=$(printf 'x%.0s' $(seq 1 64))
acase "admin: 65536-byte line" "ERR unknown command '$x64...'
$S2" adm.sock "$x64k\\nstatus\\n"
acase "admin: line too long closes the connection" "ERR line too long" adm.sock "${x64k}x\\nstatus\\n"
acase "admin: too many clients" "ERR too many clients" -h 16 adm.sock 'status\n'
wait_reply adm.sock status "$S2" || fail "admin: served again after too many clients" "[$("$ADMIN" adm.sock 'status\n' 2>&1)]"
acase "admin: a client that does not read is disconnected" "disconnected" -f adm.sock
acase "admin: still serving" "$S2" adm.sock 'status\n'
kill -TERM "$pid"
wait_exit "$pid"
want="clpdaemon: n1: running
clpdaemon: received SIGTERM, stopping"
if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ "$(cat out.txt)" != "seen w1${NL}seen w2" ] ||
  [ "$(cat err.txt)" != "$want" ] || [ -e adm.sock ]; then
  fail "admin socket commands" "exit $rc, lines seen $ok, stdout [$(cat out.txt)], stderr [$(cat err.txt)], socket [$(ls -l adm.sock 2>&1)]"
fi

# A full queue: no cycle runs for a minute, so the 1025th inject is refused
# and the 1024 queued facts are dropped at stop.
conf admfull.conf 'node_id = n1
tick_interval_ms = 60000
cycle_min_interval_ms = 60000
packages = ingwake
admin_socket = full.sock
admin_socket_mode = 0660'
cases=$((cases + 1))
start admfull.conf full.sock
[ "$(stat -c %a full.sock 2>/dev/null)" = 660 ] || fail "admin socket mode 0660" "$(ls -l full.sock)"
wait_reply full.sock status "OK node_id=n1 cycles=1 queue=0 facts=0" || ok=0
many='' wantmany='' i=0
while [ "$i" -lt 1024 ]; do
  many=$many'inject {"template":"order","slots":{"wf-id":"q"}}\n'
  wantmany="${wantmany}OK queued$NL"
  i=$((i + 1))
done
acase "admin: inject into a full queue" "${wantmany}ERR ingest queue full
OK node_id=n1 cycles=1 queue=1024 facts=0" full.sock "${many}"'inject {"template":"order","slots":{"wf-id":"q"}}\nstatus\n'
kill -TERM "$pid"
wait_exit "$pid"
want="clpdaemon: n1: running
clpdaemon: received SIGTERM, stopping
clpdaemon: 1024 queued facts dropped at shutdown"
if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ -s out.txt ] || [ "$(cat err.txt)" != "$want" ] || [ -e full.sock ]; then
  fail "admin queue full" "exit $rc, lines seen $ok, stdout [$(cat out.txt)], stderr [$(cat err.txt)]"
fi

# reopen: like SIGHUP, without its log line; a trace directory moved away
# cannot be reopened.
mkdir rdir
conf admhup.conf 'node_id = n1
tick_interval_ms = 60000
trace_text = rdir/t.log
admin_socket = hup.sock'
cases=$((cases + 1))
start admhup.conf hup.sock
acase "admin: reopen" "OK reopened" hup.sock 'reopen\n'
mv rdir rdir.1
acase "admin: reopen failure" "ERR 1 trace files could not be reopened" hup.sock 'reopen\n'
kill -TERM "$pid"
wait_exit "$pid"
want="clpdaemon: n1: running
clpdaemon: rdir/t.log: cannot reopen: No such file or directory
clpdaemon: received SIGTERM, stopping"
if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ -s out.txt ] || [ "$(cat err.txt)" != "$want" ] || [ -e hup.sock ]; then
  fail "admin reopen" "exit $rc, lines seen $ok, stdout [$(cat out.txt)], stderr [$(cat err.txt)]"
fi

# A stale socket (its daemon was killed) is replaced; a daemon whose socket
# another daemon replaced leaves that one in place at stop.
conf admstale.conf 'node_id = n1
tick_interval_ms = 60000
admin_socket = stale.sock'
cases=$((cases + 1))
start admstale.conf stale.sock
kill -KILL "$pid"
wait "$pid" 2>/dev/null
[ -S stale.sock ] || ok=0
start admstale.conf stale.sock
first=$pid
wait_reply stale.sock status "OK node_id=n1 cycles=1 queue=0 facts=0" || ok=0
"$BIN" --config admstale.conf >out2.txt 2>err2.txt &
second=$!
wait_line err2.txt "clpdaemon: n1: running" || ok=0
kill -TERM "$first"
wait_exit "$first"
[ "$rc" = 0 ] || ok=0
[ -S stale.sock ] || ok=0
wait_reply stale.sock status "OK node_id=n1 cycles=1 queue=0 facts=0" || ok=0
kill -TERM "$second"
wait_exit "$second"
if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ -e stale.sock ]; then
  fail "admin stale and replaced sockets" "exit $rc, ok $ok, socket [$(ls -l stale.sock 2>&1)], stderr [$(cat err.txt err2.txt)]"
fi

# --check leaves a stale socket in place (same inode).
conf admchk2.conf 'node_id = n1
admin_socket = stale.sock'
start admstale.conf stale.sock
kill -KILL "$pid"
wait "$pid" 2>/dev/null
i1=$(stat -c %i stale.sock 2>&1)
check "check keeps a stale admin socket" 0 "clpdaemon: admchk2.conf: configuration OK" "" -- --config admchk2.conf --check
cases=$((cases + 1))
[ -S stale.sock ] && [ "$(stat -c %i stale.sock)" = "$i1" ] ||
  fail "check leaves the stale admin socket alone" "before $i1, now [$(ls -li stale.sock 2>&1)]"
rm -f stale.sock

# A directory, a FIFO or a symlink at the path is not a socket and is left
# untouched.
for k in dir fifo link; do
  case $k in
  dir) mkdir k$k.sock ;;
  fifo) mkfifo k$k.sock ;;
  link) ln -s nowhere k$k.sock ;;
  esac
  conf adm$k.conf "node_id = n1
admin_socket = k$k.sock"
  RUN='timeout 10' check "admin socket path is a $k" 1 "" \
    "clpdaemon: k$k.sock: not a socket" -- --config adm$k.conf
  cases=$((cases + 1))
  case $k in
  dir) [ -d k$k.sock ] ;;
  fifo) [ -p k$k.sock ] ;;
  link) [ -L k$k.sock ] && [ "$(readlink k$k.sock)" = nowhere ] ;;
  esac || fail "$k at the admin socket path is kept" "$(ls -ld k$k.sock 2>&1)"
done

# A startup failure after the socket was created removes it: with 5 file
# descriptors (inherited ones above 2 closed) the libraries load and the
# socket is created, but the event loop cannot open its descriptors.
conf admfd.conf 'node_id = n1
admin_socket = fd.sock'
printf '%s\n' 'exec timeout 10 sh -c '\''exec 3>&- 4>&- 5>&- 6>&- 7>&- 8>&- 9>&-; ulimit -n 5; exec "$0" "$@"'\'' "$@"' >lim5.sh
RUN='sh lim5.sh' check "admin socket removed after a later startup failure" 1 "" \
  "clpdaemon: cannot start the event loop: Too many open files" -- --config admfd.conf
cases=$((cases + 1))
[ ! -e fd.sock ] || fail "admin socket left after a later startup failure" "$(ls -l fd.sock)"

# Out of file descriptors at accept: the listener is not polled until a
# client closes (or the next cycle), so the engine thread does not spin.
# With 16 descriptors, 9 held clients use the last free ones and a 10th
# connection waits in the backlog.
conf admemf.conf 'node_id = n1
tick_interval_ms = 60000
admin_socket = emf.sock'
cases=$((cases + 1))
sh -c 'ulimit -n 16; exec "$0" --config "$1"' "$BIN" admemf.conf >out.txt 2>err.txt &
pid=$!
ok=1
wait_line err.txt "clpdaemon: n1: running" || ok=0
"$ADMIN" -h 9 emf.sock 'status\n' >aout.txt 2>aerr.txt &
client=$!
sleep 0.5
t0=$(awk '{print $14 + $15}' /proc/$pid/stat)
sleep 1
t1=$(awk '{print $14 + $15}' /proc/$pid/stat)
kill -TERM "$pid"
wait_exit "$pid"
wait "$client" 2>/dev/null
ticks=$((t1 - t0))
if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ "$ticks" -gt 20 ] || [ -e emf.sock ] ||
  [ "$(cat err.txt)" != "clpdaemon: n1: running
clpdaemon: received SIGTERM, stopping" ]; then
  fail "admin socket out of file descriptors" "exit $rc, lines seen $ok, CPU ticks in 1 s $ticks, stderr [$(cat err.txt)]"
fi

# --- database (T8d, docs/database.md) ----------------------------------------
# The database opens after the configuration loads and before the plugins'
# init, also under --check; a configuration without `database` is covered by
# every case above.
conf dbok.conf "node_id = n1
database = sqlite:$WORK/dbok.db
plugins = $PLUGINS/clpd_example.so"
check "database: sqlite opens under --check" 0 "clpdaemon: dbok.conf: configuration OK" \
  "clpdaemon: example: init
clpdaemon: example: shutdown" -- --config dbok.conf --check
cases=$((cases + 1))
[ -f dbok.db ] || fail "database: --check creates the sqlite file" "$(ls -l)"

# A missing directory: exit 1 before any plugin init, and the message never
# shows the spec (the path).
conf dbmiss.conf "node_id = n1
database = sqlite:$WORK/nodir-secret/x.db
plugins = $PLUGINS/clpd_example.so"
check "database: cannot open" 1 "" \
  "clpdaemon: cannot open the database: cannot open the SQLite database: unable to open database file" \
  -- --config dbmiss.conf --check
cases=$((cases + 1))
if grep -q 'nodir-secret' err.txt || [ -e nodir-secret ]; then
  fail "database: the open error does not show the path" "stderr [$(cat err.txt)]"
fi

# PostgreSQL that cannot be reached (no server on that socket directory):
# exit 1 with the one-line message; no cluster is started here.
mkdir pgnone
conf dbpg.conf "node_id = n1
database = host=$WORK/pgnone port=1 dbname=clpd user=clpd connect_timeout=2"
cases=$((cases + 1))
timeout 10 "$BIN" --config dbpg.conf --check >out.txt 2>err.txt
rc=$?
if [ "$rc" != 1 ] || [ -s out.txt ] || [ "$(wc -l <err.txt | tr -d ' ')" != 1 ] ||
  ! grep -q '^clpdaemon: cannot open the database: cannot connect to PostgreSQL' err.txt; then
  fail "database: unreachable PostgreSQL" "exit $rc, stdout [$(cat out.txt)], stderr [$(cat err.txt)]"
fi

# Rejected specs (T8a): file:line, the exact message, never the value.
dberr() {
  conf "$1.conf" "node_id = n1
database = $2"
  check "database: rejected spec $1" 1 "" "clpdaemon: $1.conf:2: database: $3" -- --config "$1.conf" --check
}
PWMSG='the connection string must not contain a password (use a password file, see PGPASSFILE)'
dberr dbempty '' 'expected a connection string'
dberr dbnopath 'sqlite:' 'expected sqlite:<path>'
dberr dbbad 'not a s3cr3t conninfo' 'invalid PostgreSQL connection string'
dberr dbpw 'host=/tmp password=s3cr3t' "$PWMSG"
dberr dbsslpw 'host=/tmp sslpassword=s3cr3t' "$PWMSG"
dberr dburipw 'postgresql://clpd:s3cr3t@localhost/clpd' "$PWMSG"
# A password inside a dbname that is itself a connection string (L78).
for v in "dbnamepw host=/tmp dbname='postgresql://u:s3cr3t@h/d'" \
    "dbnamekvpw host=/tmp dbname='host=h password=s3cr3t'"; do
  dberr "${v%% *}" "${v#* }" "$PWMSG"
  cases=$((cases + 1))
  if grep -q s3cr3t err.txt; then
    fail "database: a nested password is not echoed" "stderr [$(cat err.txt)]"
  fi
done

# A running daemon with a sqlite database starts and stops gracefully.
rconf dbrun.conf "node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
database = sqlite:$WORK/dbrun.db"
daemon "database: running with a sqlite database" dbrun.conf TERM "clpdaemon: n1: running
clpdaemon: received SIGTERM, stopping"
cases=$((cases + 1))
[ -f dbrun.db ] || fail "database: the daemon creates the sqlite file" "$(ls -l)"

# --- ORM mapping (T9a, docs/orm.md) -------------------------------------------
# The mapping is read and checked at package load, also under --check; it
# needs a database.
mkdir ormpkg
conf ormpkg/package.conf 'definition = shop
version = 1
files = shop.clp
orm = orm.conf'
conf ormpkg/shop.clp '(deftemplate order (slot wf-id) (slot line-no (type INTEGER)) (slot status (type SYMBOL)))
(defrule start ?o <- (order (wf-id ?w) (status new)) => (modify ?o (status started)))'
conf ormpkg/orm.conf '# shop orders
order.table = shop_order
order.owned = yes
order.key = line-no
order.slot.wf-id = wf_id text string error
order.slot.line-no = line_no bigint integer error
order.slot.status = status text symbol default
seq.order_no = shop_order_no
seq.order_no.owned = yes'
conf orm.conf "node_id = n1
database = sqlite:$WORK/orm.db
orm_allow_drop = yes
packages = ormpkg"
check "orm: valid mapping under --check" 0 "clpdaemon: orm.conf: configuration OK" "" -- --config orm.conf --check
conf ormnodb.conf 'node_id = n1
packages = ormpkg'
check "orm: mapping without a database" 1 "" \
  "clpdaemon: shop@1: orm: a mapping needs a database (the configuration sets no database)" -- --config ormnodb.conf --check
conf ormdrop.conf 'node_id = n1
orm_allow_drop = true'
check "orm: bad orm_allow_drop" 1 "" \
  "clpdaemon: ormdrop.conf:2: orm_allow_drop: expected yes or no, got 'true'" -- --config ormdrop.conf --check
# A package without orm loads unchanged with a database.
conf ormplain.conf "node_id = n1
database = sqlite:$WORK/orm.db
orm_allow_drop = no
packages = pkg"
check "orm: package without a mapping" 0 "clpdaemon: ormplain.conf: configuration OK" "" -- --config ormplain.conf --check
mkdir ormbad
conf ormbad/package.conf 'definition = bad
version = 1
files = bad.clp
orm = orm.conf'
conf ormbad/bad.clp '(deftemplate order (slot wf-id) (slot n (type INTEGER)) (slot note) (multislot tags))'
conf ormbad/orm.conf 'order.table = shop_order
order.owned = no
order.key = n
order.slot.wf-id = wf_id text string error
order.slot.n = n bigint string error
order.slot.extra = extra text string nil
seq.order_no = shop_order_no
seq.order_no.owned = maybe'
conf ormbad.conf "node_id = n1
database = sqlite:$WORK/orm.db
packages = ormbad"
check "orm: mapping file error" 1 "" \
  "clpdaemon: ormbad/orm.conf:8: seq.order_no.owned: expected yes or no, got 'maybe'" -- --config ormbad.conf --check
conf ormbad/orm.conf 'order.table = shop_order
order.owned = no
order.key = n
order.slot.wf-id = wf_id text string error
order.slot.n = n text string error
order.slot.extra = extra text string nil
seq.order_no = shop_order_no
seq.order_no.owned = no'
conf ormtwo.conf "node_id = n1
database = sqlite:$WORK/orm.db
packages = ormpkg, ormbad"
check "orm: checks against the templates and another definition" 1 "" \
  "clpdaemon: bad@1: orm: template 'order': slot 'n' is mapped as string, which its type constraint does not allow
clpdaemon: bad@1: orm: template 'order' has no slot 'extra'
clpdaemon: bad@1: orm: template 'order': slot 'note' is not mapped
clpdaemon: bad@1: orm: template 'order': slot 'tags' is a multislot, which cannot be mapped
clpdaemon: bad@1: orm: table 'shop_order' is already mapped by shop@1
clpdaemon: bad@1: orm: sequence 'shop_order_no' is already mapped by shop@1" -- --config ormtwo.conf --check
conf ormbad/package.conf 'definition = bad
version = 1
files = bad.clp
orm = /etc/clpdaemon/orm.conf'
check "orm: absolute mapping path" 1 "" \
  "clpdaemon: ormbad/package.conf:4: orm: '/etc/clpdaemon/orm.conf' must be relative to the package directory" -- --config ormbad.conf --check
# The daemon's counter table is a reserved table name (T9e, L87).
mkdir ormres
conf ormres/package.conf 'definition = res
version = 1
files = res.clp
orm = orm.conf'
conf ormres/res.clp '(deftemplate order (slot wf-id))'
conf ormres/orm.conf 'order.table = clpd_sequence
order.owned = yes
order.slot.wf-id = wf_id text string error'
conf ormres.conf "node_id = n1
database = sqlite:$WORK/orm.db
packages = ormres"
check "orm: reserved table name" 1 "" \
  "clpdaemon: ormres/orm.conf:1: order.table: table name 'clpd_sequence' is reserved (the daemon's sequence counter table)" -- --config ormres.conf --check

# --- ORM DDL functions (T9b, docs/orm.md#ddl-functions) -----------------------
# Without a database the functions do not exist, so a package calling one
# does not load.
mkdir ddlnodb
conf ddlnodb/package.conf 'definition = nodb
version = 1
files = r.clp'
conf ddlnodb/r.clp '(deftemplate go (slot wf-id))
(defrule r (go (wf-id ?w)) => (db-create-table go))'
conf ddlnodb.conf 'node_id = n1
packages = ddlnodb'
check "orm ddl: no functions without a database" 1 "" \
  "clpdaemon: nodb@1: clips: [EXPRNPSR3] ddlnodb/r.clp, Line 2: Missing function declaration for 'db-create-table'.
clpdaemon: nodb@1: clips: ERROR:
clpdaemon: nodb@1: clips: (defrule nodb@1::r
clpdaemon: nodb@1: clips:    (go (wf-id ?w))
clpdaemon: nodb@1: clips:    =>
clpdaemon: nodb@1: clips:    (db-create-table
clpdaemon: ddlnodb/r.clp: load failed" -- --config ddlnodb.conf --check

# A rule calls the functions on a sqlite database: the owned table is
# created as the mapping says, the existing-table check reports a missing
# table, and db-drop-table is refused while orm_allow_drop = no (one log
# line, the rule halts), then drops the table once it is yes.
mkdir ormddl
conf ormddl/package.conf 'definition = ddl
version = 1
files = ddl.clp
orm = orm.conf'
conf ormddl/ddl.clp '(deftemplate order (slot wf-id) (slot line-no (type INTEGER)) (slot note))
(deftemplate party (slot wf-id) (slot name))
(deftemplate go (slot wf-id))
(defrule ddl (go (wf-id ?w)) =>
  (printout t "create " (db-create-table order) crlf)
  (printout t "check " (length$ (db-check-schema order)) crlf)
  (printout t "party " (implode$ (db-check-schema party)) crlf)
  (bind ?d (db-drop-table order))
  (printout t "drop " ?d crlf))
(deffacts seed (go (wf-id w1)))'
conf ormddl/orm.conf 'order.table = ddl_order
order.owned = yes
order.key = line-no
order.slot.wf-id = wf_id text string error
order.slot.line-no = line_no bigint integer error
order.slot.note = note text string nil
party.table = crm_party
party.owned = no
party.slot.wf-id = wf_id text string error
party.slot.name = name text string default'
# ddlrun <name> <conf> <stdout> <stderr>: runs the daemon until the last
# stderr line before the stop, then stops it with SIGTERM.
ddlrun() {
  cases=$((cases + 1))
  : >out.txt
  : >err.txt
  "$BIN" --config "$2" >out.txt 2>err.txt &
  pid=$!
  ok=1
  wait_line err.txt "${4##*$NL}" || ok=0
  kill -TERM "$pid" 2>/dev/null
  wait_exit "$pid"
  out=$(cat out.txt; printf x) err=$(cat err.txt; printf x)
  out=${out%x} err=${err%x}
  if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ "$out" != "$3$NL" ] ||
    [ "$err" != "$4${NL}clpdaemon: received SIGTERM, stopping$NL" ]; then
    fail "$1" "exit $rc (lines seen $ok), stdout [$out], stderr [$err]"
  fi
}
rconf ddl.conf "node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
database = sqlite:$WORK/ddl.db
packages = ormddl"
ddlrun "orm ddl: create, check, drop refused" ddl.conf 'create TRUE
check 0
party "table '\''crm_party'\'' does not exist"' "clpdaemon: n1: running
clpdaemon: db-drop-table: dropping tables is disabled (orm_allow_drop = no)
[PRCCODE4] Execution halted during the actions of defrule 'ddl'."
if command -v sqlite3 >/dev/null 2>&1; then
  cases=$((cases + 1))
  got=$(sqlite3 ddl.db "SELECT type || ' ' || name || ': ' || coalesce(sql, '') FROM sqlite_master ORDER BY name" 2>&1)
  # workflow_instance: created at startup, a package has a mapping (T10a).
  want='table ddl_order: CREATE TABLE "ddl_order" ("wf_id" TEXT NOT NULL, "line_no" INTEGER NOT NULL, "note" TEXT, PRIMARY KEY ("wf_id", "line_no"))
index sqlite_autoindex_ddl_order_1: 
index sqlite_autoindex_workflow_instance_1: 
table workflow_instance: CREATE TABLE "workflow_instance" ("wf_id" TEXT NOT NULL, "definition" TEXT NOT NULL, "version" TEXT NOT NULL, "status" TEXT NOT NULL, "parent_wf_id" TEXT, "owner_node" TEXT, "lease_until" TEXT, "created_at" TEXT NOT NULL, "updated_at" TEXT NOT NULL, PRIMARY KEY ("wf_id"))'
  [ "$got" = "$want" ] || fail "orm ddl: the generated table" "got [$got], want [$want]"
  # A second run keeps the existing table and its row (IF NOT EXISTS), and
  # the refused drop leaves both.
  sqlite3 ddl.db "INSERT INTO ddl_order VALUES ('w1', 1, 'kept')"
  ddlrun "orm ddl: existing owned table untouched" ddl.conf 'create TRUE
check 0
party "table '\''crm_party'\'' does not exist"' "clpdaemon: n1: running
clpdaemon: db-drop-table: dropping tables is disabled (orm_allow_drop = no)
[PRCCODE4] Execution halted during the actions of defrule 'ddl'."
  cases=$((cases + 1))
  got=$(sqlite3 ddl.db "SELECT wf_id || ' ' || line_no || ' ' || note FROM ddl_order" 2>&1)
  [ "$got" = "w1 1 kept" ] || fail "orm ddl: the row is kept" "got [$got]"
  # With orm_allow_drop = yes the same rule drops the table.
  rconf ddldrop.conf "node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
database = sqlite:$WORK/ddl.db
orm_allow_drop = yes
packages = ormddl"
  cases=$((cases + 1))
  : >out.txt
  : >err.txt
  "$BIN" --config ddldrop.conf >out.txt 2>err.txt &
  pid=$!
  ok=1
  wait_line out.txt "drop TRUE" || ok=0
  kill -TERM "$pid" 2>/dev/null
  wait_exit "$pid"
  left=$(sqlite3 ddl.db "SELECT name FROM sqlite_master WHERE name = 'ddl_order'" 2>&1)
  if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ -n "$left" ] ||
    [ "$(cat out.txt)" != "create TRUE
check 0
party \"table 'crm_party' does not exist\"
drop TRUE" ] ||
    [ "$(cat err.txt)" != "clpdaemon: n1: running
clpdaemon: received SIGTERM, stopping" ]; then
    fail "orm ddl: drop allowed" "exit $rc (lines seen $ok), table [$left], stdout [$(cat out.txt)], stderr [$(cat err.txt)]"
  fi
  # A rule of another definition cannot drop (or create) the table, even
  # with orm_allow_drop = yes (T9d, L84): one log line, the rule halts,
  # the table and its row are kept.
  mkdir ormown ormforeign
  conf ormown/package.conf 'definition = own
version = 1
files = own.clp
orm = orm.conf'
  conf ormown/own.clp '(deftemplate item (slot wf-id) (slot note))'
  conf ormown/orm.conf 'item.table = own_item
item.owned = yes
item.slot.wf-id = wf_id text string error
item.slot.note = note text string nil'
  conf ormforeign/package.conf 'definition = foreign
version = 1
files = foreign.clp'
  conf ormforeign/foreign.clp '(deftemplate go (slot wf-id))
(defrule r (go (wf-id ?w)) =>
  (printout t "foreign" crlf)
  (db-drop-table own@1::item)
  (printout t "dropped" crlf))
(deffacts seed (go (wf-id w1)))'
  sqlite3 foreign.db "CREATE TABLE own_item (wf_id TEXT NOT NULL PRIMARY KEY, note TEXT); INSERT INTO own_item VALUES ('w1', 'kept')"
  rconf foreign.conf "node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
database = sqlite:$WORK/foreign.db
orm_allow_drop = yes
packages = ormown, ormforeign"
  ddlrun "orm ddl: another definition's table is refused" foreign.conf 'foreign' "clpdaemon: n1: running
clpdaemon: db-drop-table: template 'own@1::item' is not mapped by definition foreign of the calling rule (mapped by own@1)
[PRCCODE4] Execution halted during the actions of defrule 'r'."
  cases=$((cases + 1))
  got=$(sqlite3 foreign.db "SELECT wf_id || ' ' || note FROM own_item" 2>&1)
  [ "$got" = "w1 kept" ] || fail "orm ddl: the other definition's row is kept" "got [$got]"
  # Sequences (T9c): a rule creates the owned sequence (the SQLite counter
  # row) and draws values; an unknown alias is one log line and halts the
  # rule. A second run continues from the stored value.
  mkdir ormseq
  conf ormseq/package.conf 'definition = seq
version = 1
files = seq.clp
orm = orm.conf'
  conf ormseq/seq.clp '(deftemplate go (slot wf-id))
(defrule r (go (wf-id ?w)) =>
  (printout t "create " (db-create-sequence order_no) crlf)
  (bind ?a (db-nextval order_no))
  (printout t "next " ?a " " (db-nextval order_no) " " (integerp ?a) crlf)
  (db-nextval nope)
  (printout t "not reached" crlf))
(deffacts seed (go (wf-id w1)))'
  conf ormseq/orm.conf 'seq.order_no = seq_order_no
seq.order_no.owned = yes'
  rconf seq.conf "node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
database = sqlite:$WORK/seq.db
packages = ormseq"
  seqerr="clpdaemon: n1: running
clpdaemon: db-nextval: unknown sequence alias 'nope' (no loaded package maps it)
[PRCCODE4] Execution halted during the actions of defrule 'r'."
  ddlrun "orm seq: create and nextval" seq.conf 'create TRUE
next 1 2 TRUE' "$seqerr"
  ddlrun "orm seq: values continue in a second run" seq.conf 'create TRUE
next 3 4 TRUE' "$seqerr"
  cases=$((cases + 1))
  got=$(sqlite3 seq.db "SELECT name || ' ' || value FROM clpd_sequence; SELECT sql FROM sqlite_master WHERE name = 'clpd_sequence'" 2>&1)
  want='seq_order_no 4
CREATE TABLE "clpd_sequence" ("name" TEXT PRIMARY KEY, "value" INTEGER NOT NULL)'
  [ "$got" = "$want" ] || fail "orm seq: the counter row" "got [$got], want [$want]"
else
  echo "cli_test.sh: sqlite3 not found; the orm ddl catalog cases are skipped" >&2
fi

# --- cycle transaction (T10a, docs/database.md#cycle-transaction) -------------
# The retry keys: positive integers, initial not above max (default 30000).
retryerr() {
  conf "$1.conf" "node_id = n1
$2"
  check "cycle retry: $1" 1 "" "$3" -- --config "$1.conf" --check
}
retryerr retry0 'cycle_retry_initial_ms = 0' \
  "clpdaemon: retry0.conf:2: cycle_retry_initial_ms: expected a positive integer, got '0'"
retryerr retrymax 'cycle_retry_max_ms = 1s' \
  "clpdaemon: retrymax.conf:2: cycle_retry_max_ms: expected a positive integer, got '1s'"
retryerr retryorder 'cycle_retry_initial_ms = 500
cycle_retry_max_ms = 100' \
  "clpdaemon: retryorder.conf: cycle_retry_initial_ms (500) must not exceed cycle_retry_max_ms (100)"
retryerr retrydefault 'cycle_retry_initial_ms = 40000' \
  "clpdaemon: retrydefault.conf: cycle_retry_initial_ms (40000) must not exceed cycle_retry_max_ms (30000)"
conf retryok.conf 'node_id = n1
cycle_retry_initial_ms = 250
cycle_retry_max_ms = 250'
check "cycle retry: valid keys" 0 "clpdaemon: retryok.conf: configuration OK" "" -- --config retryok.conf --check

# The mapping key <tpl>.sync (auto or manual) and the reserved instance
# table name.
mkdir ormsync
conf ormsync/package.conf 'definition = sync
version = 1
files = s.clp
orm = orm.conf'
conf ormsync/s.clp '(deftemplate order (slot wf-id))'
conf ormsync/orm.conf 'order.table = sync_order
order.owned = yes
order.sync = sometimes
order.slot.wf-id = wf_id text string error'
conf ormsync.conf "node_id = n1
database = sqlite:$WORK/sync.db
packages = ormsync"
check "orm: bad sync" 1 "" \
  "clpdaemon: ormsync/orm.conf:3: order.sync: expected auto or manual, got 'sometimes'" -- --config ormsync.conf --check
conf ormsync/orm.conf 'order.table = sync_order
order.owned = yes
order.sync = manual
order.slot.wf-id = wf_id text string error'
check "orm: sync manual" 0 "clpdaemon: ormsync.conf: configuration OK" "" -- --config ormsync.conf --check
conf ormsync/orm.conf 'order.table = workflow_instance
order.owned = no
order.slot.wf-id = wf_id text string error'
check "orm: reserved instance table name" 1 "" \
  "clpdaemon: ormsync/orm.conf:1: order.table: table name 'workflow_instance' is reserved (the daemon's workflow instance table)" -- --config ormsync.conf --check

# A package whose rule creates its table, then fails on a database error
# (db-nextval of a sequence that does not exist): the cycle is rolled back
# (the table is gone), cycle_end gets ok=0, one failure line with the wait.
mkdir txpkg
conf txpkg/package.conf 'definition = tx
version = 1
files = tx.clp
orm = orm.conf
ingest = go'
conf txpkg/tx.clp '(deftemplate go (slot wf-id))
(deftemplate order (slot wf-id) (slot note))
(defrule r (go (wf-id ?w)) => (db-create-table order) (db-nextval missing_no))
(deffacts seed (go (wf-id w1)))'
conf txpkg/orm.conf 'order.table = tx_order
order.owned = yes
order.slot.wf-id = wf_id text string error
order.slot.note = note text string nil
seq.missing_no = tx_missing_seq
seq.missing_no.owned = yes'
TXERR="clpdaemon: db-nextval: sequence alias 'missing_no': sequence 'tx_missing_seq': no such table: clpd_sequence
[PRCCODE4] Execution halted during the actions of defrule 'r'."
rconf txhooks.conf "node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
plugins = $PLUGINS/test_hooks_a.so
database = sqlite:$WORK/tx.db
packages = txpkg"
plugdaemon "cycle tx: a failed statement rolls the cycle back" txhooks.conf \
  "clpdaemon: hooks-a: cycle_end ok=0" "clpdaemon: hooks-a: init node_id=n1 post_fact=-1 EINVAL
clpdaemon: hooks-a: after_load
clpdaemon: n1: running
clpdaemon: hooks-a: cycle_begin
$TXERR
clpdaemon: cycle transaction failed: no such table: clpd_sequence; retrying in 100 ms
clpdaemon: hooks-a: cycle_end ok=0
clpdaemon: received SIGTERM, stopping
clpdaemon: hooks-a: shutdown"

# wait_status <socket> <pattern>: waits up to 5 s for a status reply
# matching the grep pattern.
wait_status() {
  n=0
  while ! "$ADMIN" "$1" 'status\n' 2>/dev/null | grep -q "$2"; do
    n=$((n + 1))
    [ "$n" -gt 100 ] && return 1
    sleep 0.05
  done
}
# After the failure the retry (100 ms later) commits, which resets the
# backoff: a later failure waits 100 ms again, not 200. That cycle touched
# instance w2 (its injected go fact), so the rollback evicts it and the
# retry reloads it (T10c): the SELECT fails, because the rolled-back cycle
# also undid the creation of tx_order, which isolates w2.
rconf txreset.conf "node_id = n1
tick_interval_ms = 60000
cycle_min_interval_ms = 10
database = sqlite:$WORK/txreset.db
packages = txpkg"
cases=$((cases + 1))
start txreset.conf admin.sock
wait_status admin.sock ' cycles=2 ' || ok=0
acase "cycle tx: inject after the retry" "OK queued" admin.sock 'inject {"template":"go","slots":{"wf-id":{"sym":"w2"}}}\n'
wait_status admin.sock ' cycles=4 ' || ok=0
kill -TERM "$pid" 2>/dev/null
wait_exit "$pid"
want="clpdaemon: n1: running
$TXERR
clpdaemon: cycle transaction failed: no such table: clpd_sequence; retrying in 100 ms
$TXERR
clpdaemon: cycle transaction failed: no such table: clpd_sequence; retrying in 100 ms
clpdaemon: rehydrate: instance 'w2': tx@1::order: table 'tx_order': no such table: tx_order
clpdaemon: sync: instance 'w2' is no longer synchronized: no such table: tx_order
clpdaemon: received SIGTERM, stopping"
if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ -s out.txt ] || [ "$(cat err.txt)" != "$want" ]; then
  fail "cycle tx: a committed cycle resets the backoff" "exit $rc, lines seen $ok, stdout [$(cat out.txt)], stderr [$(cat err.txt)]"
fi

# During a long backoff the loop serves the admin socket (an inject is
# queued, not run) and SIGTERM stops the daemon at once.
rconf txwait.conf "node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
cycle_retry_initial_ms = 60000
cycle_retry_max_ms = 60000
database = sqlite:$WORK/txwait.db
packages = txpkg"
cases=$((cases + 1))
start txwait.conf admin.sock
wait_line err.txt "clpdaemon: cycle transaction failed: no such table: clpd_sequence; retrying in 60000 ms" || ok=0
acase "cycle tx: status during the backoff" "OK node_id=n1 cycles=1 queue=0 facts=1" admin.sock 'status\n'
acase "cycle tx: inject during the backoff" "OK queued
OK node_id=n1 cycles=1 queue=1 facts=1" admin.sock 'inject {"template":"go","slots":{"wf-id":{"sym":"w2"}}}\n' 'status\n'
sleep 0.3
acase "cycle tx: no cycle during the backoff" "OK node_id=n1 cycles=1 queue=1 facts=1" admin.sock 'status\n'
t0=$(date +%s)
kill -TERM "$pid" 2>/dev/null
wait_exit "$pid"
t1=$(date +%s)
want="clpdaemon: n1: running
$TXERR
clpdaemon: cycle transaction failed: no such table: clpd_sequence; retrying in 60000 ms
clpdaemon: received SIGTERM, stopping
clpdaemon: 1 queued fact dropped at shutdown"
if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ $((t1 - t0)) -gt 3 ] || [ -s out.txt ] || [ "$(cat err.txt)" != "$want" ]; then
  fail "cycle tx: SIGTERM during the backoff" "exit $rc, lines seen $ok, $((t1 - t0)) s, stdout [$(cat out.txt)], stderr [$(cat err.txt)]"
fi

if command -v sqlite3 >/dev/null 2>&1; then
  # The rolled-back cycle left only the daemon's instance table, created at
  # startup because a package has a mapping.
  cases=$((cases + 1))
  got=$(sqlite3 tx.db "SELECT type || ' ' || name || ': ' || coalesce(sql, '') FROM sqlite_master ORDER BY name" 2>&1)
  want='index sqlite_autoindex_workflow_instance_1: 
table workflow_instance: CREATE TABLE "workflow_instance" ("wf_id" TEXT NOT NULL, "definition" TEXT NOT NULL, "version" TEXT NOT NULL, "status" TEXT NOT NULL, "parent_wf_id" TEXT, "owner_node" TEXT, "lease_until" TEXT, "created_at" TEXT NOT NULL, "updated_at" TEXT NOT NULL, PRIMARY KEY ("wf_id"))'
  [ "$got" = "$want" ] || fail "cycle tx: rolled back, workflow_instance created" "got [$got], want [$want]"
  # --check writes nothing: no workflow_instance; nor does a daemon whose
  # packages have no mapping (dbrun.db above).
  conf txcheck.conf "node_id = n1
database = sqlite:$WORK/txcheck.db
packages = txpkg"
  check "cycle tx: --check" 0 "clpdaemon: txcheck.conf: configuration OK" "" -- --config txcheck.conf --check
  cases=$((cases + 1))
  got=$(sqlite3 txcheck.db "SELECT name FROM sqlite_master" 2>&1)$(sqlite3 dbrun.db "SELECT name FROM sqlite_master" 2>&1)
  [ -z "$got" ] || fail "cycle tx: no workflow_instance under --check or without a mapping" "got [$got]"
  # The table cannot be created (a read-only database file): exit 1.
  sqlite3 txro.db 'CREATE TABLE other (x INTEGER)'
  chmod 0444 txro.db
  rconf txro.conf "node_id = n1
database = sqlite:$WORK/txro.db
packages = txpkg"
  RUN='timeout 10'
  check "cycle tx: workflow_instance cannot be created" 1 "" \
    "clpdaemon: cannot create the workflow_instance table: attempt to write a readonly database" -- --config txro.conf
  RUN=
  cases=$((cases + 1))
  [ ! -e admin.sock ] || fail "cycle tx: no admin socket after the failure" "$(ls -l admin.sock)"
  chmod 0644 txro.db
fi

# --- automatic synchronization (T10b, docs/orm.md#automatic-synchronization) --
# The deffacts fact is written by the first cycle (the rule that creates
# the tables runs first in it), a rule's assert of a mapped auto template
# is an INSERT, a fact of a manual template is not written, two modifies
# in one cycle (pay, then ship) are one UPDATE; an injected fact with the
# primary key of a stored row is a unique violation that rolls the cycle
# back, isolates the instance and evicts it without a reload (T10c).
mkdir syncpkg
conf syncpkg/package.conf 'definition = sync
version = 1
files = s.clp
orm = orm.conf
ingest = go, order'
conf syncpkg/s.clp '(deftemplate boot (slot wf-id))
(deftemplate go (slot wf-id) (slot act (type SYMBOL)))
(deftemplate order (slot wf-id) (slot line-no (type INTEGER)) (slot status (type SYMBOL)))
(deftemplate memo (slot wf-id) (slot text))
(deffacts seed (boot (wf-id w0)) (order (wf-id w1) (line-no 1) (status new)))
(defrule boot (boot (wf-id ?w)) => (db-create-table order) (db-create-table memo))
(defrule add (go (wf-id ?w) (act add)) => (assert (order (wf-id ?w) (line-no 2) (status new))) (assert (memo (wf-id ?w) (text "m"))))
(defrule pay (go (wf-id ?w) (act pay)) ?o <- (order (wf-id ?w) (line-no 1) (status new)) => (modify ?o (status paid)))
(defrule ship ?o <- (order (wf-id ?w) (status paid)) => (modify ?o (status shipped)))'
conf syncpkg/orm.conf 'order.table = sync_order
order.owned = yes
order.key = line-no
order.slot.wf-id = wf_id text symbol error
order.slot.line-no = line_no bigint integer error
order.slot.status = status text symbol default
memo.table = sync_memo
memo.owned = yes
memo.sync = manual
memo.slot.wf-id = wf_id text symbol error
memo.slot.text = body text string nil'
rconf syncrun.conf "node_id = n1
tick_interval_ms = 60000
cycle_min_interval_ms = 10
cycle_retry_initial_ms = 60000
cycle_retry_max_ms = 60000
database = sqlite:$WORK/syncrun.db
packages = syncpkg"
SYNCDUP='UNIQUE constraint failed: sync_order.wf_id, sync_order.line_no'
cases=$((cases + 1))
start syncrun.conf admin.sock
wait_status admin.sock ' cycles=1 ' || ok=0
acase "sync: inject add" "OK queued" admin.sock 'inject {"template":"go","slots":{"wf-id":{"sym":"w1"},"act":{"sym":"add"}}}\n'
wait_status admin.sock ' cycles=2 ' || ok=0
acase "sync: inject pay" "OK queued" admin.sock 'inject {"template":"go","slots":{"wf-id":{"sym":"w1"},"act":{"sym":"pay"}}}\n'
wait_status admin.sock ' cycles=3 ' || ok=0
acase "sync: inject a duplicate key" "OK queued" admin.sock 'inject {"template":"order","slots":{"wf-id":{"sym":"w1"},"line-no":1,"status":{"sym":"dup"}}}\n'
wait_line err.txt "clpdaemon: cycle transaction failed: $SYNCDUP; retrying in 60000 ms" || ok=0
kill -TERM "$pid" 2>/dev/null
wait_exit "$pid"
want="clpdaemon: n1: running
clpdaemon: sync: sync@1::order: table 'sync_order': $SYNCDUP
clpdaemon: cycle transaction failed: $SYNCDUP; retrying in 60000 ms
clpdaemon: sync: instance 'w1' is no longer synchronized: $SYNCDUP
clpdaemon: rehydrate: instance 'w1' is not reloaded: it is no longer synchronized
clpdaemon: received SIGTERM, stopping"
if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ -s out.txt ] || [ "$(cat err.txt)" != "$want" ]; then
  fail "sync: a running daemon writes its facts" "exit $rc, lines seen $ok, stdout [$(cat out.txt)], stderr [$(cat err.txt)]"
fi
if command -v sqlite3 >/dev/null 2>&1; then
  cases=$((cases + 1))
  got=$(sqlite3 syncrun.db "SELECT wf_id || '|' || line_no || '|' || status FROM sync_order ORDER BY line_no; SELECT count(*) FROM sync_memo; SELECT wf_id || '|' || definition || '|' || version || '|' || status || '|' || coalesce(parent_wf_id, owner_node, lease_until, '-') || '|' || (created_at < updated_at) || '|' || (updated_at GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:[0-9][0-9].[0-9][0-9][0-9]Z') FROM workflow_instance" 2>&1)
  want='w1|1|shipped
w1|2|new
0
w1|sync|1|active|-|1|1'
  [ "$got" = "$want" ] || fail "sync: the rows after the cycles" "got [$got], want [$want]"
fi

# --- rehydration (T10c, docs/orm.md#rehydration) -----------------------------
# A rolled-back cycle evicts the instance it touched and the retry reloads
# it from the database: the stored row comes back as it was committed (the
# rule's modify is undone), the fact the rule asserted (an unmapped memo)
# and the injected fact that started the cycle are lost, and the untouched
# boot fact stays. A later injected fact sees exactly that working memory
# (the show rule prints it).
mkdir rhpkg
conf rhpkg/package.conf 'definition = rh
version = 1
files = r.clp
orm = orm.conf
ingest = go'
conf rhpkg/r.clp '(deftemplate boot (slot wf-id))
(deftemplate go (slot wf-id) (slot act (type SYMBOL)))
(deftemplate order (slot wf-id) (slot status (type SYMBOL)))
(deftemplate memo (slot wf-id))
(deffacts seed (boot (wf-id w0)) (order (wf-id w1) (status new)))
(defrule boot (boot (wf-id ?w)) => (db-create-table order))
(defrule fail (go (wf-id ?w) (act fail)) ?o <- (order (wf-id ?w)) => (modify ?o (status bad)) (assert (memo (wf-id ?w))) (db-nextval missing_no))
(defrule show (go (wf-id ?w) (act show)) (order (wf-id ?w) (status ?s)) => (printout t "show " ?w " " ?s " memos=" (length$ (find-all-facts ((?m memo)) TRUE)) " gos=" (length$ (find-all-facts ((?g go)) TRUE)) crlf))'
conf rhpkg/orm.conf 'order.table = rh_order
order.owned = yes
order.slot.wf-id = wf_id text symbol error
order.slot.status = status text symbol default
seq.missing_no = rh_missing_seq
seq.missing_no.owned = yes'
rconf rhrun.conf "node_id = n1
tick_interval_ms = 60000
cycle_min_interval_ms = 10
cycle_retry_initial_ms = 100
cycle_retry_max_ms = 100
database = sqlite:$WORK/rhrun.db
packages = rhpkg"
cases=$((cases + 1))
start rhrun.conf admin.sock
wait_status admin.sock ' cycles=1 ' || ok=0
acase "rehydrate: inject a failing go" "OK queued" admin.sock 'inject {"template":"go","slots":{"wf-id":{"sym":"w1"},"act":{"sym":"fail"}}}\n'
wait_status admin.sock ' cycles=3 ' || ok=0
acase "rehydrate: working memory after the retry" "OK node_id=n1 cycles=3 queue=0 facts=2" admin.sock 'status\n'
acase "rehydrate: inject show" "OK queued" admin.sock 'inject {"template":"go","slots":{"wf-id":{"sym":"w1"},"act":{"sym":"show"}}}\n'
wait_line out.txt 'show w1 new memos=0 gos=1' || ok=0
kill -TERM "$pid" 2>/dev/null
wait_exit "$pid"
want="clpdaemon: n1: running
clpdaemon: db-nextval: sequence alias 'missing_no': sequence 'rh_missing_seq': no such table: clpd_sequence
[PRCCODE4] Execution halted during the actions of defrule 'fail'.
clpdaemon: cycle transaction failed: no such table: clpd_sequence; retrying in 100 ms
clpdaemon: received SIGTERM, stopping"
if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ "$(cat out.txt)" != 'show w1 new memos=0 gos=1' ] || [ "$(cat err.txt)" != "$want" ]; then
  fail "rehydrate: a rolled-back cycle reloads its instance" "exit $rc, lines seen $ok, stdout [$(cat out.txt)], stderr [$(cat err.txt)]"
fi
if command -v sqlite3 >/dev/null 2>&1; then
  cases=$((cases + 1))
  got=$(sqlite3 rhrun.db "SELECT wf_id || '|' || status FROM rh_order" 2>&1)
  [ "$got" = 'w1|new' ] || fail "rehydrate: the stored row is unchanged" "got [$got]"
fi

# --- DML functions (T10d, docs/orm.md#dml-functions) -------------------------
# A rule of a running daemon writes, counts and loads the rows of its
# sync = manual template inside the cycle, which commits; a write to a
# sync = auto template is one log line and halts the rule.
mkdir dmlpkg
conf dmlpkg/package.conf 'definition = dml
version = 1
files = d.clp
orm = orm.conf'
conf dmlpkg/d.clp '(deftemplate go (slot wf-id))
(deftemplate memo (slot wf-id) (slot n (type INTEGER)) (slot text))
(deftemplate order (slot wf-id))
(defrule r (go (wf-id ?w)) =>
  (db-create-table memo)
  (printout t "insert " (db-insert memo wf-id ?w n 1 text "hi") crlf)
  (printout t "update " (db-update memo (create$ wf-id ?w n 1) text "ho") crlf)
  (printout t "count " (db-count memo (create$ wf-id ?w)) crlf)
  (printout t "load " (db-load memo (create$ wf-id ?w)) " facts " (length$ (find-all-facts ((?m memo)) TRUE)) crlf)
  (printout t "delete " (db-delete memo (create$ wf-id ?w n 2)) crlf)
  (db-insert order wf-id ?w)
  (printout t "not reached" crlf))
(deffacts seed (go (wf-id w1)))'
conf dmlpkg/orm.conf 'memo.table = dml_memo
memo.owned = yes
memo.sync = manual
memo.key = n
memo.slot.wf-id = wf_id text symbol error
memo.slot.n = n bigint integer error
memo.slot.text = body text string nil
order.table = dml_order
order.owned = yes
order.slot.wf-id = wf_id text symbol error'
rconf dml.conf "node_id = n1
tick_interval_ms = 20
cycle_min_interval_ms = 10
database = sqlite:$WORK/dml.db
packages = dmlpkg"
ddlrun "orm dml: a rule writes, counts and loads its rows" dml.conf 'insert TRUE
update 1
count 1
load 1 facts 1
delete 0' "clpdaemon: n1: running
clpdaemon: db-insert: template 'order': table 'dml_order' has sync = auto: only the automatic synchronization writes it
[PRCCODE4] Execution halted during the actions of defrule 'r'."
if command -v sqlite3 >/dev/null 2>&1; then
  cases=$((cases + 1))
  got=$(sqlite3 dml.db "SELECT wf_id || '|' || n || '|' || body FROM dml_memo; SELECT count(*) FROM workflow_instance" 2>&1)
  want='w1|1|ho
0'
  [ "$got" = "$want" ] || fail "orm dml: the committed row" "got [$got], want [$want]"
fi

# --- T10 verify follow-ups (T10e) --------------------------------------------
# B1: a wf-id taken from a fact is shown in a log line with its control
# characters as '?', so an injected wf-id cannot forge a line.
mkdir b1pkg
conf b1pkg/package.conf 'definition = b1
version = 1
files = b.clp
orm = orm.conf
ingest = bad'
conf b1pkg/b.clp '(deftemplate bad (slot wf-id) (slot n))'
conf b1pkg/orm.conf 'bad.table = b1_bad
bad.owned = no
bad.slot.wf-id = wf_id text string error
bad.slot.n = n bigint integer nil'
rconf b1.conf "node_id = n1
tick_interval_ms = 60000
cycle_min_interval_ms = 10
database = sqlite:$WORK/b1.db
packages = b1pkg"
B1WHY="slot 'n' (column 'n'): expected integer, got STRING"
cases=$((cases + 1))
start b1.conf admin.sock
wait_status admin.sock ' cycles=1 ' || ok=0
acase "b1: inject a forged wf-id" "OK queued" admin.sock 'inject {"template":"bad","slots":{"wf-id":"a\\u0007b\\nclpdaemon: forged","n":"x"}}\n'
wait_has err.txt "is not reloaded" || ok=0
kill -TERM "$pid" 2>/dev/null
wait_exit "$pid"
want="clpdaemon: n1: running
clpdaemon: sync: b1@1::bad: table 'b1_bad': $B1WHY
clpdaemon: cycle transaction failed: $B1WHY; retrying in 100 ms
clpdaemon: sync: instance 'a?b?clpdaemon: forged' is no longer synchronized: $B1WHY
clpdaemon: rehydrate: instance 'a?b?clpdaemon: forged' is not reloaded: it is no longer synchronized
clpdaemon: received SIGTERM, stopping"
if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ -s out.txt ] || [ "$(cat err.txt)" != "$want" ] ||
  ! grep -qF "instance 'a?b?clpdaemon: forged' is no longer synchronized" err.txt ||
  grep -q '^clpdaemon: forged' err.txt; then
  fail "b1: the isolation line shows the wf-id sanitized" "exit $rc, lines seen $ok, stdout [$(cat out.txt)], stderr [$(cat err.txt)]"
fi

# A1: instance_isolate_after, a positive integer (default 3).
retryerr isolate0 'instance_isolate_after = 0' \
  "clpdaemon: isolate0.conf:2: instance_isolate_after: expected a positive integer, got '0'"
retryerr isolatex 'instance_isolate_after = three' \
  "clpdaemon: isolatex.conf:2: instance_isolate_after: expected a positive integer, got 'three'"
conf isolateok.conf 'node_id = n1
instance_isolate_after = 1'
check "cycle retry: instance_isolate_after" 0 "clpdaemon: isolateok.conf: configuration OK" "" -- --config isolateok.conf --check

# A1 poison retry: another process sets the stored row of w1 to poison;
# a rule touches w1 and fails, so the rollback reloads w1, whose poison
# row activates a rule that fails the same way on every retry. The third
# rolled-back cycle attributed to w1 (the wf-id of the failing rule)
# isolates it, and the next cycle commits: w2's row is written.
# a1case <tag> <name> <action> <failure lines> <failure message>: the
# package in <tag>pkg, the database <tag>.db.
a1case() {
  a1=$1 a1name=$2 a1act=$3 a1lines=$4 a1msg=$5
  mkdir "${a1}pkg"
  conf "${a1}pkg"/package.conf 'definition = a1
version = 1
files = a.clp
orm = orm.conf
ingest = go, order'
  conf "${a1}pkg"/a.clp "(deftemplate boot (slot wf-id))
(deftemplate go (slot wf-id) (slot act (type SYMBOL)))
(deftemplate order (slot wf-id) (slot status (type SYMBOL)))
(deftemplate memo (slot wf-id) (slot text))
(deffacts seed (boot (wf-id w0)))
(defrule boot (boot (wf-id ?w)) => (db-create-table order) (db-create-table memo))
(defrule fail (go (wf-id ?w) (act fail)) ?o <- (order (wf-id ?w)) => (modify ?o (status touched)) $a1act)
(defrule poison ?o <- (order (wf-id ?w) (status poison)) => (modify ?o (status cured)) $a1act)"
  conf "${a1}pkg"/orm.conf 'order.table = a1_order
order.owned = yes
order.slot.wf-id = wf_id text symbol error
order.slot.status = status text symbol default
memo.table = a1_memo
memo.owned = yes
memo.sync = manual
memo.slot.wf-id = wf_id text symbol error
memo.slot.text = body text string nil
seq.missing_no = a1_missing_seq
seq.missing_no.owned = yes'
  rconf "$a1.conf" "node_id = n1
tick_interval_ms = 60000
cycle_min_interval_ms = 10
cycle_retry_initial_ms = 100
cycle_retry_max_ms = 100
database = sqlite:$WORK/$a1.db
packages = ${a1}pkg"
  cases=$((cases + 1))
  start "$a1.conf" admin.sock
  wait_status admin.sock ' cycles=1 ' || ok=0
  acase "$a1name: inject w1" "OK queued" admin.sock 'inject {"template":"order","slots":{"wf-id":{"sym":"w1"},"status":{"sym":"new"}}}\n'
  wait_status admin.sock ' cycles=2 ' || ok=0
  sqlite3 "$a1.db" "UPDATE a1_order SET status = 'poison' WHERE wf_id = 'w1'; INSERT INTO a1_memo VALUES ('w1', 'old')"
  acase "$a1name: inject a failing go" "OK queued" admin.sock 'inject {"template":"go","slots":{"wf-id":{"sym":"w1"},"act":{"sym":"fail"}}}\n'
  wait_line err.txt "clpdaemon: sync: instance 'w1' is no longer synchronized: $a1msg (3 rolled-back cycles in a row)" || ok=0
  acase "$a1name: inject w2" "OK queued" admin.sock 'inject {"template":"order","slots":{"wf-id":{"sym":"w2"},"status":{"sym":"new"}}}\n'
  n=0
  while [ "$(sqlite3 "$a1.db" "SELECT status FROM a1_order WHERE wf_id = 'w2'" 2>/dev/null)" != new ]; do
    n=$((n + 1))
    [ "$n" -gt 40 ] && { ok=0; break; }
    sleep 0.05
  done
  kill -TERM "$pid" 2>/dev/null
  wait_exit "$pid"
  want="clpdaemon: n1: running
$(printf "$a1lines" fail)
clpdaemon: cycle transaction failed: $a1msg; retrying in 100 ms
$(printf "$a1lines" poison)
clpdaemon: cycle transaction failed: $a1msg; retrying in 100 ms
$(printf "$a1lines" poison)
clpdaemon: cycle transaction failed: $a1msg; retrying in 100 ms
clpdaemon: sync: instance 'w1' is no longer synchronized: $a1msg (3 rolled-back cycles in a row)
clpdaemon: rehydrate: instance 'w1' is not reloaded: it is no longer synchronized
clpdaemon: received SIGTERM, stopping"
  if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ -s out.txt ] || [ "$(cat err.txt)" != "$want" ]; then
    fail "$a1name: w1 isolated, w2 written" "exit $rc, lines seen $ok, stdout [$(cat out.txt)], stderr [$(cat err.txt)]"
  fi
  cases=$((cases + 1))
  got=$(sqlite3 "$a1.db" "SELECT wf_id || '|' || status FROM a1_order ORDER BY wf_id; SELECT wf_id || '|' || body FROM a1_memo" 2>&1)
  want='w1|poison
w2|new
w1|old'
  [ "$got" = "$want" ] || fail "$a1name: the stored rows" "got [$got], want [$want]"
}
if command -v sqlite3 >/dev/null 2>&1; then
  a1case a1n "a1 nextval" '(db-nextval missing_no)' \
    "clpdaemon: db-nextval: sequence alias 'missing_no': sequence 'a1_missing_seq': no such table: clpd_sequence\n[PRCCODE4] Execution halted during the actions of defrule '%s'." \
    'no such table: clpd_sequence'
  a1case a1i "a1 insert" '(db-insert memo wf-id ?w text "x")' \
    "clpdaemon: db-insert: template 'memo': table 'a1_memo': UNIQUE constraint failed: a1_memo.wf_id\n[PRCCODE4] Execution halted during the actions of defrule '%s'." \
    'UNIQUE constraint failed: a1_memo.wf_id'

  # T10b against an audit trigger on a table that is not owned, created
  # beforehand: two modifies in one cycle are one UPDATE, a modify back to
  # the stored values writes nothing, a key change is a DELETE then an
  # INSERT; nil in a null nil column is NULL, in a column of another null
  # policy the text nil.
  sqlite3 audit.db "CREATE TABLE au_order (wf_id TEXT NOT NULL, line_no INTEGER NOT NULL, status TEXT, a TEXT, b TEXT, PRIMARY KEY (wf_id, line_no));
CREATE TABLE audit (seq INTEGER PRIMARY KEY AUTOINCREMENT, op TEXT, wf_id TEXT, line_no INTEGER, status TEXT, a TEXT, b TEXT);
CREATE TRIGGER au_i AFTER INSERT ON au_order BEGIN INSERT INTO audit (op, wf_id, line_no, status, a, b) VALUES ('I', NEW.wf_id, NEW.line_no, NEW.status, NEW.a, NEW.b); END;
CREATE TRIGGER au_u AFTER UPDATE ON au_order BEGIN INSERT INTO audit (op, wf_id, line_no, status, a, b) VALUES ('U', NEW.wf_id, NEW.line_no, NEW.status, NEW.a, NEW.b); END;
CREATE TRIGGER au_d AFTER DELETE ON au_order BEGIN INSERT INTO audit (op, wf_id, line_no, status, a, b) VALUES ('D', OLD.wf_id, OLD.line_no, OLD.status, OLD.a, OLD.b); END;"
  mkdir aupkg
  conf aupkg/package.conf 'definition = au
version = 1
files = a.clp
orm = orm.conf
ingest = go, order'
  conf aupkg/a.clp '(deftemplate go (slot wf-id) (slot act (type SYMBOL)))
(deftemplate order (slot wf-id) (slot line-no (type INTEGER)) (slot status (type SYMBOL)) (slot a (type SYMBOL)) (slot b (type SYMBOL)))
(defrule pay (go (wf-id ?w) (act pay)) ?o <- (order (wf-id ?w) (status new)) => (modify ?o (status paid)))
(defrule ship ?o <- (order (wf-id ?w) (status paid)) => (modify ?o (status shipped)))
(defrule flip ?g <- (go (wf-id ?w) (act flip)) ?o <- (order (wf-id ?w) (status shipped)) => (retract ?g) (modify ?o (status tmp)))
(defrule flop ?o <- (order (wf-id ?w) (status tmp)) => (modify ?o (status shipped)))
(defrule rekey (go (wf-id ?w) (act rekey)) ?o <- (order (wf-id ?w) (line-no 1)) => (modify ?o (line-no 2)))'
  conf aupkg/orm.conf 'order.table = au_order
order.owned = no
order.key = line-no
order.slot.wf-id = wf_id text symbol error
order.slot.line-no = line_no bigint integer error
order.slot.status = status text symbol default
order.slot.a = a text symbol nil
order.slot.b = b text symbol default'
  rconf au.conf "node_id = n1
tick_interval_ms = 60000
cycle_min_interval_ms = 10
database = sqlite:$WORK/audit.db
packages = aupkg"
  AUDIT="SELECT op || '|' || wf_id || '|' || line_no || '|' || status || '|' || coalesce(a, 'NULL') || '|' || coalesce(b, 'NULL') FROM audit ORDER BY seq"
  cases=$((cases + 1))
  start au.conf admin.sock
  wait_status admin.sock ' cycles=1 ' || ok=0
  acase "audit: inject the order" "OK queued" admin.sock 'inject {"template":"order","slots":{"wf-id":{"sym":"w1"},"line-no":1,"status":{"sym":"new"},"a":{"sym":"nil"},"b":{"sym":"nil"}}}\n'
  wait_status admin.sock ' cycles=2 ' || ok=0
  au1=$(sqlite3 audit.db "$AUDIT" 2>&1)
  acase "audit: two modifies" "OK queued" admin.sock 'inject {"template":"go","slots":{"wf-id":{"sym":"w1"},"act":{"sym":"pay"}}}\n'
  wait_status admin.sock ' cycles=3 ' || ok=0
  au2=$(sqlite3 audit.db "$AUDIT" 2>&1)
  acase "audit: a modify back" "OK queued" admin.sock 'inject {"template":"go","slots":{"wf-id":{"sym":"w1"},"act":{"sym":"flip"}}}\n'
  wait_status admin.sock ' cycles=4 ' || ok=0
  au3=$(sqlite3 audit.db "$AUDIT" 2>&1)
  acase "audit: a key change" "OK queued" admin.sock 'inject {"template":"go","slots":{"wf-id":{"sym":"w1"},"act":{"sym":"rekey"}}}\n'
  wait_status admin.sock ' cycles=5 ' || ok=0
  au4=$(sqlite3 audit.db "$AUDIT" 2>&1)
  kill -TERM "$pid" 2>/dev/null
  wait_exit "$pid"
  want="clpdaemon: n1: running
clpdaemon: received SIGTERM, stopping"
  if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ -s out.txt ] || [ "$(cat err.txt)" != "$want" ]; then
    fail "audit: the daemon" "exit $rc, lines seen $ok, stdout [$(cat out.txt)], stderr [$(cat err.txt)]"
  fi
  AU1='I|w1|1|new|NULL|nil'
  AU2="$AU1
U|w1|1|shipped|NULL|nil"
  AU4="$AU2
D|w1|1|shipped|NULL|nil
I|w1|2|shipped|NULL|nil"
  cases=$((cases + 1))
  [ "$au1" = "$AU1" ] && [ "$au2" = "$AU2" ] && [ "$au3" = "$AU2" ] && [ "$au4" = "$AU4" ] ||
    fail "audit: the trigger rows" "after the insert [$au1], two modifies [$au2], a modify back [$au3], a key change [$au4]"
  cases=$((cases + 1))
  got=$(sqlite3 audit.db "SELECT wf_id || '|' || line_no || '|' || status || '|' || coalesce(a, 'NULL') || '|' || coalesce(b, 'NULL') FROM au_order; SELECT wf_id || '|' || definition FROM workflow_instance" 2>&1)
  want='w1|2|shipped|NULL|nil
w1|au'
  [ "$got" = "$want" ] || fail "audit: the stored row" "got [$got], want [$want]"
else
  echo "cli_test.sh: sqlite3 not found; the T10e A1 and audit cases are skipped" >&2
fi

# --- transient contention (T10f) --------------------------------------------
# Another connection holds an EXCLUSIVE lock on the database file while the
# daemon must reload an instance a rolled-back cycle evicted: the reload
# SELECT fails with "database is locked" (SQLITE_BUSY after the 5 s busy
# timeout), which is transient: w1 is not isolated, the start of the
# cycle is retried, and once the lock is released w1 is reloaded and its
# rule's modify is written. The first failure (w1's rule, a missing
# sequence) counts for w1; the two reload failures that follow neither
# count nor reset, so with instance_isolate_after = 2 three failures in a
# row isolate nothing.
if command -v sqlite3 >/dev/null 2>&1; then
  mkdir tfpkg
  conf tfpkg/package.conf 'definition = tf
version = 1
files = a.clp
orm = orm.conf
ingest = go, order'
  conf tfpkg/a.clp '(deftemplate boot (slot wf-id))
(deftemplate go (slot wf-id))
(deftemplate order (slot wf-id) (slot status (type SYMBOL)))
(deffacts seed (boot (wf-id w0)))
(defrule boot (boot (wf-id ?w)) => (db-create-table order))
(defrule fail (go (wf-id ?w)) ?o <- (order (wf-id ?w)) => (modify ?o (status touched)) (db-nextval missing_no))
(defrule stored ?o <- (order (wf-id ?w) (status stored)) => (modify ?o (status cured)))'
  conf tfpkg/orm.conf 'order.table = tf_order
order.owned = yes
order.slot.wf-id = wf_id text symbol error
order.slot.status = status text symbol default
seq.missing_no = tf_missing_seq
seq.missing_no.owned = yes'
  rconf tf.conf "node_id = n1
tick_interval_ms = 60000
cycle_min_interval_ms = 10
cycle_retry_initial_ms = 1000
cycle_retry_max_ms = 1000
instance_isolate_after = 2
database = sqlite:$WORK/tf.db
packages = tfpkg"
  cases=$((cases + 1))
  start tf.conf admin.sock
  wait_status admin.sock ' cycles=1 ' || ok=0
  acase "tf: inject w1" "OK queued" admin.sock 'inject {"template":"order","slots":{"wf-id":{"sym":"w1"},"status":{"sym":"new"}}}\n'
  wait_status admin.sock ' cycles=2 ' || ok=0
  sqlite3 tf.db "UPDATE tf_order SET status = 'stored' WHERE wf_id = 'w1'"
  acase "tf: inject a go" "OK queued" admin.sock 'inject {"template":"go","slots":{"wf-id":{"sym":"w1"}}}\n'
  TFFAIL='clpdaemon: cycle transaction failed: no such table: clpd_sequence; retrying in 1000 ms'
  wait_line err.txt "$TFFAIL" || ok=0
  # The lock, taken during the 1 s wait before the reload and held for 15 s:
  # the reloads at about 1 s and 7 s wait 5 s each and fail, the one at
  # about 13 s gets the database when the lock is released.
  printf '%s\n' '.timeout 5000' 'BEGIN EXCLUSIVE;' '.shell touch tflocked' \
    '.shell sleep 15' 'COMMIT;' | sqlite3 tf.db >tfholder.txt 2>&1 &
  hpid=$!
  n=0
  while [ ! -e tflocked ]; do
    n=$((n + 1))
    [ "$n" -gt 40 ] && { ok=0; break; }
    sleep 0.05
  done
  wait "$hpid"
  hrc=$?
  n=0
  while [ "$(sqlite3 tf.db "SELECT status FROM tf_order WHERE wf_id = 'w1'" 2>/dev/null)" != cured ]; do
    n=$((n + 1))
    [ "$n" -gt 400 ] && { ok=0; break; }
    sleep 0.05
  done
  kill -TERM "$pid" 2>/dev/null
  wait_exit "$pid"
  TFRH="clpdaemon: rehydrate: instance 'w1': tf@1::order: table 'tf_order': database is locked"
  TFRETRY='clpdaemon: cycle transaction failed: database is locked; retrying in 1000 ms'
  k=$(grep -cxF "$TFRH" err.txt)
  want="clpdaemon: n1: running
clpdaemon: db-nextval: sequence alias 'missing_no': sequence 'tf_missing_seq': no such table: clpd_sequence
[PRCCODE4] Execution halted during the actions of defrule 'fail'.
$TFFAIL"
  i=0
  while [ "$i" -lt "$k" ]; do
    want="$want
$TFRH
$TFRETRY"
    i=$((i + 1))
  done
  want="$want
clpdaemon: received SIGTERM, stopping"
  if [ "$ok" != 1 ] || [ "$rc" != 0 ] || [ "$hrc" != 0 ] || [ "$k" -lt 2 ] || [ -s out.txt ] ||
    [ "$(cat err.txt)" != "$want" ] || grep -q 'is no longer synchronized' err.txt; then
    fail "tf: a locked reload is retried, not isolated" "exit $rc, lines seen $ok, holder exit $hrc [$(cat tfholder.txt)], reload failures $k, stdout [$(cat out.txt)], stderr [$(cat err.txt)]"
  fi
  cases=$((cases + 1))
  got=$(sqlite3 tf.db "SELECT wf_id || '|' || status FROM tf_order ORDER BY wf_id; SELECT count(*) FROM workflow_instance WHERE wf_id = 'w1'" 2>&1)
  want='w1|cured
1'
  [ "$got" = "$want" ] || fail "tf: the stored rows" "got [$got], want [$want]"
else
  echo "cli_test.sh: sqlite3 not found; the T10f case is skipped" >&2
fi

printf '%d/%d CLI cases passed\n' $((cases - failures)) "$cases"
[ "$failures" -eq 0 ]
