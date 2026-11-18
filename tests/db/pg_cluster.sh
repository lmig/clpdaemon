#!/bin/sh
# Runs a PostgreSQL backend test (T8c) against a throwaway cluster.
# usage: pg_cluster.sh <test-binary>
#
# The cluster lives in a private temporary directory: initdb + pg_ctl, a
# Unix socket in that directory only (listen_addresses=''), scram-sha-256
# password authentication for local connections. A role with a password
# gets a 0600 password file exported as PGPASSFILE, and the binary runs as
#   <test-binary> "host=<dir> port=<port> dbname=clpd_test user=clpd_test" \
#     <password>
# (the conninfo has no password). The cluster is stopped and the directory
# removed on every exit path.
#
# The PostgreSQL programs come from CLPD_PG_BINDIR when it is set, from
# PATH otherwise; when initdb, pg_ctl or psql is missing the script exits
# 77 (ctest SKIP_RETURN_CODE: the test is reported SKIPPED).
set -u

U='usage: pg_cluster.sh <test-binary>'
BIN=${1:?$U}
case $BIN in /*) ;; *) BIN=$PWD/$BIN ;; esac

find_tool() {
  if [ -n "${CLPD_PG_BINDIR:-}" ]; then
    if [ -x "$CLPD_PG_BINDIR/$1" ]; then
      printf '%s\n' "$CLPD_PG_BINDIR/$1"
      return 0
    fi
    return 1
  fi
  command -v "$1"
}

for tool in initdb pg_ctl psql; do
  if ! path=$(find_tool "$tool"); then
    echo "pg_cluster.sh: $tool not found (PATH or CLPD_PG_BINDIR); skipped" >&2
    exit 77
  fi
  eval "$tool=\$path"
done
# shellcheck disable=SC2154 # set by the loop above
INITDB=$initdb PG_CTL=$pg_ctl PSQL=$psql

# Nothing from the caller's libpq environment reaches the cluster or the
# test (an inherited PGPASSWORD would defeat the password-file case).
unset PGPASSWORD PGPASSFILE PGHOST PGHOSTADDR PGPORT PGUSER PGDATABASE \
  PGSERVICE PGSERVICEFILE PGOPTIONS PGSSLMODE PGCONNECT_TIMEOUT \
  PGCLIENTENCODING PGTARGETSESSIONATTRS PGDATA PGREQUIRESSL PGAPPNAME

umask 077
WORK=$(mktemp -d) || exit 2
DATA=$WORK/data
PORT=54329
started=0

cleanup() {
  if [ "$started" = 1 ]; then
    if ! "$PG_CTL" -D "$DATA" -m immediate -w -t 30 stop >/dev/null 2>&1 &&
      [ -s "$DATA/postmaster.pid" ]; then
      kill -KILL "$(head -n 1 "$DATA/postmaster.pid")" 2>/dev/null
    fi
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

fail() {
  echo "pg_cluster.sh: $*" >&2
  if [ -s "$WORK/server.log" ]; then
    echo "--- server log (tail) ---" >&2
    tail -n 20 "$WORK/server.log" >&2
  fi
  exit 2
}

# Random passwords: hex, so no quoting is needed in SQL or the passfile.
ADMIN_PW=$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')
TEST_PW=$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')
[ ${#ADMIN_PW} = 32 ] && [ ${#TEST_PW} = 32 ] || fail "cannot generate passwords"

printf '%s\n' "$ADMIN_PW" >"$WORK/admin.pw"
"$INITDB" -D "$DATA" -U clpd_admin --pwfile="$WORK/admin.pw" -E UTF8 \
  --no-locale --auth-local=scram-sha-256 --auth-host=reject \
  >"$WORK/initdb.log" 2>&1 || {
  cat "$WORK/initdb.log" >&2
  fail "initdb failed"
}
cat >>"$DATA/postgresql.conf" <<EOF
listen_addresses = ''
unix_socket_directories = '$WORK'
port = $PORT
password_encryption = 'scram-sha-256'
fsync = off
EOF

cat >"$WORK/pgpass" <<EOF
*:*:*:clpd_admin:$ADMIN_PW
*:*:*:clpd_test:$TEST_PW
EOF
chmod 600 "$WORK/pgpass" || fail "cannot protect the password file"
export PGPASSFILE="$WORK/pgpass"

started=1
"$PG_CTL" -D "$DATA" -l "$WORK/server.log" -w -t 60 start >/dev/null 2>&1 ||
  fail "pg_ctl start failed"

"$PSQL" -X -q -v ON_ERROR_STOP=1 -h "$WORK" -p "$PORT" -U clpd_admin \
  -d postgres >/dev/null 2>"$WORK/psql.err" <<EOF || {
CREATE ROLE clpd_test LOGIN PASSWORD '$TEST_PW';
CREATE DATABASE clpd_test OWNER clpd_test;
EOF
  cat "$WORK/psql.err" >&2
  fail "cannot create the test role and database"
}

"$BIN" "host=$WORK port=$PORT dbname=clpd_test user=clpd_test" "$TEST_PW"
rc=$?
if [ "$rc" != 0 ] && [ -s "$WORK/server.log" ]; then
  echo "--- server log (tail) ---" >&2
  tail -n 20 "$WORK/server.log" >&2
fi
exit "$rc"
