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

### Timeouts

Every PostgreSQL call runs on the engine thread, so the backend bounds them by default:

| Setting | Default | When it applies | Override |
|---|---|---|---|
| `connect_timeout` | 10 s | the connection string has no `connect_timeout` and `PGCONNECT_TIMEOUT` is not set | `connect_timeout=<seconds>` in the connection string (`?connect_timeout=<seconds>` in a URI), or `PGCONNECT_TIMEOUT` |
| `lock_timeout` | 5 s | the session value after connecting is `0` (PostgreSQL's default: no timeout) | `options='-c lock_timeout=<value>'` in the connection string, or `ALTER ROLE ... SET` / `ALTER DATABASE ... SET lock_timeout` |
| `statement_timeout` | 30 s | the session value after connecting is `0` | `options='-c statement_timeout=<value>'`, or a role or database setting |

So a server that does not answer cannot hold the startup open (which runs while the stop signals are blocked) for the TCP timeout, and a lock another writer holds cannot stop the engine thread. A non-zero value from the connection string, the role or the database is kept as it is; for `lock_timeout` and `statement_timeout` a value of `0` cannot turn the timeout off (use a large value instead). `connect_timeout` is different: libpq reads `connect_timeout=0` (or `PGCONNECT_TIMEOUT=0`) as "wait forever", and the daemon keeps it, so avoid it. A `connect_timeout` set only in a `pg_service.conf` service entry is not seen and the 10 s default applies; put it in the connection string or in `PGCONNECT_TIMEOUT` instead. SQLite has none of these (its busy timeout is 5 s, see below).

A lock or statement timeout is `CLPD_DB_TRANSIENT` (T10f, [status](#status)) with the server's one-line message, for example `canceling statement due to lock timeout (SQLSTATE 55P03)` or `canceling statement due to statement timeout (SQLSTATE 57014)`; the connection stays usable, and inside a transaction the transaction is aborted like after any other error (roll it back). A connection that times out fails to open like any unreachable server (`cannot connect to PostgreSQL: ... timeout expired`). Retrying belongs to the caller (FR-26): inside a cycle, the [cycle transaction](#cycle-transaction) retries.

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

## Cycle transaction

With a `database`, every cycle of the running daemon is one transaction (T10a, D7, D8, NFR-3): it is opened before the facts queued for ingestion are asserted ([`ingest.md`](ingest.md)) and committed after `Run` and after the [automatic synchronization](orm.md#automatic-synchronization) has written the changes of the mapped facts (T10b), before the plugins' `cycle_end` ([`plugins.md`](plugins.md)). Every statement of the cycle (the rules' [DDL and sequence functions](orm.md#ddl-functions), the synchronization's writes and `workflow_instance` rows, the rules' [DML functions](orm.md#dml-functions) of T10d) runs inside it. Without a `database` the cycle is unchanged.

A cycle is rolled back when one of its statements fails with any status (`CLPD_DB_ERROR`, `CLPD_DB_UNIQUE`, `CLPD_DB_LOST`, `CLPD_DB_TRANSIENT`, a statement or parameter the interface rejects included), when the synchronization finds a value it cannot write ([write rules](orm.md#write-rules)), or when its commit fails. The daemon rolls back at the first failing statement itself, on both backends: SQLite would otherwise keep the statements that succeeded, and PostgreSQL aborts the transaction anyway. Every later statement of that cycle runs nothing and fails with `the transaction was rolled back after an earlier error` (so a rule's function that tries again gets an error, and nothing is written in autocommit). DDL is transactional on both backends: a table a rolled-back cycle created is gone, and a table it dropped is still there with its rows. A PostgreSQL sequence is not transactional: a value `db-nextval` drew in a rolled-back cycle stays used; on SQLite the counter row is an ordinary row and is rolled back with the cycle ([`orm.md`](orm.md#sequence-functions)).

A rolled-back cycle:

- writes one line to the daemon log stream, `clpdaemon: cycle transaction failed: <message>; retrying in <n> ms`, where `<message>` is the first failing statement's (or the commit's) one-line message, never the connection string;
- gives the plugins' `cycle_end` `ok = 0`;
- delays the next cycle by `<n>` ms: `cycle_retry_initial_ms` (default 100) after the first failure, doubled after each further one, never above `cycle_retry_max_ms` (default 30000), indefinitely ([`configuration.md`](configuration.md)), unless the [poison retry](#poison-retry) isolates the instance whose rule keeps failing. Ticks and notifications do not start a cycle sooner (a fact injected meanwhile waits in the queue); the event loop keeps serving signals (`SIGTERM` stops the daemon at once) and the admin socket. A committed cycle resets the wait.

A rolled-back cycle also evicts the instances it touched (every fact carrying their `wf-id`, mapped or not, is retracted), and the retry starts by reloading them from the database ([rehydration](orm.md#rehydration), T10c, FR-26): their mapped facts come back as last committed, their unmapped facts are transient and lost, and so are the facts asserted from the ingest queue in the rolled-back cycle, which are not queued again (durable inputs come through the exchange table, T13). Then the retry is a whole new cycle: the queued facts are drained and `Run` runs again, so the rules fire again on the reloaded facts. The changes the synchronization recorded for instances the cycle did not touch are kept, so the retry writes them again ([`orm.md`](orm.md#failures)). The reload runs before the retry's `begin`, outside the cycle transaction, after the reconnect when the connection was lost; a reload that loses the connection or meets transient contention (`CLPD_DB_TRANSIENT`, T10f: a lock another writer holds, a timeout) fails the attempt like a failed `begin` (the same line and wait, `clpdaemon: rehydrate: ...` before it; the next attempt reloads that instance again), and a reload the database refuses otherwise isolates that instance ([`orm.md`](orm.md#reload-failures-and-the-retry)).

When the transaction cannot even be opened (for example the server went away between cycles), the cycle does not run at all: no fact is drained, no plugin hook runs, the admin `status` cycle count does not grow, and the same line is logged with the same wait; the admin socket's listener is resumed as after every cycle (when it was paused for lack of descriptors, [`admin-socket.md`](admin-socket.md), the failed reconnect may have freed one). After `CLPD_DB_LOST` (from any statement, the begin or the commit) each retry first reopens the connection with the same connection string; a reopen that fails is logged the same way (`<message>` being the connection error) and retried after the next wait.

The reopen is synchronous: it runs on the engine thread, so while it waits for the server nothing else runs, neither signals nor the admin socket, for up to the connect timeout ([timeouts](#timeouts): 10 s by default, and forever with `connect_timeout=0`, which is one more reason to avoid that value). A server that does not answer at all (a black-holed address) therefore holds the daemon for the connect timeout on every retry; a server that refuses the connection answers at once. An asynchronous reconnect is later work.

What is not a database failure commits: a rule's `(halt)` (the rules decided to stop), a `Run` cut by `max_rules_per_cycle` or by a stop signal, a CLIPS evaluation error that is no database status (for example a wrong argument type), and a DDL or DML function that refuses before sending any statement (for example `orm_allow_drop = no`, or `db-insert` on a `sync = auto` template); the changes made so far in the cycle are kept. A DML statement the database refuses (a duplicate key of `db-insert`, a value PostgreSQL cannot read) rolls the cycle back like any other, and the instances the DML functions wrote in the cycle are evicted and reloaded with the ones whose facts changed ([`orm.md`](orm.md#dml-functions)). `cycle_end` still gets `ok = 0` for an evaluation error, as before.

### Poison retry

A cycle that fails the same way on every retry would hold every instance back forever: for example a rule that touches its instance and then fails on a database statement (`db-nextval` of a missing sequence, a `db-insert` of a key that exists), so that the rollback reloads the instance and its stored rows activate the failing rule again on the retry (T10e, L97, user decision "Aislar la instancia"). The daemon therefore attributes a rolled-back cycle whose **first failure is a database error of a rule's statement** (any of the rules' [DDL, sequence](orm.md#ddl-functions) or [DML functions](orm.md#dml-functions), from the actions of that rule or a function it calls) to the instance of the rule that was firing: the `wf-id` of the first fact of the rule's basis that has one (the symbol `nil` is none), the rule's `wf_id` of the [trace](trace.md). Each instance counts the rolled-back cycles attributed to it; when one reaches `instance_isolate_after` (default 3, [`configuration.md`](configuration.md)) the instance is isolated exactly like the instance of a [write that failed for good](orm.md#failures): after the cycle transaction line, `clpdaemon: sync: instance '<wf-id>' is no longer synchronized: <message> (<n> rolled-back cycles in a row)` (`<message>` the cycle's first failure, `<n>` the count; the line always ends with that suffix: a `<message>` too long for the line is cut, never the suffix, T10f), then `clpdaemon: rehydrate: instance '<wf-id>' is not reloaded: it is no longer synchronized`; its facts are retracted and never reloaded, its later facts are never written, so its rules do not run on it again until the daemon restarts. The next retry then commits the other instances. For example, with the defaults on SQLite:

```
clpdaemon: db-nextval: sequence alias 'missing_no': sequence 'a1_missing_seq': no such table: clpd_sequence
[PRCCODE4] Execution halted during the actions of defrule 'poison'.
clpdaemon: cycle transaction failed: no such table: clpd_sequence; retrying in 100 ms
clpdaemon: sync: instance 'w1' is no longer synchronized: no such table: clpd_sequence (3 rolled-back cycles in a row)
clpdaemon: rehydrate: instance 'w1' is not reloaded: it is no longer synchronized
```

The counts:

- The counts are per instance: a cycle attributed to another instance in between does not reset them; a committed cycle resets every count.
- Not counted, for any instance: a first failure that lost the connection (`CLPD_DB_LOST`: the server, not the instance, is at fault) or that is transient contention (`CLPD_DB_TRANSIENT`, T10f: a lock another writer holds past `lock_timeout` or SQLite's busy timeout, a deadlock, a serialization failure, a `statement_timeout`; another writer or the load, not the instance, is at fault), a failed `begin`, reload or commit, a write of the [automatic synchronization](orm.md#failures) (it isolates its own instance at once), a value it cannot write, and a rule whose basis has no fact with a `wf-id`.
- A cycle attributed to an instance that is already isolated is not counted again (its facts are evicted as any touched instance's).
- A cycle whose first failure is `CLPD_DB_LOST` or `CLPD_DB_TRANSIENT` does not reset the counts either (only a commit does): with `instance_isolate_after = 2`, an attributed failure, a lock timeout, then another attributed failure isolate the instance. Contention never isolates anything: not through the count, not through a write of the [automatic synchronization](orm.md#failures), not through a [reload](orm.md#reload-failures-and-the-retry); the cycle is retried after the backoff for as long as it lasts (the lock is released, the deadlock victim retries).

A failure that is attributed to no instance keeps the plain retry of the cycle transaction: the cycle is retried after the backoff **indefinitely**, until it commits (for example once the server is back, or once an operator created the missing table). The rule `wf-id` is the instance's text as `workflow_instance` keys it (a number in decimal, a float with 17 significant digits).

Inside the cycle transaction `db-nextval` cannot look a missing sequence up after its statement failed, so it reports the database's message (`relation "public.<name>" does not exist (SQLSTATE 42P01)`, SQLite `no such table: clpd_sequence`) instead of `does not exist`.

## workflow_instance

When a `database` is configured and at least one package has an ORM mapping (`orm` in its manifest), the daemon creates the table `workflow_instance` at startup, after the packages load and before the admin socket, with `CREATE TABLE IF NOT EXISTS`; on PostgreSQL in `current_schema()`, like the [DDL functions](orm.md#ddl-functions). An existing table is kept, rows included, but only when its columns are exactly the daemon's (below, T11a): otherwise the daemon refuses to start ([shape check](#shape-check)). It holds one row per workflow instance (S10), written by the [automatic synchronization](orm.md#workflow_instance-rows) (T10b): inserted by the first cycle that writes a row of the instance, its `updated_at` set by every such cycle; the [lifecycle functions](orm.md#lifecycle-functions) (T11a) set its final `status` and `reason`. `--check` never creates it nor checks it (it writes nothing), and a daemon without a mapping does neither.

| Column | PostgreSQL | SQLite | NULL | Meaning |
|---|---|---|---|---|
| `wf_id` | `TEXT` | `TEXT` | `NOT NULL`, the primary key | the instance's `wf-id` |
| `definition` | `TEXT` | `TEXT` | `NOT NULL` | workflow definition of its package |
| `version` | `TEXT` | `TEXT` | `NOT NULL` | version of its package |
| `status` | `TEXT` | `TEXT` | `NOT NULL` | `active` when inserted (T10b); `completed` after `wf-complete`, `failed` after `wf-fail` ([lifecycle functions](orm.md#lifecycle-functions), T11a) |
| `reason` | `TEXT` | `TEXT` | nullable | NULL, except for `failed`: the `?reason` of `wf-fail` (T11a) |
| `parent_wf_id` | `TEXT` | `TEXT` | nullable | parent instance of a subflow (T24) |
| `owner_node` | `TEXT` | `TEXT` | nullable | node holding the lease (T21) |
| `lease_until` | `TIMESTAMPTZ` | `TEXT` | nullable | end of that lease (T21) |
| `created_at` | `TIMESTAMPTZ` | `TEXT` | `NOT NULL` | first write of the instance |
| `updated_at` | `TIMESTAMPTZ` | `TEXT` | `NOT NULL` | last write of the instance |

No column has a default: the daemon writes every value (T10b; `reason` stays NULL until `wf-fail` writes it). The timestamps come from the database clock: PostgreSQL `now()`, the start of the cycle transaction; SQLite the UTC time as text `YYYY-MM-DDTHH:MM:SS.mmmZ` (for example `2026-11-19T17:51:10.960Z`), see [`orm.md`](orm.md#workflow_instance-rows). The name `workflow_instance` is reserved: no ORM mapping may name a table or a sequence so, on any backend ([`orm.md`](orm.md#messages)).

| Situation | Message (stderr), then exit 1 after the plugins' `shutdown` |
|---|---|
| The table cannot be created | `clpdaemon: cannot create the workflow_instance table: <message>` (for example `attempt to write a readonly database`, or on PostgreSQL `no current schema (the search_path names no existing schema)`) |
| The existing table does not have the daemon's columns ([shape check](#shape-check)) | `clpdaemon: the workflow_instance table does not match the daemon's: <difference>` |
| The catalog query of the shape check fails | `clpdaemon: cannot check the workflow_instance table: <message>` |

### Shape check

After the `CREATE TABLE IF NOT EXISTS` the daemon reads the columns of the `workflow_instance` it will write (PostgreSQL: `information_schema.columns` and the primary key constraint of the table in `current_schema()`, so a table of that name in a later `search_path` schema does not count; SQLite: `pragma_table_info`) and refuses to start unless they are exactly the columns of the table above (T11a, closes the T10a review R3-002): every column present, with its type (PostgreSQL `text` or `timestamptz`; SQLite the declared type `TEXT`, in any case) and its nullability, no other column, and the primary key exactly `(wf_id)`. The order of the columns does not matter. `<difference>` is the first difference found, checking the daemon's columns in the order of the table, then the other columns of the existing table, then the primary key:

| Difference | `<difference>` |
|---|---|
| A column is missing (for example a table created before T11a, without `reason`) | `missing column '<column>'` |
| A column of another type | `column '<column>' has type '<type>', expected <TYPE>` (`<type>` as the catalog names it, for example `int8` on PostgreSQL or `BIGINT` on SQLite; `<TYPE>` `TEXT` or `TIMESTAMPTZ`) |
| Nullability | `column '<column>' is nullable, expected NOT NULL`, `column '<column>' is NOT NULL, expected nullable` |
| A column the daemon does not have | `unexpected column '<column>'` |
| No primary key, or another one | `there is no primary key, expected (wf_id)`, `the primary key is (<column>, ...), expected (wf_id)` |

Names and types from the catalog are shown with control characters as `?`, at most 200 bytes. The daemon never alters the table: a `workflow_instance` created by an earlier version (T10, without `reason`) is upgraded by an operator, for example with `ALTER TABLE workflow_instance ADD COLUMN reason TEXT` (the existing rows get NULL, which is right for `active` instances).

## Interface

`src/db.h` (engine thread only): `clpd_db_open`, `clpd_db_close`, `clpd_db_reconnect`, `clpd_db_backend` (`sqlite` or `postgresql`), `clpd_db_exec`, `clpd_db_begin` / `clpd_db_begin_atomic` / `clpd_db_commit` / `clpd_db_rollback`, `clpd_db_in_transaction`, `clpd_db_atomic_failed` (1 when a statement of the open atomic transaction failed, so it is rolled back already), `clpd_db_error`, and the result accessors.

- `clpd_db_begin_atomic` opens an all-or-nothing transaction (the cycle transaction): the first `clpd_db_exec` that fails rolls it back at once on both backends, every later `clpd_db_exec` runs nothing and returns `CLPD_DB_ERROR` with `the transaction was rolled back after an earlier error`, `clpd_db_commit` returns the first failure's status and message, and `clpd_db_rollback` returns `CLPD_DB_OK`. The transaction stays open (`clpd_db_in_transaction` is 1) until one of those two calls, also after a lost connection.
- `clpd_db_reconnect(db, spec, msg, n)` replaces the connection of a handle with a new one opened with the same spec (an open transaction is rolled back, or abandoned when the connection is gone), so everything holding the handle keeps it. On failure `msg` holds the open error (never the spec) and every call on the handle but reconnect, close, backend and error returns `CLPD_DB_LOST` with `not connected to the database` until a reconnect succeeds.

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

`CLPD_DB_OK`, `CLPD_DB_ERROR` (see `clpd_db_error`, one line), `CLPD_DB_UNIQUE` (unique or primary-key violation), `CLPD_DB_LOST` (connection lost; any open transaction is over, and retrying belongs to the caller, FR-26), `CLPD_DB_TRANSIENT` (T10f: contention that a retry of the same work may not meet; the connection stays usable).

| Backend | `CLPD_DB_UNIQUE` | `CLPD_DB_TRANSIENT` | `CLPD_DB_LOST` |
|---|---|---|---|
| PostgreSQL | SQLSTATE `23505` | SQLSTATE `40001` (serialization failure), `40P01` (deadlock detected), `55P03` (lock not available, `lock_timeout`), `57014` (query canceled, `statement_timeout`) | the connection is `CONNECTION_BAD` after the call |
| SQLite | `SQLITE_CONSTRAINT_UNIQUE`, `SQLITE_CONSTRAINT_PRIMARYKEY` | `SQLITE_BUSY`, `SQLITE_LOCKED` and their extended codes (`database is locked`, after the 5 s busy timeout) | never (a failed reconnect only) |

Every other failure is `CLPD_DB_ERROR` (for example PostgreSQL `42P01`, an undefined table). The message of a `CLPD_DB_TRANSIENT` is the backend's, exactly as it was when that failure was `CLPD_DB_ERROR` (for example `canceling statement due to lock timeout (SQLSTATE 55P03)`, `database is locked`). A caller that does not tell the statuses apart treats `CLPD_DB_TRANSIENT` as a failure like `CLPD_DB_ERROR`; inside an atomic transaction it rolls back like any failure and `clpd_db_commit` returns it. The cycle transaction tells it apart: it is retried and never isolates an instance ([poison retry](#poison-retry)). `clpd_db_begin` inside a transaction and `clpd_db_commit` or `clpd_db_rollback` outside one are `CLPD_DB_ERROR`. On PostgreSQL a failed statement aborts the open transaction, so the next `clpd_db_commit` returns `CLPD_DB_ERROR` and nothing in it is kept; SQLite does not abort the transaction on an error, so `clpd_db_commit` there returns `CLPD_DB_OK` and keeps the statements that succeeded.

Out of memory anywhere in the database layer is fatal: the daemon writes `clpdaemon: out of memory` to its log stream (stderr, or the original stderr while the trace captures fds 1 and 2) and exits 1.

## SQLite stand-ins

The SQLite backend accepts PostgreSQL-style DDL type names: `TIMESTAMPTZ` and `JSONB` columns hold TEXT, `BYTEA` holds BLOB. Foreign keys are on, and a busy database is waited for up to 5 s, after which the statement (or the `begin`) fails with `database is locked`, `CLPD_DB_TRANSIENT` ([status](#status)).

Limitation: SQLite gives `TIMESTAMPTZ` and `JSONB` columns NUMERIC affinity, so a numeric-looking string stored there can come back reformatted (`1e2` as `100.0`) and an expression over those columns comes back as a number. SQLite is for tests; the tables the ORM generates use SQLite type names without NUMERIC affinity, and `db-check-schema` rejects a NUMERIC-affinity column there ([`orm.md`](orm.md#sql-types)).

SQLite has no sequences: the ORM's `db-create-sequence` and `db-nextval` keep one row per sequence in the table `clpd_sequence (name TEXT PRIMARY KEY, value INTEGER NOT NULL)`, advanced with `UPDATE ... RETURNING`, which needs SQLite 3.35 or later, and only while the value is an INTEGER ([`orm.md`](orm.md#sequence-functions)). The name `clpd_sequence` is reserved: an ORM mapping cannot name a table so, on any backend.

## Tests

- `clpdaemon_db`, `clpdaemon_db_sqlite_suite`: the interface (out of memory included, through a link seam on the allocators) and the shared backend suite (`tests/db/db_suite.c`) on SQLite. `CLPD_DB_TRANSIENT` (T10f): a second connection's write, `begin` and read against a lock the first holds fail with `database is locked`, while a missing table stays `CLPD_DB_ERROR` and a duplicate key `CLPD_DB_UNIQUE`.
- `clpdaemon_db_pg`: the same suite plus PostgreSQL-only cases (password through `PGPASSFILE`, type mapping, `CLPD_DB_LOST` after the backend is terminated, the default timeouts and their overrides, a long class-22 value that must not leak, out of memory while decoding a `bytea`; a TCP listener on `127.0.0.1` that never answers stands in for an unreachable server) against a throwaway cluster. `tests/db/pg_cluster.sh` creates it with `initdb` in a private temporary directory, starts it with `pg_ctl` on a Unix socket in that directory only (no TCP), creates the role and database with `psql`, runs the test, and stops and removes the cluster on every exit path. The programs come from `CLPD_PG_BINDIR` when it is set, from `PATH` otherwise; when `initdb`, `pg_ctl` or `psql` is missing the script exits 77 and ctest reports the test as SKIPPED, not failed. T10f: SQLSTATE `40001`, `40P01`, `55P03` and `57014` raised by a PL/pgSQL function are `CLPD_DB_TRANSIENT` (also as the first failure of an atomic transaction, returned by its commit), `23505` stays `CLPD_DB_UNIQUE` and `42P01` `CLPD_DB_ERROR`, each with its usual message; the real lock and statement timeouts are `CLPD_DB_TRANSIENT`. T10g: the neighbours `40000`, `40002`, `55P02`, `57P01` and `23503` stay `CLPD_DB_ERROR`, and in an atomic transaction an `INSERT` that times out waiting for a table another connection has locked is `CLPD_DB_TRANSIENT`, returned by the commit with its message, nothing written.
- `clpdaemon_orm_ddl`, `clpdaemon_orm_ddl_pg`: the ORM DDL and sequence functions ([`orm.md`](orm.md#ddl-functions); two connections drawing from one sequence get distinct values) on a temporary SQLite file and, through the same `tests/db/pg_cluster.sh`, on a throwaway PostgreSQL cluster (SKIPPED without the programs). The PostgreSQL tests share a ctest resource lock, so their clusters never run at the same time.
- `clpdaemon_orm_tx`, `clpdaemon_orm_tx_pg`: the [cycle transaction](#cycle-transaction) and the [`workflow_instance`](#workflow_instance) table on SQLite and on a throwaway PostgreSQL cluster: a rolled-back cycle undoes a create and a drop and leaves every table and row unchanged, the exact failure lines, the backoff sequence (doubling, cap, reset), a failing begin or commit and the reconnect after `CLPD_DB_LOST` (injected through link seams on `clpd_db_begin_atomic`, `clpd_db_commit` and `clpd_db_reconnect`, this test binary only), what commits (`(halt)`, an evaluation error, a refusal), `db-nextval` in a rolled-back cycle, and, through the event loop, the waits between retries and `SIGTERM` during a wait. The atomic transaction and the reconnect are also in the shared suite and in `clpdaemon_db` / `clpdaemon_db_pg`.
- `clpdaemon_orm_sync`, `clpdaemon_orm_sync_pg`: the [automatic synchronization](orm.md#automatic-synchronization), the [loader](orm.md#loading-rows) and the [rehydration](orm.md#rehydration) on SQLite and on a throwaway PostgreSQL cluster, with a real CLIPS environment, two packages of two definitions and the cycle calls (the loader: the null policies, every rejected-row line, the statements, equality and `IS NULL` filters, a failed `SELECT`; the rehydration: touched instances evicted, mapped or not, and reloaded without echo when the retry starts, manual templates included, an untouched instance kept, a drained fact lost, an isolated instance not reloaded, a refused and a lost reload); the rows read back after each cycle (deffacts written by the first cycle, every write rule, one `INSERT` for an assert and its modifies, nothing for an assert and its retract, one `UPDATE` of the changed columns, `DELETE` + `INSERT` for a key change, `DELETE` for a retract), the exact statements and their order (recorded through a link seam on `clpd_db_exec`, this test binary only), the `workflow_instance` rows and their timestamps, manual and unmapped templates never written, suspended asserts and retracts, and the failures (a unique violation from two facts with one key, each value error, an `int4` overflow on PostgreSQL, a failure injected into each statement of a flush, a rule's failure before the flush), each leaving every table unchanged. T10f, with transient and lost failures injected through the `clpd_db_exec` seam: a rule statement failing that way with `instance_isolate_after = 1` isolates nothing and the retry reloads the instance; a transient failure between two attributed ones neither counts nor resets; a sync write failing that way isolates nothing and the retry writes the row; a reload failing that way fails the start (no reconnect) and the next start reloads the instance.
- `clpdaemon_orm_life`, `clpdaemon_orm_life_pg`: the [lifecycle functions](orm.md#lifecycle-functions) and the [shape check](#shape-check) on SQLite and on a throwaway PostgreSQL cluster (T11a), with a real CLIPS environment, two packages of two definitions and the synchronization attached: the caller scope (the top level, a rule of `MAIN`), the pending modify written before the final row (the exact statements and their order, through a link seam on `clpd_db_exec`, this test binary only), `completed` with a NULL `reason` and `failed` with a string or symbol reason, `updated_at` later and `created_at` kept, the facts gone (mapped and unmapped) and the mapped rows kept, a row inserted for an instance without one (a numeric `wf-id` too), a second call refused (also within one rule), an instance of another definition refused, facts asserted later never written, a rolled-back cycle that undoes the final state (the retry reloads the instance and a later call succeeds), a pending value that cannot be written (the cycle rolled back, the instance isolated, a later call refused), a refused lookup and a refused final write (each rolling the cycle back with the exact lines); every refused call leaves every table unchanged. The shape check: the daemon's table, another column order, a missing `reason`, an extra column, each nullability, another type, no and another primary key, SQLite types in any case, a refused catalog query, and on PostgreSQL a wrongly shaped table of a later `search_path` schema ignored.
- `clpdaemon_cli`: opening a SQLite database under `--check` and in a running daemon, the open error, an unreachable PostgreSQL server, and the rejected values; the retry keys, a cycle rolled back by a rule (`cycle_end ok=0`, the exact lines, the table gone), the backoff reset, `SIGTERM` and the admin socket during a wait, `workflow_instance` created by a running daemon and not by `--check`, and a table that cannot be created; the synchronization of a running daemon (deffacts, a rule's insert, a manual template, two modifies as one update, a unique violation rolled back); the rehydration of a running daemon (a rule's modify undone, its unmapped fact and the injected fact lost, the working memory a later rule sees). T10f: another connection holds an exclusive lock on the SQLite file while the daemon must reload an evicted instance: the reloads fail with `database is locked` and are retried, nothing is isolated (with `instance_isolate_after = 2` after an attributed failure), and the instance's row is written once the lock is released. T10g: a duplicate `db-insert`, a reload that fails with `database is locked`, then the duplicate again isolate the instance only after the second duplicate (`(2 rolled-back cycles in a row)`), and another instance is written. T11a: `wf-complete` and `wf-fail` do not exist without a database; a running daemon completes and fails instances (the rows, `reason`, `updated_at`, the facts a later rule counts, a second call's exact line, a later fact never written), and a `workflow_instance` without `reason` is refused at startup (exit 1, nothing written, no admin socket; `--check` does not look at it) and accepted once the column is added.
- `clpdaemon_cli_pg`: a running daemon against the throwaway PostgreSQL cluster (`tests/cli/cli_pg_test.sh` through `tests/db/pg_cluster.sh`, which passes extra arguments on to the program it runs): the synchronized rows and the `workflow_instance` row, then a lost server (its backend terminated, the database refusing connections) while the admin listener is paused for lack of descriptors (`prlimit`; without it the pause is not set up): the reconnect failures are logged, the admin `status` is answered during the backoff, and once the server is back the cycles commit again; then a rolled-back cycle whose instance the retry reloads from PostgreSQL. SKIPPED like the other PostgreSQL tests. T10f: another session holds a lock on a table a rule writes (`lock_timeout` lowered through `options`) for longer than `instance_isolate_after` failures: no isolation line, and the rows are written after the release; and a rule statement whose backend is terminated while it waits for that lock, with `instance_isolate_after = 1`, isolates nothing either. T10g: a lock on a table the automatic synchronization writes (every sync write fails with `55P03`), and an `ACCESS EXCLUSIVE` lock that also blocks the reload `SELECT`: no isolation, the row written after the release; a duplicate `db-insert`, two lock timeouts, then the duplicate again isolate the instance only after the second duplicate (`(2 rolled-back cycles in a row)`). An isolation is counted only on lines starting with `clpdaemon: sync: instance '`. T11a: the same lifecycle run as on SQLite (rows, `reason`, `updated_at`, facts, exact lines), and a `workflow_instance` without `reason` refused at startup.

```sh
CLPD_PG_BINDIR=/usr/lib/postgresql/17/bin ctest --test-dir build -R clpdaemon_db_pg --output-on-failure
```
