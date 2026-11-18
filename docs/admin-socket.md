# Admin socket

The daemon listens on a Unix domain stream socket for administration and tests (S4, FR-20, decision D1 "Unix, admin y pruebas"): inject a fact, report status, reopen the trace files. It is a local file only, never a network port (NFR-2), and no command evaluates CLIPS text: `inject` goes through the ingest pipeline and its deftemplate whitelist ([`ingest.md`](ingest.md)), never `Eval` or `Build` (FR-21).

## Socket file

| Key | Default | Meaning |
|---|---|---|
| `admin_socket` | `/run/clpdaemon/admin.sock` | Path of the socket ([`configuration.md`](configuration.md)). |
| `admin_socket_mode` | `0600` | Permission bits of the socket file: 3 or 4 octal digits, owner and group bits only, for example `0600` or `0660`. A client needs write permission to connect. |

The daemon creates the socket after the rule packages load and before the event loop starts, and only when it runs: `--check` validates both keys but never creates, replaces or removes the file. The socket is created with no permission at all (umask `0777` around `bind`) and then set to `admin_socket_mode`, so it is never more accessible than configured, even briefly. The umask is process-wide: a file a plugin thread creates at that very moment gets no permissions. The parent directory must exist (the daemon does not create it); with the default path, the service manager or the operator creates `/run/clpdaemon` (T17).

A socket file already at the path, for example left by a daemon that was killed, is removed and replaced. Any other file type at the path is an error and is left untouched. The socket file is removed when the daemon stops, after a startup failure that happens once it was created, and on every other exit path except when the process is killed by a signal the daemon does not handle (for example `SIGKILL`); a later start then replaces the stale file. If another process replaced the file meanwhile (a second daemon started with the same path replaces a live socket too), the stopping daemon leaves the new file alone.

Startup errors, written to stderr; the daemon then shuts the plugins down and exits 1, like every other startup failure:

| Situation | Message |
|---|---|
| Path of 108 bytes or more (`sun_path` limit) | `clpdaemon: <path>: admin socket path too long (max 107 bytes)` |
| Path exists and is not a socket | `clpdaemon: <path>: not a socket` |
| Old socket cannot be removed | `clpdaemon: <path>: cannot remove the old socket: <system error>` |
| Socket cannot be created, bound (for example, a missing directory), set to its mode or listened on | `clpdaemon: <path>: cannot bind the admin socket: <system error>` |
| Socket cannot be added to the event loop | `clpdaemon: <path>: cannot serve the admin socket: <system error>` |

For example, `clpdaemon: /run/clpdaemon/admin.sock: cannot bind the admin socket: No such file or directory` when `/run/clpdaemon` does not exist.

## Serving

The socket is served on the engine thread through the event loop (NFR-1): commands run between cycles, never during a `Run`, so a long cycle delays the replies. Up to 16 clients are connected at once; a further connection receives `ERR too many clients` and is closed at once. Each client has its own input buffer and its own reply buffer, and the engine thread never blocks on a client: replies are written without waiting, and a client that does not read its replies is disconnected once more than 65536 bytes of them are waiting in the daemon (beyond what the kernel socket buffer holds). A client that ends its side of the connection (EOF) still receives the replies to the lines it sent, then the daemon closes the connection; a read or write error closes it at once. A peer that is gone never kills the daemon (`MSG_NOSIGNAL`, no `SIGPIPE`).

## Protocol

One command per line, ending in LF; a CR before the LF is ignored, so CRLF works too. Each line gets exactly one reply line, in order, starting with `OK` or `ERR`. Several commands may come in one write and one command may be split across writes; a last line without LF when the client disconnects is discarded and never runs.

A line holds at most 65536 bytes before its LF (a CR counts). A longer line gets `ERR line too long` and the connection is closed (the reply is sent if the socket takes it at once). A line containing a NUL byte gets `ERR NUL byte not allowed` and the connection stays open. An empty line gets `ERR empty command`.

The command word is the text before the first space, case-sensitive; what follows that space is the argument.

| Command | Reply | Meaning |
|---|---|---|
| `inject <json>` | `OK queued` | The fact JSON ([`ingest.md`](ingest.md#fact-json)) is valid against the whitelist and the slot constraints and was queued; the next cycle asserts it. Each inject wakes the event loop, so the cycle starts within `cycle_min_interval_ms`, not at the next tick. |
| | `ERR <message>` | The exact rejection message of [`ingest.md`](ingest.md#rejection-messages), for example `ERR template 'nope' is not ingestible` or `ERR invalid JSON: offset 1: unexpected end of input`. Nothing is queued. |
| | `ERR ingest queue full` | The queue shared with the plugins already holds 1024 facts ([`ingest.md`](ingest.md#queue)). |
| `status` | `OK node_id=<id> cycles=<n> queue=<n> facts=<n>` | `cycles`: engine cycles run since the start; `queue`: facts waiting in the ingest queue (from every source); `facts`: facts in the working memory. |
| `reopen` | `OK reopened` | The trace files were reopened (log rotation), the same as `SIGHUP` ([`trace.md`](trace.md)) but without the `clpdaemon: received SIGHUP, reopening trace files` line. With no trace file configured there is nothing to reopen and the reply is `OK reopened`. |
| | `ERR <n> trace files could not be reopened` | That many files kept their previous stream; each one is reported on stderr as `clpdaemon: <path>: cannot reopen: <system error>`. |
| `status <anything>`, `reopen <anything>` | `ERR 'status' takes no arguments`, `ERR 'reopen' takes no arguments` | Also for a trailing space. |
| anything else | `ERR unknown command '<word>'` | `<word>` is shown like a name in an ingest message: control characters become `?`, and a word longer than 64 bytes is cut there and followed by `...`, so a reply cannot forge terminal or log output. |

The socket validates an `inject` when it receives it, so its reply carries the exact error; the fact is asserted at the start of the next cycle. If CLIPS then refuses it anyway, the daemon writes `clpdaemon: admin socket: inject rejected: <message>` to stderr (the daemon log stream).

## Examples

```
$ printf 'inject {"template":"order","slots":{"wf-id":"w1","qty":3}}\nstatus\nreopen\nbogus\n' | socat - UNIX-CONNECT:/run/clpdaemon/admin.sock
OK queued
OK node_id=n1 cycles=42 queue=1 facts=17
OK reopened
ERR unknown command 'bogus'
```

(`nc -U /run/clpdaemon/admin.sock` works as well; the tests use their own client, `tests/admin/admin_client.c`, because neither tool is always installed.)

## Internal interface

`src/admin.h`: `clpd_admin_bind`, `clpd_admin_attach` (registers the socket with the loop's fd API, `clpd_loop_add_fd` in `src/loop.h`), `clpd_admin_detach`, `clpd_admin_close`, and `clpd_admin_command`, which runs one line and is tested on its own (`tests/admin/admin_test.c`).
