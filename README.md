# clpdaemon

CLIPS 6.4.2 workflow engine daemon. Workflows are coded as CLIPS rule packages, persisted in PostgreSQL, and isolated from external systems through a single exchange table (inbox/outbox). Several engine nodes scale out behind a dispatcher.

Status: specification complete, implementation starting (v1, task T1).

## Documentation

| Document | Content |
|---|---|
| [`docs/requirements.md`](docs/requirements.md) | Requirements specification, architecture, data model, decision record |
| [`docs/exchange-contract.md`](docs/exchange-contract.md) | Contract for connector teams: exchange table, states, payload, rules |
| [`odd/tasks/clpdaemon.md`](odd/tasks/clpdaemon.md) | Phased task plan with status and commit evidence |

## Monorepo layout

Directories are created by the task that introduces them.

| Path | Content | Task |
|---|---|---|
| `vendor/clips/` | CLIPS 6.4.2 core sources, unmodified | T1 |
| `engine/` | Daemon (engine node): event loop, tracing, plugin loader, admin socket, exchange consumer | T1–T15 |
| `plugins/` | C plugins: `example/`, `orm/`, `zmq/` | T6, T8–T10, T14 |
| `packages/` | Rule packages (workflow definitions and samples) | T4, T16 |
| `contracts/` | Exchange table DDL and message JSON Schemas | T12 |
| `dispatcher/` | `clpd-dispatch` | T18–T20 |
| `connectors/` | Connectors to external systems, any language (later phase) | — |
| `deploy/` | systemd units and deployment assets | T17 |
| `tests/` | Integration and end-to-end tests | all |

## Build

Prerequisites: CMake >= 3.20, a C99 compiler (GCC), pkg-config, and the development files of SQLite 3 (`sqlite3.pc`) and libpq (`libpq.pc`), both required. Optional, for the PostgreSQL test `clpdaemon_db_pg`: `initdb`, `pg_ctl` and `psql` in `PATH` or in `CLPD_PG_BINDIR`; without them the test is SKIPPED ([`docs/database.md`](docs/database.md#tests)). On the reference development machine the tools come from a user-level Homebrew installation (`/home/linuxbrew/.linuxbrew/bin`).

```sh
export PKG_CONFIG_PATH=/home/linuxbrew/.linuxbrew/opt/libpq/lib/pkgconfig:/home/linuxbrew/.linuxbrew/lib/pkgconfig
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

`PKG_CONFIG_PATH` is needed only when pkg-config does not find the two libraries on its own: the first entry is the `lib/pkgconfig` directory of a keg-only libpq (Homebrew does not link it into its prefix, so its `libpq.pc` is only there), the second the package manager's shared `lib/pkgconfig` directory, where `sqlite3.pc` lives. Use the equivalent directories of your installation; with system packages (for example `libsqlite3-dev` and `libpq-dev`) it can be left unset. The sanitizer build below needs the same variable.

CLIPS is vendored unmodified under `vendor/clips/` (see its README).

Sanitizer build (ASan + UBSan). Upstream CLIPS reports some undefined behavior at environment creation and destruction; the suppressions file silences it for `vendor/clips` only:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan
UBSAN_OPTIONS=suppressions=$PWD/tests/sanitizers/ubsan-vendor.supp \
  ctest --test-dir build-asan --output-on-failure
```

## Run

```sh
build/src/clpdaemon --config <file> --check
```

Command line, configuration keys, defaults and exact error messages: [`docs/configuration.md`](docs/configuration.md).

Admin socket (inject a test fact, status, reopen the trace files): [`docs/admin-socket.md`](docs/admin-socket.md); fact ingestion: [`docs/ingest.md`](docs/ingest.md).

C plugins (contract, lifecycle, building one): [`docs/plugins.md`](docs/plugins.md); the example plugin builds as `build/plugins/clpd_example.so`.

Database (the `database` key, PostgreSQL and the test-only SQLite backend, password files, the PostgreSQL test): [`docs/database.md`](docs/database.md).
