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
# Then a rolled-back cycle (a rule's db-nextval of a missing sequence)
# evicts the instance it touched and the retry reloads it from PostgreSQL
# (T10c): the rule's modify is undone, its injected fact is lost.
# T10e: the poison retry isolates an instance whose rule fails on every
# retry (db-nextval of a missing sequence, a duplicate db-insert) and the
# other instance is written; the automatic synchronization against an
# audit trigger on a table that is not owned.
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
(deftemplate go (slot wf-id) (slot act (type SYMBOL)))
(deftemplate item (slot wf-id) (slot status (type SYMBOL)))
(deffacts seed (boot (wf-id w0)) (item (wf-id w1) (status new)))
(defrule boot (boot (wf-id ?w)) => (db-create-table item))
(defrule done (go (wf-id ?w)) ?i <- (item (wf-id ?w) (status new)) => (modify ?i (status done)))
(defrule fail (go (wf-id ?w) (act fail)) ?i <- (item (wf-id ?w)) => (modify ?i (status bad)) (db-nextval missing_no))
(defrule show (go (wf-id ?w) (act show)) (item (wf-id ?w) (status ?s)) => (printout t "show " ?w " " ?s " gos=" (length$ (find-all-facts ((?g go)) TRUE)) crlf))' >pkg/r.clp
printf '%s\n' 'item.table = pg_item
item.owned = yes
item.slot.wf-id = wf_id text symbol error
item.slot.status = status text symbol default
seq.missing_no = pg_missing_seq
seq.missing_no.owned = yes' >pkg/orm.conf
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
# Only the soft limit is lowered, so the restore below can raise it again
# for a runner that is not root (the hard limit stays).
soft=$(prlimit --pid "$pid" --nofile --output SOFT --noheadings 2>/dev/null | tr -d ' ')
prlimit --pid "$pid" --nofile="$free:" 2>/dev/null || free=
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
[ -n "$free" ] && prlimit --pid "$pid" --nofile="${soft:-1024}:" 2>/dev/null
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

# Rehydration: the failing cycle modifies the row's fact, then fails; the
# retry reloads the stored row (done), and the injected go facts are lost.
FAILMSG='clpdaemon: cycle transaction failed: relation "public.pg_missing_seq" does not exist (SQLSTATE 42P01); retrying in 200 ms'
cases=$((cases + 1))
"$ADMIN" admin.sock 'inject {"template":"go","slots":{"wf-id":{"sym":"w1"},"act":{"sym":"fail"}}}\n' >inject.txt 2>&1
wait_has err.txt "$FAILMSG" || fail "pg rehydrate: the cycle is rolled back" "stderr [$(cat err.txt)]"
sleep 0.5
"$ADMIN" admin.sock 'inject {"template":"go","slots":{"wf-id":{"sym":"w1"},"act":{"sym":"show"}}}\n' >>inject.txt 2>&1
wait_has out.txt 'show w1' || true
[ "$(cat out.txt)" = 'show w1 done gos=1' ] && [ "$(cat inject.txt)" = "OK queued${NL}OK queued" ] ||
  fail "pg rehydrate: the instance is reloaded" "stdout [$(cat out.txt)], inject [$(cat inject.txt)], stderr [$(cat err.txt)]"
cases=$((cases + 1))
got=$(q "SELECT status FROM pg_item WHERE wf_id = 'w1'")
[ "$got" = done ] || fail "pg rehydrate: the stored row is unchanged" "got [$got]"
: >out.txt

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

# stop_daemon <name>: SIGTERM, then exit 0 with the stopping line last.
stop_daemon() {
  kill -TERM "$pid" 2>/dev/null
  n=0
  while kill -0 "$pid" 2>/dev/null && [ "$n" -lt 100 ]; do n=$((n + 1)); sleep 0.05; done
  cases=$((cases + 1))
  if kill -0 "$pid" 2>/dev/null; then
    fail "$1: SIGTERM" "still running"
  else
    wait "$pid"
    rc=$?
    pid=
    [ "$rc" = 0 ] && [ "$(tail -n 1 err.txt)" = 'clpdaemon: received SIGTERM, stopping' ] ||
      fail "$1: SIGTERM" "exit $rc, stderr [$(cat err.txt)]"
  fi
}

