#!/bin/sh
# CLI cases of a running daemon against PostgreSQL (T10b), run by
# tests/db/pg_cluster.sh on its throwaway cluster:
#   pg_cluster.sh /bin/sh cli_pg_test.sh <clpdaemon> <admin-client>
# which appends "<conninfo> <password>" (PGPASSFILE is exported).
# Cases: the automatic synchronization writes the rows and the
# workflow_instance row; a lost server starts a backoff during which the
# admin socket is still served, also when its listener was paused because
# the daemon ran out of descriptors (the failed begin/reconnect path
# resumes it, L91 R3-003); after the server is back the cycles commit.
set -u

U='usage: cli_pg_test.sh <clpdaemon> <admin-client> <conninfo> <password>'
BIN=${1:?$U}
ADMIN=${2:?$U}
CONNINFO=${3:?$U}
PASSWORD=${4:?$U}
case $BIN in /*) ;; *) BIN=$PWD/$BIN ;; esac
case $ADMIN in /*) ;; *) ADMIN=$PWD/$ADMIN ;; esac

if [ -n "${CLPD_PG_BINDIR:-}" ]; then PSQL=$CLPD_PG_BINDIR/psql; else PSQL=$(command -v psql); fi
WORK=$(mktemp -d) || exit 2
pid=
trap 'if [ -n "$pid" ]; then kill -KILL "$pid" 2>/dev/null; fi; rm -rf "$WORK"' EXIT
cd "$WORK" || exit 2
failures=0
cases=0
NL='
'

fail() {
  failures=$((failures + 1))
  printf 'FAIL: %s\n  %s\n' "$1" "$2"
}
# q <sql>: one query as the test role, unaligned, no headers.
q() { "$PSQL" -X -q -A -t -v ON_ERROR_STOP=1 -d "$CONNINFO" -c "$1" 2>&1; }
# qa <sql>: as the cluster's superuser (connection limits do not apply).
qa() { "$PSQL" -X -q -A -t -v ON_ERROR_STOP=1 -d "$CONNINFO user=clpd_admin" -c "$1" 2>&1; }
wait_has() {
  n=0
  while ! grep -qF "$2" "$1" 2>/dev/null; do
    n=$((n + 1))
    [ "$n" -gt 200 ] && return 1
    sleep 0.05
  done
}
# wait_q <sql> <want>: up to 10 s for the query to return want.
wait_q() {
  n=0
  while [ "$(q "$1")" != "$2" ]; do
    n=$((n + 1))
    [ "$n" -gt 200 ] && return 1
    sleep 0.05
  done
}

mkdir pkg
printf '%s\n' 'definition = pgsync
version = 2
files = r.clp
orm = orm.conf
ingest = go' >pkg/package.conf
printf '%s\n' '(deftemplate boot (slot wf-id))
(deftemplate go (slot wf-id))
(deftemplate item (slot wf-id) (slot status (type SYMBOL)))
(deffacts seed (boot (wf-id w0)) (item (wf-id w1) (status new)))
(defrule boot (boot (wf-id ?w)) => (db-create-table item))
(defrule done (go (wf-id ?w)) ?i <- (item (wf-id ?w) (status new)) => (modify ?i (status done)))' >pkg/r.clp
printf '%s\n' 'item.table = pg_item
item.owned = yes
item.slot.wf-id = wf_id text symbol error
item.slot.status = status text symbol default' >pkg/orm.conf
printf '%s\n' "node_id = n1
tick_interval_ms = 50
cycle_min_interval_ms = 10
cycle_retry_initial_ms = 200
cycle_retry_max_ms = 200
admin_socket = $WORK/admin.sock
database = $CONNINFO
packages = pkg" >run.conf

q 'DROP TABLE IF EXISTS pg_item' >/dev/null
q 'DROP TABLE IF EXISTS workflow_instance' >/dev/null
"$BIN" --config run.conf >out.txt 2>err.txt &
pid=$!

# The first cycle creates the table (rule) and writes the deffacts fact.
cases=$((cases + 1))
wait_q "SELECT wf_id || ' ' || status FROM pg_item" 'w1 new' ||
  fail "pg sync: the seed row" "got [$(q 'SELECT * FROM pg_item')], stderr [$(cat err.txt)]"
cases=$((cases + 1))
got=$(q "SELECT wf_id || ' ' || definition || ' ' || version || ' ' || status || ' ' || (created_at = updated_at)::int FROM workflow_instance")
[ "$got" = 'w1 pgsync 2 active 1' ] || fail "pg sync: the instance row" "got [$got]"

# Out of descriptors: the next admin connection cannot be accepted and the
# listener is paused. Then the server goes away for the daemon: its
# backend is terminated and the database refuses new connections, so the
# next cycle's begin fails (lost), and each retry's reconnect fails. The
# reconnect closes the lost connection's descriptor; the failed-begin path
# resumes the listener, so the waiting client is served during the
# backoff.
# The limit is the lowest free descriptor number: every new descriptor
# fails with EMFILE until one below it is closed.
free=0
for fd in $(ls /proc/"$pid"/fd | sort -n); do
  [ "$fd" = "$free" ] && free=$((free + 1))
done
prlimit --pid "$pid" --nofile="$free:$free" 2>/dev/null || free=
qa "ALTER DATABASE clpd_test CONNECTION LIMIT 0" >/dev/null
"$ADMIN" admin.sock 'status\n' >status.txt 2>&1 &
cpid=$!
sleep 0.3
qa "SELECT count(pg_terminate_backend(pid)) FROM pg_stat_activity WHERE datname = 'clpd_test' AND usename = 'clpd_test'" >/dev/null
cases=$((cases + 1))
wait_has err.txt 'too many connections for database' ||
  fail "pg sync: the reconnect fails" "stderr [$(cat err.txt)]"
n=0
while kill -0 "$cpid" 2>/dev/null && [ "$n" -lt 100 ]; do n=$((n + 1)); sleep 0.05; done
cases=$((cases + 1))
if kill -0 "$cpid" 2>/dev/null; then
  kill -KILL "$cpid" 2>/dev/null
  fail "pg sync: admin status during the backoff" "not answered (descriptor limit ${free:-not set}), stderr [$(cat err.txt)]"
else
  wait "$cpid"
  case $(cat status.txt) in
  'OK node_id=n1 cycles='*) ;;
  *) fail "pg sync: admin status during the backoff" "got [$(cat status.txt)]" ;;
  esac
fi
[ -n "$free" ] && prlimit --pid "$pid" --nofile=1024:1024 2>/dev/null
cases=$((cases + 1))
grep -q '^clpdaemon: cycle transaction failed: .*; retrying in 200 ms$' err.txt ||
  fail "pg sync: the failure line" "stderr [$(cat err.txt)]"
case $(cat err.txt) in *"$PASSWORD"*|*"$CONNINFO"*) fail "pg sync: no secret in the log" "stderr [$(cat err.txt)]" ;; esac

# The server is back: the next retry reconnects and the cycles commit; an
# injected fact's rule modifies the row.
qa "ALTER DATABASE clpd_test CONNECTION LIMIT -1" >/dev/null
cases=$((cases + 1))
"$ADMIN" admin.sock 'inject {"template":"go","slots":{"wf-id":{"sym":"w1"}}}\n' >inject.txt 2>&1
[ "$(cat inject.txt)" = 'OK queued' ] || fail "pg sync: inject after the reconnect" "got [$(cat inject.txt)]"
wait_q "SELECT status FROM pg_item WHERE wf_id = 'w1'" 'done' ||
  fail "pg sync: written after the reconnect" "got [$(q 'SELECT * FROM pg_item')], stderr [$(cat err.txt)]"
cases=$((cases + 1))
got=$(q "SELECT (updated_at > created_at)::int FROM workflow_instance WHERE wf_id = 'w1'")
[ "$got" = 1 ] || fail "pg sync: updated_at moved" "got [$got]"

kill -TERM "$pid" 2>/dev/null
n=0
while kill -0 "$pid" 2>/dev/null && [ "$n" -lt 100 ]; do n=$((n + 1)); sleep 0.05; done
cases=$((cases + 1))
if kill -0 "$pid" 2>/dev/null; then
  fail "pg sync: SIGTERM" "still running"
else
  wait "$pid"
  rc=$?
  pid=
  last=$(tail -n 1 err.txt)
  [ "$rc" = 0 ] && [ "$last" = 'clpdaemon: received SIGTERM, stopping' ] && [ ! -s out.txt ] ||
    fail "pg sync: SIGTERM" "exit $rc, stdout [$(cat out.txt)], stderr [$(cat err.txt)]"
fi
q 'DROP TABLE IF EXISTS pg_item' >/dev/null
q 'DROP TABLE IF EXISTS workflow_instance' >/dev/null

printf '%d/%d PostgreSQL CLI cases passed\n' $((cases - failures)) "$cases"
[ "$failures" -eq 0 ]
