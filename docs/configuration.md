# Configuration and command line

## Command line

```
usage: clpdaemon --config <file> [--foreground] [--check]
```

| Option | Meaning |
|---|---|
| `--config <file>` | Configuration file. Required. |
| `--foreground` | Run in the foreground. This is the default (systemd `Type=notify`); the daemon never double-forks. |
| `--check` | Validate the configuration, open the trace files and the database, load and initialize the plugins, load and lint the rule packages, shut the plugins down, and exit. It never creates the admin socket. |
| `--help` | Print the usage line on stdout and exit 0. |

Exit codes: `0` success, `1` invalid configuration or a startup failure (trace files, database, plugins, rule packages, admin socket), `2` command-line error.

With `--check`, the daemon validates the configuration, opens the trace files ([`trace.md`](trace.md)) and the database when `database` is set ([`database.md`](database.md)), loads the plugins and calls their `init` ([`plugins.md`](plugins.md)), loads and lints the rule packages ([`packages.md`](packages.md)), so package files may call plugin functions, then calls the plugins' `shutdown` and exits. Plugin log lines (`clpdaemon: <plugin>: <message>`) go to stderr, also under `--check`. Without `--check`, it does the same, creates the admin socket ([`admin-socket.md`](admin-socket.md)), and then runs until it receives `SIGTERM` or `SIGINT`. It always stays in the foreground, with or without `--foreground`; there is no detached mode (systemd supervises it, T17).

## Running

A cycle runs `Run` with the `max_rules_per_cycle` limit. The first cycle runs at start; then a cycle is due `tick_interval_ms` after the start of the previous cycle (fixed rate), or `cycle_min_interval_ms` after it when a notification arrived in between. When a cycle takes longer than the interval, the next one starts at once; missed ticks are not replayed.

| Signal | Effect |
|---|---|
| `SIGTERM`, `SIGINT` | Graceful stop: a running cycle halts after the current rule firing, then the daemon exits 0. The signals are blocked from the start of startup (before the plugins load), so one received while the daemon is still starting is handled as soon as the event loop starts, before the first cycle (the plugins are shut down as usual). |
| `SIGHUP` | Reopens the trace files ([`trace.md`](trace.md)); the daemon keeps running. One received during startup is handled once the event loop starts. |