# A1 poison retry (T10e): the stored row of w1 is set to poison by another
# session; a rule touches w1 and fails, the rollback reloads the poison
# row, whose rule fails the same way on every retry; the third rolled-back
# cycle attributed to w1 isolates it and w2's row is written within 2 s.
# pg_a1 <tag> <name> <action> <failure message>: the package in <tag>pkg.
pg_a1() {
  a1=$1 a1name=$2 a1act=$3 a1msg=$4
  mkdir "${a1}pkg"
  printf '%s\n' 'definition = pga1
version = 1
files = a.clp
orm = orm.conf
ingest = go, order' >"${a1}pkg"/package.conf
  printf '%s\n' "(deftemplate boot (slot wf-id))
(deftemplate go (slot wf-id) (slot act (type SYMBOL)))
(deftemplate order (slot wf-id) (slot status (type SYMBOL)))
(deftemplate memo (slot wf-id) (slot text))
(deffacts seed (boot (wf-id w0)))
(defrule boot (boot (wf-id ?w)) => (db-create-table order) (db-create-table memo))
(defrule fail (go (wf-id ?w) (act fail)) ?o <- (order (wf-id ?w)) => (modify ?o (status touched)) $a1act)
(defrule poison ?o <- (order (wf-id ?w) (status poison)) => (modify ?o (status cured)) $a1act)" >"${a1}pkg"/a.clp
  printf '%s\n' 'order.table = pga1_order
order.owned = yes
order.slot.wf-id = wf_id text symbol error
order.slot.status = status text symbol default
memo.table = pga1_memo
memo.owned = yes
memo.sync = manual
memo.slot.wf-id = wf_id text symbol error
memo.slot.text = body text string nil
seq.missing_no = pga1_missing_seq
seq.missing_no.owned = yes' >"${a1}pkg"/orm.conf
  printf '%s\n' "node_id = n1
tick_interval_ms = 60000
cycle_min_interval_ms = 10
cycle_retry_initial_ms = 100
cycle_retry_max_ms = 100
admin_socket = $WORK/admin.sock
database = $CONNINFO
packages = ${a1}pkg" >"$a1.conf"
  q 'DROP TABLE IF EXISTS pga1_order' >/dev/null
  q 'DROP TABLE IF EXISTS pga1_memo' >/dev/null
  q 'DROP TABLE IF EXISTS workflow_instance' >/dev/null
  "$BIN" --config "$a1.conf" >out.txt 2>err.txt &
  pid=$!
  cases=$((cases + 1))
  wait_q "SELECT count(*) FROM information_schema.tables WHERE table_name = 'pga1_memo'" 1 ||
    fail "$a1name: the tables" "stderr [$(cat err.txt)]"
  "$ADMIN" admin.sock 'inject {"template":"order","slots":{"wf-id":{"sym":"w1"},"status":{"sym":"new"}}}\n' >inject.txt 2>&1
  wait_q "SELECT status FROM pga1_order WHERE wf_id = 'w1'" new ||
    fail "$a1name: w1 written" "stderr [$(cat err.txt)]"
  q "UPDATE pga1_order SET status = 'poison' WHERE wf_id = 'w1'; INSERT INTO pga1_memo VALUES ('w1', 'old')" >/dev/null
  "$ADMIN" admin.sock 'inject {"template":"go","slots":{"wf-id":{"sym":"w1"},"act":{"sym":"fail"}}}\n' >>inject.txt 2>&1
  iso="clpdaemon: sync: instance 'w1' is no longer synchronized: $a1msg (3 rolled-back cycles in a row)"
  cases=$((cases + 1))
  wait_has err.txt "$iso" || fail "$a1name: w1 isolated" "stderr [$(cat err.txt)]"
  "$ADMIN" admin.sock 'inject {"template":"order","slots":{"wf-id":{"sym":"w2"},"status":{"sym":"new"}}}\n' >>inject.txt 2>&1
  cases=$((cases + 1))
  n=0
  while [ "$(q "SELECT status FROM pga1_order WHERE wf_id = 'w2'")" != new ]; do
    n=$((n + 1))
    [ "$n" -gt 40 ] && { fail "$a1name: w2 written within 2 s" "stderr [$(cat err.txt)]"; break; }
    sleep 0.05
  done
  stop_daemon "$a1name"
  cases=$((cases + 1))
  fails=$(grep -c "^clpdaemon: cycle transaction failed: $a1msg; retrying in 100 ms\$" err.txt)
  [ "$fails" = 3 ] && [ "$(grep -cxF "$iso" err.txt)" = 1 ] &&
    grep -qxF "clpdaemon: rehydrate: instance 'w1' is not reloaded: it is no longer synchronized" err.txt &&
    [ "$(cat inject.txt)" = "OK queued${NL}OK queued${NL}OK queued" ] ||
    fail "$a1name: the log" "failures $fails, inject [$(cat inject.txt)], stderr [$(cat err.txt)]"
  cases=$((cases + 1))
  got=$(q "SELECT wf_id || '|' || status FROM pga1_order ORDER BY wf_id; SELECT wf_id || '|' || body FROM pga1_memo")
  [ "$got" = "w1|poison${NL}w2|new${NL}w1|old" ] || fail "$a1name: the stored rows" "got [$got]"
  q 'DROP TABLE IF EXISTS pga1_order' >/dev/null
  q 'DROP TABLE IF EXISTS pga1_memo' >/dev/null
  q 'DROP TABLE IF EXISTS workflow_instance' >/dev/null
}
pg_a1 a1n "pg a1 nextval" '(db-nextval missing_no)' \
  'relation "public.pga1_missing_seq" does not exist (SQLSTATE 42P01)'
