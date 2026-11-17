# Configuration and command line

## Command line

```
usage: clpdaemon --config <file> [--foreground] [--check]
```

| Option | Meaning |
|---|---|
| `--config <file>` | Configuration file. Required. |
| `--foreground` | Run in the foreground. This is the default (systemd `Type=notify`); the daemon never double-forks. |
| `--check` | Validate the configuration and exit. |
| `--help` | Print the usage line on stdout and exit 0. |

Exit codes: `0` success, `1` invalid configuration, `2` command-line error.

Without `--check`, the daemon validates the configuration and then runs until it receives `SIGTERM` or `SIGINT`. It always stays in the foreground, with or without `--foreground`; there is no detached mode (systemd supervises it, T17).

## Running

A cycle runs `Run` with the `max_rules_per_cycle` limit. The first cycle runs at start; then a cycle is due `tick_interval_ms` after the start of the previous cycle (fixed rate), or `cycle_min_interval_ms` after it when a notification arrived in between. When a cycle takes longer than the interval, the next one starts at once; missed ticks are not replayed.

| Signal | Effect |
|---|---|
| `SIGTERM`, `SIGINT` | Graceful stop: a running cycle halts after the current rule firing, then the daemon exits 0. |
| `SIGHUP` | Reopens the trace files (T5); the daemon keeps running. |

Runtime messages (stderr): `clpdaemon: <node_id>: running` once the daemon accepts signals; `clpdaemon: received <SIGTERM|SIGINT>, stopping`; `clpdaemon: received SIGHUP, reopening trace files`. A startup failure prints `clpdaemon: cannot start the event loop: <system error>` or `clpdaemon: cannot create the CLIPS environment` and exits 1. A system error while running prints `clpdaemon: event loop failed: <system error>` and exits 1.

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
| `admin_socket` | path | `/run/clpdaemon/admin.sock` |
| `trace_text` | path, empty = disabled | empty |
| `trace_jsonl` | path, empty = disabled | empty |
| `watch` | list of `facts`, `rules` (may be empty) | `facts,rules` |
| `packages` | list of paths | empty |
| `plugins` | list of paths | empty |

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
| Unknown watch item | `clpdaemon: <file>:<line>: watch: unknown item '<item>' (expected facts, rules)` |
| Empty list item | `clpdaemon: <file>:<line>: <key>: empty list item` |
| `node_id` absent | `clpdaemon: <file>: missing required key 'node_id'` |
| Interval order | `clpdaemon: <file>: cycle_min_interval_ms (<a>) must not exceed tick_interval_ms (<b>)` |

The interval-order check runs only when both values are valid.