Runtime messages (stderr; with a trace file, the original stderr, since fds 1 and 2 are then redirected into the trace, see [`trace.md`](trace.md#process-output-fds-1-and-2)): `clpdaemon: <node_id>: running` once the daemon accepts signals; `clpdaemon: received <SIGTERM|SIGINT>, stopping`; `clpdaemon: received SIGHUP, reopening trace files`, then `clpdaemon: <path>: cannot reopen: <system error>` for a trace file that could not be reopened; `clpdaemon: <path>: cannot write: <system error>` the first time a trace file cannot be written, then `clpdaemon: STDERR: <text>` or `clpdaemon: STDWRN: <text>` for each CLIPS error or warning line while it stays unwritable. `clpdaemon: trace capture failed: <system error>; stdout and stderr go back to the console` if the redirection of fds 1 and 2 stops working; the daemon keeps running. A trace file that cannot be opened at startup prints `clpdaemon: <path>: cannot open: <system error>`. A database that cannot be opened prints `clpdaemon: cannot open the database: <message>` (one line, never the connection string, see [`database.md`](database.md#startup)) and exits 1, before any plugin is loaded. A plugin that cannot be loaded or initialized prints one of the messages of [`plugins.md`](plugins.md#messages) and exits 1, after shutting down the plugins already initialized. An admin socket that cannot be created prints one of the messages of [`admin-socket.md`](admin-socket.md#socket-file) (for example `clpdaemon: <path>: not a socket`) and exits 1 after the plugins' `shutdown`; the socket file is removed on every exit path once it was created. A startup failure prints `clpdaemon: cannot block the stop signals: <system error>`, `clpdaemon: cannot start the event loop: <system error>`, `clpdaemon: cannot create the CLIPS environment`, `clpdaemon: cannot attach the trace to the CLIPS environment` or `clpdaemon: cannot redirect stdout and stderr to the trace: <system error>` and exits 1. A system error while running prints `clpdaemon: event loop failed: <system error>` and exits 1. Facts still queued for ingestion at teardown, also under `--check` and after a startup failure, are dropped with `clpdaemon: <n> queued facts dropped at shutdown` (`1 queued fact` for one), after the plugins' `shutdown` ([`ingest.md`](ingest.md#queue)). Running out of memory prints `clpdaemon: <system error>` and exits 1. All of these go to the original stderr, also while fds 1 and 2 are redirected.

## File format

One `key = value` per line; lines may end in LF or CRLF, and spaces and tabs count as blanks. Blank lines and lines whose first non-blank character is `#` are ignored. Spaces around the key and the value are trimmed. Each key may appear once. Lists are comma-separated; spaces around items are trimmed.

| Key | Type | Default |
|---|---|---|
| `node_id` | 1-64 characters from `[A-Za-z0-9._-]` | required |
| `tick_interval_ms` | positive integer | `5000` |
| `cycle_min_interval_ms` | non-negative integer, not greater than `tick_interval_ms` | `200` |
| `max_rules_per_cycle` | `-1` (unlimited) or a positive integer | `10000` |
| `safety_poll_interval_ms` | positive integer | `5000` |
| `idle_eviction_after` | duration: positive integer followed by `s`, `m` or `h` | `30m` |
| `admin_socket` | path of the admin socket ([`admin-socket.md`](admin-socket.md)); its directory must exist; `none` disables the socket (use `./none` for a file named `none`) | `/run/clpdaemon/admin.sock` |
| `admin_socket_mode` | permission bits of the admin socket: 3 or 4 octal digits (a 4-digit value starts with `0`), no bits for others, e.g. `0600`, `0660` | `0600` |
| `trace_text` | path, empty = disabled ([`trace.md`](trace.md)) | empty |
| `trace_jsonl` | path, empty = disabled ([`trace.md`](trace.md)) | empty |
| `watch` | list of `facts`, `rules` (may be empty): trace events written ([`trace.md`](trace.md#event-sources)) | `facts,rules` |
| `packages` | list of paths | empty |
| `plugins` | list of paths, each containing a `/`, loaded in order ([`plugins.md`](plugins.md)) | empty |
| `database` | connection string: `sqlite:<path>` (tests only) or a PostgreSQL conninfo (`key=value` pairs or a `postgresql://` / `postgres://` URI) without a password ([`database.md`](database.md)); never echoed in messages | absent (no database) |

Integers are decimal and must not exceed 2147483647.

## Messages

All messages go to stderr, except `--check` success and `--help`, which go to stdout. Every validation error in the file is reported, one per line, in file order, followed by the file-level errors; then the program exits 1.

| Situation | Message |
|---|---|
| `--check` success (stdout) | `clpdaemon: <file>: configuration OK` |
| `--config` missing | `clpdaemon: missing required option --config` then the usage line, exit 2 |
| `--config` given twice | `clpdaemon: option --config given more than once` then the usage line, exit 2 |
| `--config` without a value | `clpdaemon: option --config requires a file` then the usage line, exit 2 |
| Unknown option | `clpdaemon: unknown option '<arg>'` then the usage line, exit 2 |
| File cannot be opened | `clpdaemon: <file>: cannot open: <system error>` |
| File cannot be read (for example, a directory, or out of memory) | `clpdaemon: <file>: cannot read: <system error>`; the file-level checks below are skipped |
| Line containing a NUL byte | `clpdaemon: <file>:<line>: line contains a NUL byte`; the line is ignored |
| Line without `=` | `clpdaemon: <file>:<line>: expected 'key = value'` |
| Unknown key | `clpdaemon: <file>:<line>: unknown key '<key>'` |
| Repeated key | `clpdaemon: <file>:<line>: duplicate key '<key>' (first set on line <n>)` |
| Bad `node_id` | `clpdaemon: <file>:<line>: node_id: expected 1-64 characters from [A-Za-z0-9._-], got '<value>'` |
| Bad positive integer | `clpdaemon: <file>:<line>: <key>: expected a positive integer, got '<value>'` |
| Bad non-negative integer | `clpdaemon: <file>:<line>: <key>: expected a non-negative integer, got '<value>'` |
| Bad `max_rules_per_cycle` | `clpdaemon: <file>:<line>: max_rules_per_cycle: expected -1 or a positive integer, got '<value>'` |
| Bad duration | `clpdaemon: <file>:<line>: idle_eviction_after: expected a duration such as 30s, 30m or 2h, got '<value>'` |
| Empty path | `clpdaemon: <file>:<line>: <key>: expected a path` |
| Bad `admin_socket_mode` | `clpdaemon: <file>:<line>: admin_socket_mode: expected an octal mode such as 0600 or 0660, with no permission bits for others, got '<value>'` |
| Unknown watch item | `clpdaemon: <file>:<line>: watch: unknown item '<item>' (expected facts, rules)` |
| Empty list item | `clpdaemon: <file>:<line>: <key>: empty list item` |
| Empty `database` | `clpdaemon: <file>:<line>: database: expected a connection string` |
| `database = sqlite:` without a path (or `sqlite` alone) | `clpdaemon: <file>:<line>: database: expected sqlite:<path>` |
| `database` that libpq cannot parse | `clpdaemon: <file>:<line>: database: invalid PostgreSQL connection string` |
| `database` with `password=`, `sslpassword=` or a password in the URI | `clpdaemon: <file>:<line>: database: the connection string must not contain a password (use a password file, see PGPASSFILE)` |
| `node_id` absent | `clpdaemon: <file>: missing required key 'node_id'` |
| Interval order | `clpdaemon: <file>: cycle_min_interval_ms (<a>) must not exceed tick_interval_ms (<b>)` |

The interval-order check runs only when both values are valid. The `database` messages never show the value, which may name a user or host.