pg_a1 a1i "pg a1 insert" '(db-insert memo wf-id ?w text "x")' \
  'duplicate key value violates unique constraint "pga1_memo_pkey" (SQLSTATE 23505)'

# T10b against an audit trigger on a table that is not owned, created
# beforehand: one U for two modifies, nothing for a modify back to the
# stored values, D then I for a key change; nil in a null nil column is
# NULL, in a column of another null policy the text nil.
q 'DROP TABLE IF EXISTS pgau_order' >/dev/null
q 'DROP TABLE IF EXISTS pgau_audit' >/dev/null
q 'CREATE TABLE pgau_order (wf_id TEXT NOT NULL, line_no BIGINT NOT NULL, status TEXT, a TEXT, b TEXT, PRIMARY KEY (wf_id, line_no))' >/dev/null
q 'CREATE TABLE pgau_audit (seq SERIAL PRIMARY KEY, op TEXT, wf_id TEXT, line_no BIGINT, status TEXT, a TEXT, b TEXT)' >/dev/null
q "CREATE OR REPLACE FUNCTION pgau_trg() RETURNS trigger LANGUAGE plpgsql AS \$\$ BEGIN IF TG_OP = 'DELETE' THEN INSERT INTO pgau_audit (op, wf_id, line_no, status, a, b) VALUES ('D', OLD.wf_id, OLD.line_no, OLD.status, OLD.a, OLD.b); RETURN OLD; END IF; INSERT INTO pgau_audit (op, wf_id, line_no, status, a, b) VALUES (left(TG_OP, 1), NEW.wf_id, NEW.line_no, NEW.status, NEW.a, NEW.b); RETURN NEW; END \$\$" >/dev/null
q 'CREATE TRIGGER pgau_t AFTER INSERT OR UPDATE OR DELETE ON pgau_order FOR EACH ROW EXECUTE FUNCTION pgau_trg()' >/dev/null
mkdir aupkg
printf '%s\n' 'definition = pgau
version = 1
files = a.clp
orm = orm.conf
ingest = go, order' >aupkg/package.conf
printf '%s\n' '(deftemplate go (slot wf-id) (slot act (type SYMBOL)))
(deftemplate order (slot wf-id) (slot line-no (type INTEGER)) (slot status (type SYMBOL)) (slot a (type SYMBOL)) (slot b (type SYMBOL)))
(defrule pay (go (wf-id ?w) (act pay)) ?o <- (order (wf-id ?w) (status new)) => (modify ?o (status paid)))
(defrule ship ?o <- (order (wf-id ?w) (status paid)) => (modify ?o (status shipped)))
(defrule flip ?g <- (go (wf-id ?w) (act flip)) ?o <- (order (wf-id ?w) (status shipped)) => (retract ?g) (modify ?o (status tmp)))
(defrule flop ?o <- (order (wf-id ?w) (status tmp)) => (modify ?o (status shipped)))
(defrule rekey (go (wf-id ?w) (act rekey)) ?o <- (order (wf-id ?w) (line-no 1)) => (modify ?o (line-no 2)))' >aupkg/a.clp
printf '%s\n' 'order.table = pgau_order
order.owned = no
order.key = line-no
order.slot.wf-id = wf_id text symbol error
order.slot.line-no = line_no bigint integer error
order.slot.status = status text symbol default
order.slot.a = a text symbol nil
order.slot.b = b text symbol default' >aupkg/orm.conf
printf '%s\n' "node_id = n1
tick_interval_ms = 60000
cycle_min_interval_ms = 10
admin_socket = $WORK/admin.sock
database = $CONNINFO
packages = aupkg" >au.conf
AUDIT="SELECT op || '|' || wf_id || '|' || line_no || '|' || status || '|' || coalesce(a, 'NULL') || '|' || coalesce(b, 'NULL') FROM pgau_audit ORDER BY seq"
# au_step <inject> <cycles>: one injected fact, then waits for that cycle
# count in the admin status.
au_step() {
  "$ADMIN" admin.sock "inject $1\\n" >>inject.txt 2>&1
  n=0
  while ! "$ADMIN" admin.sock 'status\n' 2>/dev/null | grep -q " cycles=$2 "; do
    n=$((n + 1))
    [ "$n" -gt 200 ] && return 1
    sleep 0.05
  done
}
: >inject.txt
"$BIN" --config au.conf >out.txt 2>err.txt &
pid=$!
ok=1
n=0
while ! "$ADMIN" admin.sock 'status\n' 2>/dev/null | grep -q ' cycles=1 '; do
  n=$((n + 1))
  [ "$n" -gt 200 ] && { ok=0; break; }
  sleep 0.05
