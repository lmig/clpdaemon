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

Prerequisites: CMake >= 3.20 and a C99 compiler (GCC). On the reference development machine the tools come from a user-level Homebrew installation (`/home/linuxbrew/.linuxbrew/bin`).

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

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
