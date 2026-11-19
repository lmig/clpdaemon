# Database

The daemon keeps workflow state in a relational database (S7, decision D6 "PostgreSQL"). PostgreSQL is the production backend; SQLite stands in for it in tests only. Both sit behind one interface, `src/db.h`, with the same calls and semantics; the ORM (T9, T10) builds on it. With no `database` key the daemon runs without a database (and without the ORM).

## Configuration

| Key | Default | Meaning |
|---|---|---|
| `database` | absent (no database) | Connection string: `sqlite:<path>` (tests only) or a PostgreSQL connection string, without a password ([`configuration.md`](configuration.md)). |

Accepted forms:

| Form | Example |
|---|---|
| `sqlite:<path>` (tests only; the path must not be empty, a relative path is relative to the daemon's working directory, the file is created when missing) | `database = sqlite:/var/tmp/clpd-test.db` |
| libpq `key=value` conninfo | `database = host=/run/postgresql dbname=clpd user=clpd` |
| libpq URI, `postgresql://` or `postgres://` | `database = postgresql://clpd@db.example.org/clpd?sslmode=verify-full` |

The PostgreSQL forms are parsed with libpq's own parser (`PQconninfoParse`), so every libpq option is accepted. libpq also reads its usual environment variables (`PGHOST`, `PGPORT`, `PGSSLMODE`, ...) for options the string leaves out.

### Password

The connection string must not contain a password: a `password=` or `sslpassword=` option, or a password in the URI (`postgresql://user:secret@host/db` or `?password=...`), is rejected when the configuration loads. A `dbname` value that is itself a connection string (it contains `=` or starts with `postgresql://` or `postgres://`) is checked the same way, up to 8 levels deep; a nested value that libpq cannot parse, or a deeper one, is an invalid connection string. Supply the password through a password file: `~/.pgpass`, or the file named by `PGPASSFILE` (mode `0600`, one `host:port:database:user:password` line per server; see the libpq documentation). The configuration file can then be readable by operators without exposing a credential. (libpq would also take `PGPASSWORD` from the environment; do not use it, since the environment of a process is visible to other tools.)

### Never echoed

The value of `database` is never written to stderr, the trace files or the admin socket, not even in an error. A configuration error names the file and line only. A connection error shows the message of the backend, which may contain the socket path, the host name, the user name or an invalid option value (for example `cannot connect to PostgreSQL: invalid sslmode value: "bogus"`), never a password, since the string cannot hold one. A SQLite open error does not show the path.

## Startup

The daemon opens the database after the configuration loads and before the plugins are loaded and initialized ([`plugins.md`](plugins.md)), and `--check` opens it too, so `--check` reports a database the daemon could not reach. A SQLite file that does not exist is created, also under `--check`. The connection is closed when the daemon exits, after the plugins' `shutdown` and the CLIPS environment are gone, and on every startup failure after it was opened.

| Situation | Message (stderr), then exit 1 |
|---|---|
| Invalid `database` value | one of the configuration messages below, `clpdaemon: <file>:<line>: database: ...` |
| The database cannot be opened | `clpdaemon: cannot open the database: <message>` |

`<message>` is one line, for example:

- `cannot open the SQLite database: unable to open database file` (missing directory, no permission)
- `cannot open the SQLite database: file is not a database`
- `cannot connect to PostgreSQL: connection to server on socket "/run/postgresql/.s.PGSQL.5432" failed: No such file or directory`

Configuration messages (also in [`configuration.md`](configuration.md#messages)):

| Situation | Message |
|---|---|
| Empty value | `clpdaemon: <file>:<line>: database: expected a connection string` |
| `sqlite:` without a path, or `sqlite` alone | `clpdaemon: <file>:<line>: database: expected sqlite:<path>` |
| libpq cannot parse it | `clpdaemon: <file>:<line>: database: invalid PostgreSQL connection string` |
| A password in it | `clpdaemon: <file>:<line>: database: the connection string must not contain a password (use a password file, see PGPASSFILE)` |

## Interface

`src/db.h` (engine thread only): `clpd_db_open`, `clpd_db_close`, `clpd_db_backend` (`sqlite` or `postgresql`), `clpd_db_exec`, `clpd_db_begin` / `clpd_db_commit` / `clpd_db_rollback`, `clpd_db_error`, and the result accessors.

### Statements and parameters

Parameterized statements only (NFR-2): every value goes through a parameter, never into the SQL text. A statement is one SQL statement (one trailing `;` allowed) whose parameters are written exactly `$1..$n` for `n` parameters, on both backends; each number is used at least once (`$1` may repeat). Any other placeholder (`?`, `?1`, `:name`, `@name`, `$name`, `$0`, `$01`), a number above `n`, a missing number or a second statement is an error and nothing runs. Quotes (`'...'`, `E'...'`, `"..."`) and comments are skipped by that check. On SQLite a parameter must stand alone (`$1::text` is an error there). Transaction-control SQL (`BEGIN`, `COMMIT`, ...) goes through the transaction calls, not `clpd_db_exec`.

### Value types

| Type | C | PostgreSQL column types read as it | SQLite |
|---|---|---|---|
| NULL | — | any NULL | NULL |
| INTEGER | `int64_t` | `int2`, `int4`, `int8`, `bool` (0/1) | INTEGER |
| FLOAT | `double` (not NaN as a parameter) | `float4`, `float8` | REAL |
| TEXT | UTF-8 bytes without NUL | everything else (`numeric`, `timestamptz`, `jsonb`, ...) | TEXT |
| BLOB | bytes, NUL included | `bytea` | BLOB |

An INTEGER parameter is sent to PostgreSQL as `int8`: a function that takes `int4` needs `$1::int4`.

### Status

`CLPD_DB_OK`, `CLPD_DB_ERROR` (see `clpd_db_error`, one line), `CLPD_DB_UNIQUE` (unique or primary-key violation), `CLPD_DB_LOST` (connection lost; any open transaction is over, and retrying belongs to the caller, FR-26). `clpd_db_begin` inside a transaction and `clpd_db_commit` or `clpd_db_rollback` outside one are `CLPD_DB_ERROR`. On PostgreSQL a failed statement aborts the open transaction, so the next `clpd_db_commit` returns `CLPD_DB_ERROR` and nothing in it is kept; SQLite does not abort the transaction on an error, so `clpd_db_commit` there returns `CLPD_DB_OK` and keeps the statements that succeeded.

## SQLite stand-ins

The SQLite backend accepts PostgreSQL-style DDL type names: `TIMESTAMPTZ` and `JSONB` columns hold TEXT, `BYTEA` holds BLOB. Foreign keys are on, and a busy database is waited for up to 5 s.

Limitation: SQLite gives `TIMESTAMPTZ` and `JSONB` columns NUMERIC affinity, so a numeric-looking string stored there can come back reformatted (`1e2` as `100.0`) and an expression over those columns comes back as a number. SQLite is for tests; per-backend DDL type names belong to the ORM (T9).

## Tests

- `clpdaemon_db`, `clpdaemon_db_sqlite_suite`: the interface and the shared backend suite (`tests/db/db_suite.c`) on SQLite.
- `clpdaemon_db_pg`: the same suite plus PostgreSQL-only cases (password through `PGPASSFILE`, type mapping, `CLPD_DB_LOST` after the backend is terminated) against a throwaway cluster. `tests/db/pg_cluster.sh` creates it with `initdb` in a private temporary directory, starts it with `pg_ctl` on a Unix socket in that directory only (no TCP), creates the role and database with `psql`, runs the test, and stops and removes the cluster on every exit path. The programs come from `CLPD_PG_BINDIR` when it is set, from `PATH` otherwise; when `initdb`, `pg_ctl` or `psql` is missing the script exits 77 and ctest reports the test as SKIPPED, not failed.
- `clpdaemon_cli`: opening a SQLite database under `--check` and in a running daemon, the open error, an unreachable PostgreSQL server, and the rejected values.

```sh
CLPD_PG_BINDIR=/usr/lib/postgresql/17/bin ctest --test-dir build -R clpdaemon_db_pg --output-on-failure
```