done
au_step '{"template":"order","slots":{"wf-id":{"sym":"w1"},"line-no":1,"status":{"sym":"new"},"a":{"sym":"nil"},"b":{"sym":"nil"}}}' 2 || ok=0
au1=$(q "$AUDIT")
au_step '{"template":"go","slots":{"wf-id":{"sym":"w1"},"act":{"sym":"pay"}}}' 3 || ok=0
au2=$(q "$AUDIT")
au_step '{"template":"go","slots":{"wf-id":{"sym":"w1"},"act":{"sym":"flip"}}}' 4 || ok=0
au3=$(q "$AUDIT")
au_step '{"template":"go","slots":{"wf-id":{"sym":"w1"},"act":{"sym":"rekey"}}}' 5 || ok=0
au4=$(q "$AUDIT")
stop_daemon "pg audit"
AU1='I|w1|1|new|NULL|nil'
AU2="$AU1${NL}U|w1|1|shipped|NULL|nil"
AU4="$AU2${NL}D|w1|1|shipped|NULL|nil${NL}I|w1|2|shipped|NULL|nil"
cases=$((cases + 1))
[ "$ok" = 1 ] && [ "$au1" = "$AU1" ] && [ "$au2" = "$AU2" ] && [ "$au3" = "$AU2" ] && [ "$au4" = "$AU4" ] &&
  [ "$(cat err.txt)" = "clpdaemon: n1: running${NL}clpdaemon: received SIGTERM, stopping" ] ||
  fail "pg audit: the trigger rows" "cycles seen $ok, after the insert [$au1], two modifies [$au2], a modify back [$au3], a key change [$au4], inject [$(cat inject.txt)], stderr [$(cat err.txt)]"
cases=$((cases + 1))
got=$(q "SELECT wf_id || '|' || line_no || '|' || status || '|' || coalesce(a, 'NULL') || '|' || coalesce(b, 'NULL') FROM pgau_order")
[ "$got" = 'w1|2|shipped|NULL|nil' ] || fail "pg audit: the stored row" "got [$got]"
q 'DROP TABLE IF EXISTS pgau_order' >/dev/null
q 'DROP TABLE IF EXISTS pgau_audit' >/dev/null
q 'DROP FUNCTION IF EXISTS pgau_trg()' >/dev/null
q 'DROP TABLE IF EXISTS workflow_instance' >/dev/null

printf '%d/%d PostgreSQL CLI cases passed\n' $((cases - failures)) "$cases"
[ "$failures" -eq 0 ]
